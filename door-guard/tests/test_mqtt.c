/*
 * test_mqtt.c — MQTT 服务宿主测试(测试内起最小 broker,零外部依赖)
 *
 * broker = mg_mqtt_listen 的回环监听:CONNECT→CONNACK、SUBSCRIBE→SUBACK、
 * PUBLISH(QoS1)→PUBACK、PINGREQ→PINGRESP,收到的每条发布按序记录供断言。
 *
 * 覆盖:未启用空转(start 仍 OK、不建连接)/ 连接握手(CONNECT+订阅
 * cmd/+ + status retain 上线发布)/ 内置命令 ping 往返(rsp/pong)/
 * 未知命令回 unknown-cmd / 自定义命令注册(mqtt_cmd_register 扩展口)/
 * 业务上报 mqtt_publish_json / 验证事件转发 event/auth / 停服发 offline。
 */
#include "dg_test.h"

#include "cfg.h"
#include "event_bus.h"
#include "events.h"
#include "mongoose.h"
#include "mqtt/mqtt_service.h"
#include "netcore.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 最小 broker ---- */

#define BR_LOG_MAX 16
typedef struct {
    char topic[80];
    char payload[128];
    bool retain;
} br_pub_t;

static struct mg_mgr s_br_mgr;
static struct mg_connection *s_br_ln = NULL;
static atomic_int s_br_connects;             /* 收到的 CONNECT 数 */
static atomic_int s_br_subscribes;
static br_pub_t s_br_pubs[BR_LOG_MAX];
static atomic_int s_br_pub_cnt;
static uint16_t s_sub_id;                    /* 最近 SUBSCRIBE 报文 id */

static void br_record(const struct mg_mqtt_message *m)
{
    const int idx = atomic_fetch_add(&s_br_pub_cnt, 1);
    if (idx >= BR_LOG_MAX)
        return;
    br_pub_t *p = &s_br_pubs[idx];
    snprintf(p->topic, sizeof(p->topic), "%.*s", (int)m->topic.len,
             m->topic.len < sizeof(p->topic) ? m->topic.buf : "");
    snprintf(p->payload, sizeof(p->payload), "%.*s", (int)m->data.len,
             m->data.len < sizeof(p->payload) ? m->data.buf : "");
    p->retain = (m->dgram.len > 0) && (m->dgram.buf[0] & 0x01);
}

static void br_cb(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_MQTT_CMD)
        return;
    struct mg_mqtt_message *m = (struct mg_mqtt_message *)ev_data;
    switch (m->cmd) {
    case MQTT_CMD_CONNECT:
        atomic_fetch_add(&s_br_connects, 1);
        mg_mqtt_send_header(c, MQTT_CMD_CONNACK, 0, 2);
        mg_send(c, "\x00\x00", 2);           /* 会话接受,无遗嘱标志 */
        break;
    case MQTT_CMD_SUBSCRIBE:
        atomic_fetch_add(&s_br_subscribes, 1);
        s_sub_id = m->id;
        mg_mqtt_send_header(c, MQTT_CMD_SUBACK, 0, 3);
        uint8_t suback[3] = { (uint8_t)(m->id >> 8), (uint8_t)m->id, 0x01 };
        mg_send(c, suback, 3);
        break;
    case MQTT_CMD_PUBLISH:
        br_record(m);
        if (m->qos == 1) {
            mg_mqtt_send_header(c, MQTT_CMD_PUBACK, 0, 2);
            uint8_t ack[2] = { (uint8_t)(m->id >> 8), (uint8_t)m->id };
            mg_send(c, ack, 2);
        }
        break;
    case MQTT_CMD_PINGREQ:
        mg_mqtt_pong(c);
        break;
    default:
        break;                                /* PUBACK(DISCONNECT 等)忽略 */
    }
}

/* 主线程泵 broker loop ms 毫秒 */
static void br_pump(int ms)
{
    const int64_t until = mg_millis() + ms;
    while ((int64_t)mg_millis() < until)
        mg_mgr_poll(&s_br_mgr, 10);
}

/* broker → client 发一条 PUBLISH */
static void br_pub(const char *topic, const char *payload)
{
    for (struct mg_connection *c = s_br_mgr.conns; c; c = c->next) {
        struct mg_mqtt_opts o = { 0 };
        o.topic = mg_str(topic);
        o.message = mg_str(payload);
        o.qos = 1;
        mg_mqtt_pub(c, &o);
    }
}

static const br_pub_t *br_find(const char *topic_prefix)
{
    const int n = atomic_load(&s_br_pub_cnt);
    for (int i = 0; i < n && i < BR_LOG_MAX; i++)
        if (!strncmp(s_br_pubs[i].topic, topic_prefix, strlen(topic_prefix)))
            return &s_br_pubs[i];
    return NULL;
}

/* ---- cfg 注入:mqtt 段(默认关/开的两个用例各写一份) ---- */

static char s_dir[128];
static int s_port;                           /* broker 实际端口(内核分配) */

static void cfg_write(bool enabled, int port)
{
    char def[160], cur[160];
    snprintf(def, sizeof(def), "%s/default.json", s_dir);
    snprintf(cur, sizeof(cur), "%s/cur.json", s_dir);
    char uri[48] = "";
    if (port > 0)
        snprintf(uri, sizeof(uri), "mqtt://127.0.0.1:%d", port);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"mqtt\":{\"enabled\":%s,\"uri\":\"%s\",\"topic_prefix\":\"dgtest\"}}",
             enabled ? "true" : "false", uri);
    FILE *f = fopen(def, "wb");
    DG_CHECK(f != NULL);
    if (f) {
        fwrite(body, 1, strlen(body), f);
        fclose(f);
    }
    f = fopen(cur, "wb");
    DG_CHECK(f != NULL);
    if (f) {
        fwrite("{}", 1, 2, f);
        fclose(f);
    }
    DG_CHECK(cfg_load(def, cur) == DG_OK);
}

/* 自定义命令(扩展口):回显 payload */
static int cmd_echo(const char *payload, char *resp, size_t resp_cap)
{
    snprintf(resp, resp_cap, "\"echo\":\"%s\"", payload);
    return DG_OK;
}

/* 扩展订阅捕获(mqtt_sub_fn 契约:loop 线程,纯接收无应答) */
static atomic_int s_sub_hit;
static char s_sub_topic[32];
static char s_sub_payload[128];

static void sub_capture(const char *topic_suffix, const char *payload)
{
    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s", topic_suffix);
    snprintf(s_sub_payload, sizeof(s_sub_payload), "%s", payload);
    atomic_fetch_add(&s_sub_hit, 1);
}

static void t_disabled(void)
{
    printf("[M1] 未启用:start 空转,不建连接\n");
    cfg_write(false, 0);
    DG_CHECK(mqtt_service_start() == DG_OK);
    DG_CHECK(mqtt_service_connected() == false);
    DG_CHECK(atomic_load(&s_br_connects) == 0);
    DG_CHECK(mqtt_publish_json("x/y", "{}", false) == DG_ERR_NOT_INIT);
    mqtt_service_stop();
}

static void t_enabled(void)
{
    printf("[M2] 连接握手:CONNECT + 订阅 cmd/+ + status retain 上线\n");
    cfg_write(true, s_port);    DG_CHECK(mqtt_cmd_register("echo", cmd_echo) == DG_OK);
    DG_CHECK(mqtt_sub_register("ota/version", sub_capture) == DG_OK);
    DG_CHECK(mqtt_sub_register("ota/version", sub_capture) == DG_ERR_PARAM); /* 重名拒 */
    DG_CHECK(mqtt_cmd_register("ping", cmd_echo) == DG_ERR_PARAM);  /* 重名拒 */
    DG_CHECK(mqtt_service_start() == DG_OK);

    for (int i = 0; i < 100 && !mqtt_service_connected(); i++) {
        br_pump(20);
        usleep(20 * 1000);
    }
    br_pump(300);
    DG_CHECK(mqtt_service_connected() == true);
    DG_CHECK(atomic_load(&s_br_connects) == 1);
    DG_CHECK(atomic_load(&s_br_subscribes) == 2);   /* cmd/+ + ota/version 扩展订阅 */
    const br_pub_t *st = br_find("dgtest/status");
    DG_CHECK(st != NULL && strcmp(st->payload, "{\"state\":\"online\"}") == 0);
    DG_CHECK(st != NULL && st->retain == true);              /* retain 上线态 */
    DG_CHECK(br_find("dgtest/hello") != NULL);               /* 附带 ip hello */

    printf("[M3] 内置命令 ping → rsp/pong;未知命令 → unknown-cmd\n");
    atomic_store(&s_br_pub_cnt, 0);
    br_pub("dgtest/cmd/ping", "{\"id\":7}");
    br_pump(300);
    usleep(100 * 1000);
    br_pump(200);
    const br_pub_t *rsp = br_find("dgtest/rsp/ping");
    DG_CHECK(rsp != NULL && strstr(rsp->payload, "pong") != NULL);

    atomic_store(&s_br_pub_cnt, 0);
    br_pub("dgtest/cmd/nope", "");
    br_pump(300);
    usleep(100 * 1000);
    br_pump(200);
    rsp = br_find("dgtest/rsp/nope");
    DG_CHECK(rsp != NULL && strstr(rsp->payload, "unknown-cmd") != NULL);

    printf("[M4] 自定义命令(echo)与业务上报\n");
    atomic_store(&s_br_pub_cnt, 0);
    br_pub("dgtest/cmd/echo", "hi");
    br_pump(300);
    usleep(100 * 1000);
    br_pump(200);
    rsp = br_find("dgtest/rsp/echo");
    DG_CHECK(rsp != NULL && strstr(rsp->payload, "\"echo\":\"hi\"") != NULL);

    DG_CHECK(mqtt_publish_json("event/x", "{\"k\":1}", false) == DG_OK);
    br_pump(300);
    rsp = br_find("dgtest/event/x");
    DG_CHECK(rsp != NULL && strcmp(rsp->payload, "{\"k\":1}") == 0);

    printf("[M5] 验证事件转发 event/auth(用户名含引号也稳)\n");
    atomic_store(&s_br_pub_cnt, 0);
    ev_auth_result_t r;
    memset(&r, 0, sizeof(r));
    r.has_user = true;
    snprintf(r.user_name, sizeof(r.user_name), "张\"三\\");
    r.method = DG_METHOD_FACE_1N;
    r.result = DG_RESULT_PASS;
    r.ts = 1759900000;
    EVENT_BUS_PUBLISH(EV_AUTH_RESULT, &r);
    br_pump(300);
    usleep(50 * 1000);
    br_pump(300);
    rsp = br_find("dgtest/event/auth");
    DG_CHECK(rsp != NULL && strstr(rsp->payload, "\"ok\":true") != NULL);
    DG_CHECK(rsp != NULL && strstr(rsp->payload, "张\'三") != NULL); /* 引号净化 */

    printf("[M4b] 扩展订阅 ota/version:平台推送直达处理器"); putchar(10);
    atomic_store(&s_br_pub_cnt, 0);
    br_pub("dgtest/ota/version", "{\"version\":\"1.2.3\"}");
    br_pump(300);
    usleep(50 * 1000);
    br_pump(300);
    DG_CHECK(atomic_load(&s_sub_hit) == 1);
    DG_CHECK(strcmp(s_sub_topic, "ota/version") == 0);
    DG_CHECK(strstr(s_sub_payload, "1.2.3") != NULL);

    printf("[M6] 停服:offline retain 告别\n");
    atomic_store(&s_br_pub_cnt, 0);
    mqtt_service_stop();
    br_pump(300);
    rsp = br_find("dgtest/status");
    DG_CHECK(rsp != NULL && strcmp(rsp->payload, "{\"state\":\"offline\"}") == 0);
    DG_CHECK(mqtt_service_connected() == false);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);

    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_mqtt_%d", (int)getpid());
    char cmd[480];   /* 两个 s_dir 拼接余量 */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    DG_CHECK(system(cmd) == 0);

    /* broker 先起:端口取内核分配(127.0.0.1:0 → loc 回填) */
    mg_mgr_init(&s_br_mgr);
    s_br_ln = mg_mqtt_listen(&s_br_mgr, "mqtt://127.0.0.1:0", br_cb, NULL);
    DG_CHECK(s_br_ln != NULL);
    char loc[48] = "";
    mg_snprintf(loc, sizeof(loc), "%M", mg_print_ip_port, &s_br_ln->loc);
    int port = 0;
    const char *colon = strrchr(loc, ':');
    if (colon)
        port = atoi(colon + 1);
    DG_CHECK(port > 0);
    s_port = port;
    printf("mini broker 就绪:%s\n", loc);

    DG_CHECK(netcore_start() == DG_OK);

    t_disabled();
    t_enabled();

    netcore_stop();
    mg_mgr_free(&s_br_mgr);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    (void)system(cmd);
    DG_TEST_EXIT();
}
