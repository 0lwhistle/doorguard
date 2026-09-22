/*
 * ntp_service.c — NTP 服务实现(应用内 SNTP,跑在 netcore 统一事件循环)
 *
 * 2026-09-22 起替代 chrony 方案:rootfs 不再需要 chrony 常驻(两个东西同时
 * 调系统时间会打架),校正 = 向 ntp_server 发 SNTP 请求 → settimeofday 步进。
 * 与 chrony 持续 slew 的差异:SNTP 是一次步进,时间回拨在门禁场景可接受
 * (access_logs 的展示顺序以落库 ts 为准,记录在 spec-network §3)。
 *
 * 触发语义与旧版一致:
 *   - 开机自动一次(延迟 10s 等网络栈就绪)
 *   - EV_NET_NTP_TRIGGER(菜单按钮)
 *   - web /api/ntp(202 异步,结果走 WebSocket)
 * 联网探测不再 shell 出去 ping,直接用 net_info 的在线状态。
 *
 * 线程契约:SNTP 连接/超时判定都在 netcore loop 线程(经 netcore_post 进入),
 * 业务线程只调用 trigger(立即返回)——chronyc 秒级阻塞 + 每次触发 spawn
 * 线程的旧实现整体退役。
 */
#include "ntp_service.h"
#include "net_info.h"
#include "netcore.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[NTP]";

#define SNTP_TIMEOUT_MS  8000        /* 单次校正超时(含 DNS/往返) */
#define BOOT_DELAY_S     10          /* 开机后等网络栈就绪 */

static atomic_bool s_running = false;
/* loop 线程私有的在途请求 */
static struct mg_connection *s_sntp_c = NULL;
static int64_t s_deadline_ms = 0;
static bool s_attempt_done = false;   /* 本轮是否已发布结果(防重复/防挂住) */

static void publish(bool ok, int err, int64_t ts)
{
    ev_ntp_result_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.ok = ok;
    ev.err = err;
    ev.synced_ts = ts;
    EVENT_BUS_PUBLISH(EV_NET_NTP_RESULT, &ev);
}

/* 每轮校正只发一次结果:成功/失败/超时/异常关闭都经此收口。
 * 少了它曾经挂住:sntp_running 靠结果事件复位,连接静默关闭(如服务端
 * 回 kiss-of-death)时永不复位,web 侧后续触发一直 409 */
static void publish_attempt(bool ok, int err, int64_t ts)
{
    s_attempt_done = true;
    publish(ok, err, ts);
}

/* ---- loop 线程:SNTP 请求与超时 ---- */

static void sntp_cb(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_SNTP_TIME) {
        uint64_t epoch_ms = *(uint64_t *)ev_data;
        struct timeval tv;
        tv.tv_sec = (time_t)(epoch_ms / 1000);
        tv.tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000);
        bool set_ok = (settimeofday(&tv, NULL) == 0);
        if (!set_ok)
            DG_LOGW(TAG, "settimeofday 失败(需要 root?),时间未写入");
        else
            DG_LOGI(TAG, "SNTP 同步成功:%lld", (long long)tv.tv_sec);
        s_sntp_c = NULL;
        /* 事件回调内禁止 mg_close_conn(立即 free,poll 循环继续用 c 即 UAF,
         * 板上实测段错误);is_closing 由 poll 末尾统一延迟关闭 */
        c->is_closing = 1;
        publish_attempt(set_ok, set_ok ? DG_OK : DG_ERR_IO,
                        set_ok ? (int64_t)tv.tv_sec : 0);
    } else if (ev == MG_EV_ERROR) {
        /* DNS 解析失败 / 网络不可达:mongoose 经此事件报告并关闭连接 */
        DG_LOGW(TAG, "SNTP 失败: %s", (const char *)ev_data);
        if (s_sntp_c == c) {
            s_sntp_c = NULL;
            publish_attempt(false, DG_ERR_NETWORK, 0);
        }
    } else if (ev == MG_EV_POLL) {
        if (s_sntp_c == c && (int64_t)mg_millis() > s_deadline_ms) {
            DG_LOGW(TAG, "SNTP 超时(%d ms)", SNTP_TIMEOUT_MS);
            s_sntp_c = NULL;
            c->is_closing = 1;
            publish_attempt(false, DG_ERR_NETWORK, 0);
        }
    } else if (ev == MG_EV_CLOSE) {
        /* 连接关闭而本轮还没结论(服务端回 kiss-of-death/坏报文等,
         * mongoose 内部解析失败即关连接):补一条失败,别让状态挂住 */
        if (s_sntp_c == c) {
            s_sntp_c = NULL;
            if (!s_attempt_done) {
                DG_LOGW(TAG, "SNTP 连接关闭但未取到时间(服务端应答无效?)");
                publish_attempt(false, DG_ERR_NETWORK, 0);
            }
        }
    }
}

/* loop 线程(netcore_post 闭包):发一次 SNTP 请求 */
static void sntp_start(void *arg)
{
    (void)arg;
    if (s_sntp_c)
        return;                              /* 已有在途请求:忽略重复触发 */

    char url[96];
    const dg_cfg_t *cfg = cfg_get();
    const char *server = (cfg && cfg->ntp_server[0]) ? cfg->ntp_server
                                                     : "ntp.aliyun.com";
    snprintf(url, sizeof(url), "udp://%s:123", server);

    s_sntp_c = mg_sntp_connect(netcore_mgr(), url, sntp_cb, NULL);
    if (!s_sntp_c) {
        DG_LOGW(TAG, "SNTP 连接创建失败(%s)", server);
        publish(false, DG_ERR_NETWORK, 0);
        return;
    }
    s_attempt_done = false;
    s_deadline_ms = mg_millis() + SNTP_TIMEOUT_MS;
    DG_LOGI(TAG, "开始 SNTP 校正(%s)", server);
}

/* ---- 业务线程入口 ---- */

int ntp_service_trigger(void)
{
    if (!atomic_load(&s_running))
        return DG_ERR_NOT_INIT;
    if (!net_info_is_online()) {
        DG_LOGW(TAG, "设备未联网,NTP 校正失败");
        publish(false, DG_ERR_NETWORK, 0);
        return DG_ERR_NETWORK;
    }
    netcore_post(sntp_start, NULL);
    return DG_OK;
}

/* 菜单按钮 → 总线触发:与 web 按钮同一入口 */
static int on_trigger_req(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    ntp_service_trigger();
    return 0;
}

/* 开机自动校正一次(独立线程只做"等待+探测",校正本身已异步化) */
static void *boot_trigger(void *arg)
{
    (void)arg;
    sleep(BOOT_DELAY_S);
    if (net_info_is_online()) {
        DG_LOGI(TAG, "开机联网检测通过,自动校正一次");
        ntp_service_trigger();
    } else {
        DG_LOGW(TAG, "开机未联网,跳过自动校正");
    }
    return NULL;
}

int ntp_service_start(void)
{
    if (atomic_load(&s_running))
        return DG_OK;
    atomic_store(&s_running, true);
    event_bus_subscribe(EV_NET_NTP_TRIGGER, on_trigger_req, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, boot_trigger, NULL) == 0)
        pthread_detach(tid);
    DG_LOGI(TAG, "ntp 服务启动(应用内 SNTP)");
    return DG_OK;
}
