/*
 * mqtt_service.c — MQTT 上位机通道实现(mongoose mg_mqtt on netcore)
 *
 * 状态机(loop 线程内):IDLE → CONNECTING(TCP+CONNECT) → CONNECTED
 * (CONNACK ack==0)→ 断开/失败 → 退避重连(5/10/20/60s 封顶,连上即复位)。
 * 遗嘱(LWT)<prefix>/status="offline" retain:设备异常掉线,平台靠 retain
 * 拿到终态;正常停服主动发 offline 覆盖。
 *
 * 命令应答协议:<prefix>/cmd/<name> 收到 → 查注册表(内置 ping/status/open)
 * → 处理 → <prefix>/rsp/<name> 回 {"ok":true[,…]} 或 {"ok":false,"err":"…"};
 * 未知命令回 err="unknown-cmd"。
 *
 * 线程契约:mg_* 只在 netcore loop 线程执行(start/stop 的连接操作经
 * netcore_post 投递);业务线程(总线回调)的发布请求走邮箱(定长环形,
 * 满丢最旧计数)——遥测类消息宁丢不堵,绝不反向阻塞总线。
 */
#include "mqtt_service.h"
#include "cfg.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "net_info.h"
#include "netcore.h"
#include "timeutil.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "[MQTT]";

#define MQTT_KEEPALIVE_S    30     /* CONNECT keepalive;半开检测 = 3×此值 */
#define MQTT_BACKOFF_MIN_S  5      /* 重连退避起点 */
#define MQTT_BACKOFF_MAX_S  60     /* 重连退避封顶 */
#define MQTT_CMD_MAX        8      /* 注册命令上限(不含内置) */
#define MQTT_CMD_NAME_MAX   16
#define MQTT_MAILBOX_DEPTH  8      /* 业务→loop 发布请求环形队列容量 */
#define MQTT_RESP_MAX       192    /* rsp JSON 上限 */

/* ---- 状态(跨线程只经原子量/邮箱) ---- */

static atomic_bool s_running = false;
static atomic_bool s_enabled = false;       /* cfg 快照:整链路开关 */
static atomic_bool s_connected = false;

/* loop 线程私有 */
static struct mg_connection *s_conn = NULL;
static struct mg_timer *s_timer = NULL;     /* 1s 周期:重连/保活/半开检测 */
static int64_t s_next_retry_ms = 0;
static int     s_backoff_s = MQTT_BACKOFF_MIN_S;
static int64_t s_last_rx_ms = 0;
static bool    s_ping_out = false;          /* PINGREQ 已发未答 */
static char    s_prefix[32];                /* 主题前缀副本(启动时定格) */

typedef struct {
    char name[MQTT_CMD_NAME_MAX];
    mqtt_cmd_fn fn;
} mqtt_cmd_t;
static mqtt_cmd_t s_cmds[MQTT_CMD_MAX];
static int s_cmd_cnt;

/* 订阅注册表(扩展口):注册线程写、loop 线程读,细粒度互斥;fn 永远在
 * loop 线程执行。注册过的主题在每次(重)连接成功后统一 SUBSCRIBE */
typedef struct {
    char suffix[MQTT_SUB_NAME_MAX];
    mqtt_sub_fn fn;
} mqtt_sub_t;
static mqtt_sub_t s_subs[MQTT_SUB_MAX];
static int s_sub_cnt;
static pthread_mutex_t s_subs_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool enabled_now(void)
{
    return atomic_load(&s_enabled);
}

/* ---- 邮箱(业务线程 → loop):发布请求 ---- */

typedef struct {
    char suffix[48];
    char json[224];        /* 与 on_auth_result 的 json 同容(最坏 ~194) */
    bool retain;
} mbox_item_t;
static mbox_item_t s_mbox[MQTT_MAILBOX_DEPTH];
static int s_mbox_head = 0, s_mbox_tail = 0, s_mbox_dropped = 0;
static pthread_mutex_t s_mbox_mtx = PTHREAD_MUTEX_INITIALIZER;

static void mbox_push(const char *suffix, const char *json, bool retain)
{
    pthread_mutex_lock(&s_mbox_mtx);
    int next = (s_mbox_head + 1) % MQTT_MAILBOX_DEPTH;
    if (next == s_mbox_tail) {              /* 满:丢最旧(遥测宁丢不堵) */
        s_mbox_tail = (s_mbox_tail + 1) % MQTT_MAILBOX_DEPTH;
        s_mbox_dropped++;
    }
    mbox_item_t *it = &s_mbox[s_mbox_head];
    snprintf(it->suffix, sizeof(it->suffix), "%s", suffix);
    snprintf(it->json, sizeof(it->json), "%s", json);
    it->retain = retain;
    s_mbox_head = next;
    pthread_mutex_unlock(&s_mbox_mtx);
}

static int mbox_pop(mbox_item_t *out)
{
    pthread_mutex_lock(&s_mbox_mtx);
    if (s_mbox_head == s_mbox_tail) {
        pthread_mutex_unlock(&s_mbox_mtx);
        return 0;
    }
    *out = s_mbox[s_mbox_tail];
    s_mbox_tail = (s_mbox_tail + 1) % MQTT_MAILBOX_DEPTH;
    pthread_mutex_unlock(&s_mbox_mtx);
    return 1;
}

/* ---- 事件(总线线程 → UI/服务) ---- */

static void pub_state(bool connected, int err)
{
    ev_mqtt_state_t ev = { .connected = connected, .err = err };
    EVENT_BUS_PUBLISH(EV_MQTT_STATE, &ev);
}

/* JSON 字符串字段净化:入参可能来自用户输入(姓名等),裸拼会破 JSON
 * 结构;引号/反斜杠换成撇号,控制字符直接丢 */
static void json_sanitize(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; p && *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
            dst[o++] = '\'';
        else if (c >= 0x20)
            dst[o++] = *p;
    }
    dst[o] = '\0';
}

/* ---- loop 线程:发布与连接 ---- */

/* 全限定主题:<prefix>/<suffix> */
static void topic_full(char *out, size_t cap, const char *suffix)
{
    snprintf(out, cap, "%s/%s", s_prefix, suffix);
}

/* 只在 loop 线程调用(连接在手) */
static void pub_now(const char *suffix, const char *json, bool retain)
{
    if (!s_conn || !atomic_load(&s_connected))
        return;
    char topic[96];   /* prefix(31)+suffix(47)+分隔,64 会截断 */
    topic_full(topic, sizeof(topic), suffix);
    struct mg_mqtt_opts o = { 0 };
    o.topic = mg_str(topic);
    o.message = mg_str(json);
    o.qos = 0;
    o.retain = retain;
    mg_mqtt_pub(s_conn, &o);
}

/* ---- 内置命令(loop 线程) ---- */

/** ping:连通性/往返探测 */
static int cmd_ping(const char *payload, char *resp, size_t resp_cap)
{
    (void)payload;
    snprintf(resp, resp_cap, "\"pong\":true,\"uptime_s\":%lld",
             (long long)(now_mono_ms() / 1000));
    return DG_OK;
}

/** status:设备侧快照(IP/链路,与主页状态栏同源数据) */
static int cmd_status(const char *payload, char *resp, size_t resp_cap)
{
    (void)payload;
    net_info_addr_t a;
    net_info_read(&a);
    snprintf(resp, resp_cap, "\"ip\":\"%s\",\"link\":%s,\"uptime_s\":%lld",
             a.ip, (a.have_ip && a.have_link) ? "true" : "false",
             (long long)(now_mono_ms() / 1000));
    return DG_OK;
}

/** open:默认拒绝(安全默认,见 cfg mqtt.allow_remote_open 注)。授权开启后
 *  也只发 EV_MQTT_CMD 交总线——开门必须走 access 流程留 access_logs,不能
 *  在这里直碰 relay;消费端接入前如实回 no-consumer(接口预留) */
static int cmd_open(const char *payload, char *resp, size_t resp_cap)
{
    if (!cfg_get()->mqtt_allow_remote_open) {
        snprintf(resp, resp_cap, "\"err\":\"not-allowed\"");
        return DG_ERR_UNSUPPORTED;
    }
    ev_mqtt_cmd_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.name, sizeof(ev.name), "open");
    snprintf(ev.payload, sizeof(ev.payload), "%s", payload ? payload : "");
    ev.id = (uint32_t)(now_mono_ms() & 0x7FFFFFFFu);
    EVENT_BUS_PUBLISH(EV_MQTT_CMD, &ev);
    snprintf(resp, resp_cap, "\"err\":\"no-consumer\"");
    return DG_ERR_UNSUPPORTED;
}

/* 命令分发:注册表优先(扩展口),内置兜底;应答统一回 rsp/<name> */
static void cmd_dispatch(const char *name, const char *payload)
{
    static const struct { const char *name; mqtt_cmd_fn fn; } BUILTIN[] = {
        { "ping", cmd_ping },
        { "status", cmd_status },
        { "open", cmd_open },
    };
    mqtt_cmd_fn fn = NULL;
    for (int i = 0; i < s_cmd_cnt && !fn; i++) {
        if (!strcmp(s_cmds[i].name, name))
            fn = s_cmds[i].fn;
    }
    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]) && !fn; i++) {
        if (!strcmp(BUILTIN[i].name, name))
            fn = BUILTIN[i].fn;
    }

    char resp[MQTT_RESP_MAX];
    if (!fn) {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"err\":\"unknown-cmd\"}");
        DG_LOGW(TAG, "未知远程命令:%s", name);
    } else {
        char detail[144] = "";
        const int rc = fn(payload ? payload : "", detail, sizeof(detail));
        if (rc == DG_OK)
            snprintf(resp, sizeof(resp), "{\"ok\":true%s%s}",
                     detail[0] ? "," : "", detail);
        else
            snprintf(resp, sizeof(resp), "{\"ok\":false%s%s}",
                     detail[0] ? "," : "", detail);
    }
    char suffix[MQTT_CMD_NAME_MAX + 8];
    snprintf(suffix, sizeof(suffix), "rsp/%s", name);
    pub_now(suffix, resp, false);
}

/* ---- loop 线程:连接事件 ---- */

static void mqtt_cb(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_MQTT_OPEN) {
        /* 注意:OPEN 的 ev_data 是 &ack(uint8_t*,返回码)而非
         * mg_mqtt_message*(mongoose.h 注释与实现不一致,读错就是
         * 随机"拒绝码") */
        const uint8_t ack = *(const uint8_t *)ev_data;
        if (ack != 0) {
            DG_LOGW(TAG, "CONNACK 拒绝(ack=%u)", ack);
            c->is_closing = 1;               /* 走 CLOSE → 退避重连 */
            return;
        }
        s_conn = c;
        s_backoff_s = MQTT_BACKOFF_MIN_S;    /* 连上即复位退避 */
        s_last_rx_ms = (int64_t)mg_millis();
        s_ping_out = false;
        atomic_store(&s_connected, true);
        DG_LOGI(TAG, "broker 已连接(%s)", cfg_get()->mqtt_uri);

        char topic[96];   /* prefix(31)+suffix(47)+分隔,64 会截断 */
        topic_full(topic, sizeof(topic), "cmd/+");
        struct mg_mqtt_opts so = { .topic = mg_str(topic), .qos = 1 };
        mg_mqtt_sub(s_conn, &so);

        /* 扩展订阅(注册表快照取到局部,缩短持锁窗口;retained 公告会在
         * SUBACK 前后送达,消费方自行容忍) */
        mqtt_sub_t snap[MQTT_SUB_MAX];
        int n;
        pthread_mutex_lock(&s_subs_mtx);
        n = s_sub_cnt;
        memcpy(snap, s_subs, sizeof(mqtt_sub_t) * (size_t)n);
        pthread_mutex_unlock(&s_subs_mtx);
        for (int i = 0; i < n; i++) {
            char stopic[96];
            topic_full(stopic, sizeof(stopic), snap[i].suffix);
            struct mg_mqtt_opts uo = { .topic = mg_str(stopic), .qos = 1 };
            mg_mqtt_sub(s_conn, &uo);
            DG_LOGI(TAG, "订阅 %s", stopic);
        }

        pub_now("status", "{\"state\":\"online\"}", true);
        net_info_addr_t a;
        net_info_read(&a);
        char ip[24];
        json_sanitize(ip, sizeof(ip), a.ip);
        char hello[128];
        snprintf(hello, sizeof(hello), "{\"ip\":\"%s\"}", ip);
        pub_now("hello", hello, false);

        pub_state(true, DG_OK);
    } else if (ev == MG_EV_MQTT_MSG) {
        struct mg_mqtt_message *m = (struct mg_mqtt_message *)ev_data;
        s_last_rx_ms = (int64_t)mg_millis();
        /* 主题形如 <prefix>/cmd/<name> 或 <prefix>/<sub 后缀>;QoS1 的
         * PUBACK 由 mongoose 自动回 */
        char topic[96];
        snprintf(topic, sizeof(topic), "%.*s", (int)m->topic.len, m->topic.buf);
        char cmd_path[48];
        snprintf(cmd_path, sizeof(cmd_path), "%s/cmd/", s_prefix);
        if (!strncmp(topic, cmd_path, strlen(cmd_path))) {
            char payload[130];
            if (m->data.len < sizeof(payload))
                snprintf(payload, sizeof(payload), "%.*s", (int)m->data.len,
                         m->data.buf);
            else
                payload[0] = '\0';           /* 超长载荷:按空处理,命令仍分派 */
            const char *name = topic + strlen(cmd_path);
            DG_LOGI(TAG, "远程命令 %s(%zuB)", name, m->data.len);
            cmd_dispatch(name, payload);
            return;
        }
        /* 扩展订阅分发:精确匹配注册的 <prefix>/<suffix>(retained 公告/
         * 平台单向推送;超长载荷截断——公告字段有长度上限,截断即坏包,
         * 消费方解析失败自然丢弃) */
        mqtt_sub_t snap[MQTT_SUB_MAX];
        int n;
        pthread_mutex_lock(&s_subs_mtx);
        n = s_sub_cnt;
        memcpy(snap, s_subs, sizeof(mqtt_sub_t) * (size_t)n);
        pthread_mutex_unlock(&s_subs_mtx);
        for (int i = 0; i < n; i++) {
            char full[96];
            topic_full(full, sizeof(full), snap[i].suffix);
            if (strcmp(topic, full))
                continue;
            char payload[MQTT_SUB_PAYLOAD_MAX];
            if (m->data.len < sizeof(payload))
                snprintf(payload, sizeof(payload), "%.*s", (int)m->data.len,
                         m->data.buf);
            else
                payload[0] = '\0';
            DG_LOGI(TAG, "订阅消息 %s(%zuB)", snap[i].suffix, m->data.len);
            snap[i].fn(snap[i].suffix, payload);
        }
    } else if (ev == MG_EV_MQTT_CMD) {
        s_last_rx_ms = (int64_t)mg_millis();  /* PUBACK/PINGRESP 等也算活性 */
    } else if (ev == MG_EV_ERROR) {
        DG_LOGW(TAG, "连接失败: %s", (const char *)ev_data);
    } else if (ev == MG_EV_CLOSE) {
        const bool was_conn = atomic_load(&s_connected);
        s_conn = NULL;
        atomic_store(&s_connected, false);
        if (atomic_load(&s_running) && enabled_now()) {
            s_next_retry_ms = (int64_t)mg_millis() + s_backoff_s * 1000;
            s_backoff_s = s_backoff_s * 2 > MQTT_BACKOFF_MAX_S
                              ? MQTT_BACKOFF_MAX_S : s_backoff_s * 2;
            if (was_conn) {
                DG_LOGW(TAG, "连接断开,退避后重连");
                pub_state(false, DG_ERR_NETWORK);
            }
        }
    }
}

/* loop 线程:发起连接 */
static void connect_once(void *arg)
{
    (void)arg;
    if (!atomic_load(&s_running) || !enabled_now() || s_conn)
        return;
    const dg_cfg_t *cfg = cfg_get();
    if (!cfg->mqtt_uri[0]) {
        DG_LOGW(TAG, "mqtt.enabled=1 但 mqtt.uri 为空,保持空闲");
        return;
    }

    char client_id[64];
    if (cfg->mqtt_client_id[0]) {
        snprintf(client_id, sizeof(client_id), "%s", cfg->mqtt_client_id);
    } else {
        char ip[64] = "0.0.0.0";
        if (net_info_primary_ipv4(ip, sizeof(ip)) != DG_OK)
            snprintf(ip, sizeof(ip), "0.0.0.0");
        const char *dot = strrchr(ip, '.');
        snprintf(client_id, sizeof(client_id), "doorguard-%s",
                 dot ? dot + 1 : ip);
    }

    char will_topic[64];
    topic_full(will_topic, sizeof(will_topic), "status");
    struct mg_mqtt_opts o = { 0 };
    o.client_id = mg_str(client_id);
    o.topic = mg_str(will_topic);            /* CONNECT will topic */
    o.message = mg_str("{\"state\":\"offline\"}");
    o.keepalive = MQTT_KEEPALIVE_S;
    o.retain = true;                          /* will retain */
    o.clean = true;
    if (cfg->mqtt_username[0]) {
        o.user = mg_str(cfg->mqtt_username);
        o.pass = mg_str(cfg->mqtt_password);
    }

    s_conn = mg_mqtt_connect(netcore_mgr(), cfg->mqtt_uri, &o, mqtt_cb, NULL);
    if (!s_conn)
        DG_LOGW(TAG, "mg_mqtt_connect 失败(%s)", cfg->mqtt_uri);
}

/* loop 线程:立即排空邮箱(发布请求经 netcore_post 触发,遥测延迟从
 * 定时器周期(1s)降到 poll 粒度(几十 ms);定时器里的排水作兜底) */
static void drain_mailbox(void *arg)
{
    (void)arg;
    if (!s_conn || !atomic_load(&s_connected))
        return;
    mbox_item_t it;
    while (mbox_pop(&it))
        pub_now(it.suffix, it.json, it.retain);
}

/* loop 线程:1s 周期 —— 重连 / 保活 PINGREQ / 半开检测 / 邮箱兜底排水 */
static void timer_fn(void *arg)
{
    (void)arg;
    if (!atomic_load(&s_running) || !enabled_now())
        return;

    const int64_t now = (int64_t)mg_millis();
    if (!s_conn && now >= s_next_retry_ms) {
        netcore_post(connect_once, NULL);
        return;
    }
    if (!s_conn)
        return;
    if (!s_ping_out && now - s_last_rx_ms > (MQTT_KEEPALIVE_S / 2) * 1000) {
        mg_mqtt_ping(s_conn);
        s_ping_out = true;
    } else if (s_ping_out && now - s_last_rx_ms > 3LL * MQTT_KEEPALIVE_S * 1000) {
        DG_LOGW(TAG, "keepalive 无应答(半开?),强制重连");
        s_conn->is_closing = 1;
        return;
    }
    drain_mailbox(NULL);
}

static void timer_start(void *arg)
{
    (void)arg;
    s_timer = mg_timer_add(netcore_mgr(), 1000, MG_TIMER_REPEAT, timer_fn, NULL);
    s_next_retry_ms = (int64_t)mg_millis();   /* 立即首连 */
}

static void timer_stop(void *arg)
{
    (void)arg;
    if (s_timer) {
        mg_timer_free(&netcore_mgr()->timers, s_timer);
        s_timer = NULL;
    }
    if (s_conn) {
        /* 主动告别覆盖 LWT;直发不经 pub_now——stop() 已把 s_connected
         * 置 false,经它会被"未连接"守卫拦掉。is_draining 而非 is_closing:
         * 轮询循环对 is_closing 连接跳过写阶段直接关,待发的告别包会被
         * 吞掉;draining = 排空 send 后才关(mongoose 的优雅收尾语义) */
        struct mg_mqtt_opts o = { 0 };
        char topic[96];   /* prefix(31)+suffix(47)+分隔,64 会截断 */
        topic_full(topic, sizeof(topic), "status");
        o.topic = mg_str(topic);
        o.message = mg_str("{\"state\":\"offline\"}");
        o.retain = true;
        mg_mqtt_pub(s_conn, &o);
        s_conn->is_draining = 1;
        s_conn = NULL;
    }
    atomic_store(&s_connected, false);
}

/* ---- 总线订阅(总线线程):验证事件/看门狗处置 → 邮箱 ---- */

static int on_auth_result(const event_t *e, void *ud)
{
    (void)ud;
    if (!atomic_load(&s_enabled) || !atomic_load(&s_connected))
        return 0;                             /* 未连接不攒(重连后由 status 补) */
    const ev_auth_result_t *r = (const ev_auth_result_t *)e->data;
    char name[DG_NAME_LEN * 2];
    json_sanitize(name, sizeof(name), r->user_name);
    char json[224];                           /* 最坏 ~47 字面+127 名+20 ts */
    if (r->has_user)
        snprintf(json, sizeof(json),
                 "{\"ok\":%s,\"method\":%d,\"user\":\"%s\",\"ts\":%lld}",
                 (r->result == DG_RESULT_PASS) ? "true" : "false", r->method,
                 name, (long long)r->ts);
    else
        snprintf(json, sizeof(json), "{\"ok\":false,\"method\":%d,\"ts\":%lld}",
                 r->method, (long long)r->ts);
    mbox_push("event/auth", json, false);
    return 0;
}

static int on_service_state(const event_t *e, void *ud)
{
    (void)ud;
    if (!atomic_load(&s_enabled) || !atomic_load(&s_connected))
        return 0;
    const ev_sys_service_state_t *s = (const ev_sys_service_state_t *)e->data;
    char name[64];
    json_sanitize(name, sizeof(name), s->name);
    char json[160];
    snprintf(json, sizeof(json), "{\"service\":\"%s\",\"state\":%d}",
             name, s->state);
    mbox_push("event/service", json, false);
    return 0;
}

static event_subscription_t *s_sub_auth;
static event_subscription_t *s_sub_svc;

/* ---- 公共 API ---- */

int mqtt_service_start(void)
{
    if (atomic_load(&s_running))
        return DG_OK;
    atomic_store(&s_running, true);

    const dg_cfg_t *cfg = cfg_get();
    atomic_store(&s_enabled, cfg->mqtt_enabled ? true : false);
    snprintf(s_prefix, sizeof(s_prefix), "%s",
             cfg->mqtt_topic_prefix[0] ? cfg->mqtt_topic_prefix : "doorguard");

    if (!atomic_load(&s_enabled)) {
        DG_LOGI(TAG, "mqtt 未启用(cfg mqtt.enabled=0),服务空转");
        return DG_OK;
    }
    if (!netcore_running()) {
        DG_LOGE(TAG, "netcore 未运行,MQTT 不可用");
        atomic_store(&s_running, false);
        return DG_ERR_NOT_INIT;
    }

    s_backoff_s = MQTT_BACKOFF_MIN_S;
    s_sub_auth = event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    s_sub_svc = event_bus_subscribe(EV_SYS_SERVICE_STATE, on_service_state, NULL);
    netcore_post(timer_start, NULL);
    DG_LOGI(TAG, "mqtt 服务启动(%s,前缀 %s)", cfg->mqtt_uri, s_prefix);
    return DG_OK;
}

void mqtt_service_stop(void)
{
    if (!atomic_load(&s_running))
        return;
    atomic_store(&s_running, false);
    atomic_store(&s_connected, false);

    /* 连接/定时器的拆解全在 loop 侧(mg_* 的线程契约);stop 只翻标志、
     * 撤订阅。若 netcore 已停,post 静默丢弃——进程退出路径无害 */
    netcore_post(timer_stop, NULL);
    if (s_sub_auth) {
        event_bus_unsubscribe(s_sub_auth);
        s_sub_auth = NULL;
    }
    if (s_sub_svc) {
        event_bus_unsubscribe(s_sub_svc);
        s_sub_svc = NULL;
    }
    DG_LOGI(TAG, "mqtt 服务停止(邮箱丢弃累计 %d)", s_mbox_dropped);
}

bool mqtt_service_connected(void)
{
    return atomic_load(&s_connected);
}

int mqtt_publish_json(const char *suffix, const char *json, bool retain)
{
    if (!suffix || !json)
        return DG_ERR_PARAM;
    if (!atomic_load(&s_running) || !atomic_load(&s_enabled))
        return DG_ERR_NOT_INIT;
    mbox_push(suffix, json, retain);
    netcore_post(drain_mailbox, NULL);      /* 立即排水,不等 1s 定时器 */
    return DG_OK;
}

int mqtt_cmd_register(const char *name, mqtt_cmd_fn fn)
{
    static const char *const BUILTIN[] = { "ping", "status", "open" };
    if (!name || !fn || !name[0] || strlen(name) >= MQTT_CMD_NAME_MAX)
        return DG_ERR_PARAM;
    if (s_cmd_cnt >= MQTT_CMD_MAX)
        return DG_ERR_NO_MEMORY;
    for (int i = 0; i < s_cmd_cnt; i++) {
        if (!strcmp(s_cmds[i].name, name))
            return DG_ERR_PARAM;              /* 重名:已注册命令不可遮蔽 */
    }
    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        if (!strcmp(BUILTIN[i], name))
            return DG_ERR_PARAM;              /* 内置命令不可遮蔽 */
    }
    snprintf(s_cmds[s_cmd_cnt].name, MQTT_CMD_NAME_MAX, "%s", name);
    s_cmds[s_cmd_cnt].fn = fn;
    s_cmd_cnt++;
    return DG_OK;
}

/* loop 线程:把注册表里的扩展订阅补发一遍(已在订阅的主题重复 SUBSCRIBE
 * 无害,broker 侧幂等;比增量记账简单且覆盖「注册即断线重连」的窗口) */
static void subs_apply(void *arg)
{
    (void)arg;
    if (!s_conn || !atomic_load(&s_connected))
        return;
    mqtt_sub_t snap[MQTT_SUB_MAX];
    int n;
    pthread_mutex_lock(&s_subs_mtx);
    n = s_sub_cnt;
    memcpy(snap, s_subs, sizeof(mqtt_sub_t) * (size_t)n);
    pthread_mutex_unlock(&s_subs_mtx);
    for (int i = 0; i < n; i++) {
        char topic[96];
        topic_full(topic, sizeof(topic), snap[i].suffix);
        struct mg_mqtt_opts so = { .topic = mg_str(topic), .qos = 1 };
        mg_mqtt_sub(s_conn, &so);
        DG_LOGI(TAG, "订阅 %s", topic);
    }
}

int mqtt_sub_register(const char *suffix, mqtt_sub_fn fn)
{
    if (!suffix || !fn || !suffix[0] || strlen(suffix) >= MQTT_SUB_NAME_MAX)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_subs_mtx);
    for (int i = 0; i < s_sub_cnt; i++) {
        if (!strcmp(s_subs[i].suffix, suffix)) {
            pthread_mutex_unlock(&s_subs_mtx);
            return DG_ERR_PARAM;              /* 重名:同一主题不可双消费 */
        }
    }
    if (s_sub_cnt >= MQTT_SUB_MAX) {
        pthread_mutex_unlock(&s_subs_mtx);
        return DG_ERR_NO_MEMORY;
    }
    snprintf(s_subs[s_sub_cnt].suffix, MQTT_SUB_NAME_MAX, "%s", suffix);
    s_subs[s_sub_cnt].fn = fn;
    s_sub_cnt++;
    pthread_mutex_unlock(&s_subs_mtx);

    /* 已连接则立即补发;未连接等 OPEN 路径统一订阅。netcore 已停时 post
     * 静默丢弃(进程退出路径无害,与 stop 注释同约定) */
    netcore_post(subs_apply, NULL);
    return DG_OK;
}

int64_t mqtt_service_heartbeat_ms(void)
{
    if (!atomic_load(&s_running) || !atomic_load(&s_enabled))
        return 0;                             /* 0 = 不判失联(registry 契约) */
    return netcore_heartbeat_ms();            /* 引擎活性 = loop 活性 */
}
