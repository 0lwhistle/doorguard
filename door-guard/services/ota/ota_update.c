/*
 * ota_update.c — MQTT OTA 升级编排实现
 *
 * 线程模型:
 *   - 公告入口在 netcore loop 线程(mqtt_sub_fn)或测试直驱;只做解析与
 *     状态判定,绝不在 loop 里做 IO;
 *   - 下载是独立 worker 线程(pthread):阻塞读传输层 → ota_service 流水线
 *     (ota_write_chunk 的环形满阻塞在 worker 里是安全语义,与 web 事件
 *     循环的 can_accept 限流互不影响——同一时刻仅一个 ota_service 会话,
 *     冲突方收到 BUSY);
 *   - 状态与公告快照由互斥保护;对外呈现走 EV_NET_OTA_UPDATE 事件 +
 *     ota/state MQTT 上报(宁丢不堵)。
 *
 * 交接边界:ota_finish 校验闭环即止,S60 ota_watch 负责装槽/切换/回滚
 * (docs/tech/OTA_PLAN.md)——升级包不做任何分区写入。
 */
#include "ota_update.h"
#include "mqtt/mqtt_service.h"
#include "ota_service.h"
#include "cfg.h"
#include "cJSON.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "timeutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <string.h>

static const char *TAG = "[OTA-UPD]";

#ifndef DG_FW_VERSION
#define DG_FW_VERSION "unknown"   /* 交叉编译由 CMake 注入 git describe */
#endif

#define PROGRESS_STEP_PERMILLE 50   /* 每推进 5% 发一次进度事件 */
#define QUERY_TIMEOUT_MS (10 * 1000)
#define DL_CHUNK (16 * 1024)        /* 下载循环读块大小 */

/* ---- 共享状态(互斥) ---- */

typedef struct {
    char version[32];
    char date[16];
    char notes[128];
    char url[128];
    char sha256[65];
    uint32_t size;
} announce_t;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static ota_upd_state_t s_state = OTA_UPD_IDLE;
static int32_t  s_err = DG_OK;
static uint32_t s_permille;
static int64_t  s_query_ms;                 /* QUERYING 起点时间 */
static announce_t s_ann;                    /* 最近一次有效公告 */
static bool s_have_ann;

static atomic_bool s_running = false;
static atomic_bool s_busy = false;          /* 下载线程在途 */
static atomic_bool s_abort_req = false;     /* stop 请求 worker 收尾 */
static pthread_t s_thread;
static bool s_thread_alive;

/* 传输注入口(测试);NULL = 内置 HTTP */
static const ota_transport_t *s_tp;
static void *s_tp_ud;

/* ---- 对外呈现:事件 + MQTT 状态上报 ---- */

static void state_publish_locked(void)
{
    ev_ota_update_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.state = (uint8_t)s_state;
    ev.err = s_err;
    ev.permille = s_permille;
    if (s_have_ann && s_state != OTA_UPD_IDLE && s_state != OTA_UPD_QUERYING) {
        snprintf(ev.version, sizeof(ev.version), "%s", s_ann.version);
        snprintf(ev.date, sizeof(ev.date), "%s", s_ann.date);
        snprintf(ev.notes, sizeof(ev.notes), "%s", s_ann.notes);
    }
    EVENT_BUS_PUBLISH(EV_NET_OTA_UPDATE, &ev);
}

static void state_set(ota_upd_state_t st, int32_t err, uint32_t permille)
{
    pthread_mutex_lock(&s_mu);
    s_state = st;
    s_err = err;
    s_permille = permille;
    state_publish_locked();
    pthread_mutex_unlock(&s_mu);
}

/* 平台可见性(部署侧可订阅 ota/state 看进度;发送失败静默——遥测语义) */
static void mqtt_state_report(const char *phase, uint32_t permille, int err)
{
    char json[192];
    pthread_mutex_lock(&s_mu);
    const char *ver = s_ann.version;
    pthread_mutex_unlock(&s_mu);
    if (permille > 0)
        snprintf(json, sizeof(json),
                 "{\"state\":\"%s\",\"version\":\"%s\",\"permille\":%u,\"err\":%d}",
                 phase, ver, permille, err);
    else
        snprintf(json, sizeof(json),
                 "{\"state\":\"%s\",\"version\":\"%s\",\"err\":%d}",
                 phase, ver, err);
    mqtt_publish_json("ota/state", json, false);
}

/* ---- 公告解析与版本比较(纯函数段) ---- */

/* 解析点分数字前缀:n[0..2] 回填,返回成功解析的段数(≥1 = 可比较)。
 * 接受 v/V 前缀与第 3 段后的任意后缀(如 1.2.3-12-gabc 取 1.2.3) */
static int ver_parse(const char *s, int n[3])
{
    if (!s)
        return 0;
    while (*s == ' ' || *s == 'v' || *s == 'V')
        s++;
    int cnt = 0;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9')
            break;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 9999999)
                v = 9999999;                /* 防溢出,版本号不会这么大 */
            s++;
        }
        n[i] = v;
        cnt++;
        if (*s == '.')
            s++;
        else
            break;
    }
    return cnt;
}

bool ota_version_newer(const char *cur, const char *cand)
{
    int a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
    int na = ver_parse(cur, a), nb = ver_parse(cand, b);
    if (nb == 0)
        return false;                       /* 候选解析不了:不当新版 */
    if (na == 0)
        return true;                        /* 当前版本不可解析(unknown) */
    for (int i = 0; i < 3; i++) {
        if (b[i] != a[i])
            return b[i] > a[i];
    }
    return false;                           /* 前三段相等:后缀不参与比较 */
}

static bool hex_sha_valid(const char *s)
{
    if (strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok)
            return false;
    }
    return true;
}

/* 公告处理:解析失败/不比当前新 → 静默(IDLE 呈现);新版 → AVAILABLE,
 * 自动开则直接起下载。FAILED 后同版本重公告给一次重试机会(retained
 * 重连必到,重试频率天然受公告节奏约束) */
void ota_update_on_announce(const char *payload)
{
    if (!payload || !payload[0])
        return;
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        DG_LOGW(TAG, "公告 JSON 解析失败(%uB)", (unsigned)strlen(payload));
        return;
    }
    announce_t a;
    memset(&a, 0, sizeof(a));
    const cJSON *jv = cJSON_GetObjectItem(root, "version");
    const cJSON *jd = cJSON_GetObjectItem(root, "date");
    const cJSON *jn = cJSON_GetObjectItem(root, "notes");
    const cJSON *ju = cJSON_GetObjectItem(root, "url");
    const cJSON *js = cJSON_GetObjectItem(root, "sha256");
    const cJSON *ji = cJSON_GetObjectItem(root, "size");
    bool ok = cJSON_IsString(jv) && jv->valuestring[0] &&
              cJSON_IsString(js) && hex_sha_valid(js->valuestring) &&
              cJSON_IsNumber(ji) && ji->valuedouble > 0 &&
              ji->valuedouble <= (double)UINT32_MAX;
    if (ok) {
        snprintf(a.version, sizeof(a.version), "%s", jv->valuestring);
        snprintf(a.sha256, sizeof(a.sha256), "%s", js->valuestring);
        a.size = (uint32_t)ji->valuedouble;
        if (cJSON_IsString(jd))
            snprintf(a.date, sizeof(a.date), "%s", jd->valuestring);
        if (cJSON_IsString(jn))
            snprintf(a.notes, sizeof(a.notes), "%s", jn->valuestring);
        if (cJSON_IsString(ju))
            snprintf(a.url, sizeof(a.url), "%s", ju->valuestring);
    }
    cJSON_Delete(root);
    if (!ok) {
        DG_LOGW(TAG, "公告字段缺失/非法(version/sha256/size 必填)");
        return;
    }

    if (atomic_load(&s_busy)) {
        /* 在装公告版本时来了新公告:本次升级闭环后自然追平,不叠会话 */
        DG_LOGI(TAG, "下载进行中,忽略新公告 %s", a.version);
        return;
    }

    bool start = false;
    pthread_mutex_lock(&s_mu);
    const bool newer = ota_version_newer(DG_FW_VERSION, a.version);
    const bool retry = !newer && !strcmp(a.version, s_ann.version) &&
                       s_state == OTA_UPD_FAILED;
    if (newer || retry) {
        s_ann = a;
        s_have_ann = true;
        s_err = DG_OK;
        s_permille = 0;
        s_state = OTA_UPD_AVAILABLE;
        start = cfg_get()->ota_auto_update;
        state_publish_locked();
    }
    pthread_mutex_unlock(&s_mu);
    if (!newer && !retry) {
        DG_LOGI(TAG, "公告 %s 不比当前 %s 新,忽略", a.version, DG_FW_VERSION);
        /* 查询中的悬置态收敛:有了平台应答就不再等超时 */
        pthread_mutex_lock(&s_mu);
        if (s_state == OTA_UPD_QUERYING) {
            s_state = OTA_UPD_IDLE;
            state_publish_locked();
        }
        pthread_mutex_unlock(&s_mu);
        return;
    }
    DG_LOGI(TAG, "发现新版本 %s(当前 %s,%uB)%s", a.version, DG_FW_VERSION,
            a.size, start ? ",自动更新开→立即下载" : "");
    if (start)
        ota_update_apply();
}

/* ---- 下载 worker ---- */

static void announce_to_manifest(const announce_t *a, ota_manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->version, sizeof(m->version), "%s", a->version);
    snprintf(m->sha256, sizeof(m->sha256), "%s", a->sha256);
    m->size = a->size;
}

static void *download_thread(void *arg)
{
    (void)arg;
    announce_t a;
    ota_manifest_t m;
    char stage[256];

    pthread_mutex_lock(&s_mu);
    a = s_ann;
    pthread_mutex_unlock(&s_mu);
    announce_to_manifest(&a, &m);

    const char *url = a.url[0] ? a.url : cfg_get()->ota_url;
    int rc = DG_OK;
    uint32_t permille = 0;
    void *tp_ud = NULL;
    const ota_transport_t *tp;

    if (!url[0]) {
        DG_LOGE(TAG, "公告无 url 且 cfg ota_url 为空,无法下载");
        rc = DG_ERR_PARAM;
        goto done;
    }

    pthread_mutex_lock(&s_mu);
    tp = s_tp;
    tp_ud = s_tp_ud;
    pthread_mutex_unlock(&s_mu);

    void *src = NULL;
    if (tp) {
        src = tp_ud;
    } else {
        src = ota_http_src_new();
        if (!src) {
            rc = DG_ERR_NO_MEMORY;
            goto done;
        }
    }

    /* 打开源流:重定向在 http_open 内部消化;content_len 与公告对拍 */
    uint32_t content_len = 0;
    rc = tp ? tp->open(src, url, &content_len)
            : ota_http_open(src, url, &content_len);
    if (rc != DG_OK)
        goto free_src;
    if (content_len && content_len != a.size) {
        DG_LOGE(TAG, "源大小 %u 与公告 %u 不符", content_len, a.size);
        rc = DG_ERR_PARAM;
        goto close_src;
    }

    bool resumed = false;
    rc = ota_begin(&m, 0, &resumed);
    if (rc != DG_OK) {
        /* BUSY = web 上传正占着 ota 会话:如实报失败,公告保留可重试 */
        DG_LOGE(TAG, "ota_begin 失败(%d):升级会话被占用?", rc);
        goto close_src;
    }

    for (;;) {
        if (atomic_load(&s_abort_req)) {
            rc = DG_ERR_STATE;              /* stop 收尾:丢弃本包 */
            break;
        }
        uint8_t buf[DL_CHUNK];
        size_t got = 0;
        rc = tp ? tp->read(src, buf, sizeof(buf), &got)
                : ota_http_read(src, buf, sizeof(buf), &got);
        if (rc != DG_OK)
            break;
        if (got == 0) {
            rc = DG_OK;                     /* 流尽 = 收满 */
            break;
        }
        size_t received = 0;
        rc = ota_write_chunk(buf, got, &received);
        if (rc != DG_OK)
            break;
        pthread_mutex_lock(&s_mu);
        permille = (uint32_t)((uint64_t)ota_staged_bytes() * 1000 / a.size);
        if (permille > 1000)
            permille = 1000;
        bool report = permille - s_permille >= PROGRESS_STEP_PERMILLE;
        if (report)
            s_permille = permille;
        pthread_mutex_unlock(&s_mu);
        if (report) {
            state_set(OTA_UPD_DOWNLOADING, DG_OK, permille);
            mqtt_state_report("downloading", permille, DG_OK);
        }
    }

    if (rc == DG_OK) {
        rc = ota_finish(stage, sizeof(stage));
        if (rc == DG_OK) {
            DG_LOGI(TAG, "包校验闭环,暂存就绪(%s),交 S60 装槽", stage);
            state_set(OTA_UPD_STAGED, DG_OK, 1000);
            mqtt_state_report("staged", 1000, DG_OK);
        }
    }
    if (rc != DG_OK && !atomic_load(&s_abort_req)) {
        ota_abort();
        DG_LOGE(TAG, "下载/校验失败(%d)", rc);
        state_set(OTA_UPD_FAILED, rc, permille);
        mqtt_state_report("failed", permille, rc);
    } else if (rc != DG_OK) {
        ota_abort();                        /* stop 路径:静默清理 */
    }

close_src:
    if (tp && tp->close)
        tp->close(src);
    else
        ota_http_close(src);
free_src:
    if (!tp)
        ota_http_src_free(src);
done:
    atomic_store(&s_busy, false);
    return NULL;
}

int ota_update_apply(void)
{
    bool expect = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expect, true))
        return DG_ERR_BUSY;

    pthread_mutex_lock(&s_mu);
    bool can = s_have_ann && s_state == OTA_UPD_AVAILABLE;
    pthread_mutex_unlock(&s_mu);
    if (!can) {
        atomic_store(&s_busy, false);
        return DG_ERR_STATE;                /* 无公告或不在待更新态 */
    }

    state_set(OTA_UPD_DOWNLOADING, DG_OK, 0);
    mqtt_state_report("downloading", 0, DG_OK);

    if (pthread_create(&s_thread, NULL, download_thread, NULL) != 0) {
        atomic_store(&s_busy, false);
        state_set(OTA_UPD_FAILED, DG_ERR_INTERNAL, 0);
        return DG_ERR_NO_MEMORY;
    }
    s_thread_alive = true;
    pthread_detach(s_thread);
    return DG_OK;
}

/* ---- 手动检查更新 ---- */

int ota_update_check(void)
{
    if (!mqtt_service_connected())
        return DG_ERR_NETWORK;
    int rc = mqtt_publish_json("ota/query", "{}", false);
    if (rc != DG_OK)
        return rc;
    pthread_mutex_lock(&s_mu);
    s_state = OTA_UPD_QUERYING;
    s_err = DG_OK;
    s_query_ms = now_mono_ms();
    state_publish_locked();
    pthread_mutex_unlock(&s_mu);
    return DG_OK;
}

/* ---- 状态快照 ---- */

void ota_update_status(ota_upd_status_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&s_mu);
    ota_upd_state_t st = s_state;
    /* QUERYING 超时按 IDLE 呈现(平台不应答 = 查询无果;状态机不真正
     * 回写,避免与迟到的公告竞争) */
    if (st == OTA_UPD_QUERYING && now_mono_ms() - s_query_ms > QUERY_TIMEOUT_MS)
        st = OTA_UPD_IDLE;
    out->state = st;
    out->err = s_err;
    out->permille = s_permille;
    if (s_have_ann && st != OTA_UPD_IDLE && st != OTA_UPD_QUERYING) {
        snprintf(out->version, sizeof(out->version), "%s", s_ann.version);
        snprintf(out->date, sizeof(out->date), "%s", s_ann.date);
        snprintf(out->notes, sizeof(out->notes), "%s", s_ann.notes);
    }
    pthread_mutex_unlock(&s_mu);
}

/* ---- 注入口 ---- */

void ota_update_transport_set(const ota_transport_t *t, void *ud)
{
    pthread_mutex_lock(&s_mu);
    s_tp = t;
    s_tp_ud = ud;
    pthread_mutex_unlock(&s_mu);
}

/* ---- 启停 ---- */

static void on_version_msg(const char *suffix, const char *payload)
{
    (void)suffix;
    ota_update_on_announce(payload);
}

int ota_update_start(void)
{
    if (atomic_load(&s_running))
        return DG_OK;
    atomic_store(&s_running, true);
    atomic_store(&s_abort_req, false);
    /* mqtt 未启用时同样注册:注册表在 mqtt 侧,连接建立才有意义,空转无害 */
    int rc = mqtt_sub_register("ota/version", on_version_msg);
    if (rc != DG_OK)
        DG_LOGW(TAG, "注册 ota/version 订阅失败(%d)——mqtt 未启用?", rc);
    DG_LOGI(TAG, "OTA 升级编排就绪(当前版本 %s,自动更新 %s)", DG_FW_VERSION,
            cfg_get()->ota_auto_update ? "开" : "关");
    return DG_OK;
}

void ota_update_stop(void)
{
    if (!atomic_load(&s_running))
        return;
    atomic_store(&s_running, false);
    atomic_store(&s_abort_req, true);
    if (atomic_load(&s_busy)) {
        /* worker 是 detached:等它自认收尾(它每轮检查 abort 请求) */
        for (int i = 0; i < 300 && atomic_load(&s_busy); i++)
            usleep(10 * 1000);
    }
    atomic_store(&s_busy, false);
    (void)s_thread_alive;
    state_set(OTA_UPD_IDLE, DG_OK, 0);
}
