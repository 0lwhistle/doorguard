/*
 * web_server.c — 内嵌 web 实现(civetweb)
 *
 * 路由:
 *   GET  /            登录/主控台单页(嵌入 HTML)
 *   POST /api/login   登录 → token(口令 PBKDF2 校验,凭据存 device_config)
 *   GET  /api/ws      WebSocket:实时推送 EV_AUTH_RESULT / NTP 结果
 *   GET  /api/logs    日志查询(时间段/分页,JSON)
 *   GET  /api/device  设备信息(版本/运行时长/日志总数)
 *   POST /api/ntp     NTP 校正触发
 *   POST /api/ota/upload  OTA 包流式接收(见 ota_service.h)
 * 鉴权:除 / 与 /api/login 外需 X-Auth-Token(token 表:内存+超时)。
 */
#include "web_server.h"
#include "ota/ota_service.h"
#include "ntp/ntp_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "hal/storage/crypto.h"

#include <openssl/rand.h>

#include "civetweb.h"

#include <cJSON.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "web_pages.h"

static const char *TAG = "[WEB]";

#define WEB_MAX_TOKENS 8
#define WEB_TOKEN_TTL_S 3600

typedef struct {
    char token[33];
    time_t expiry;
    bool used;
} web_token_t;

static struct mg_context *s_ctx = NULL;
static web_token_t s_tokens[WEB_MAX_TOKENS];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static event_subscription_t *s_subs[4];
static int s_sub_cnt = 0;

/* ---- WebSocket 推送(轮询排水模式) ----
 *
 * 为什么不由总线线程直接 mg_websocket_write:civetweb 连接仅允许其
 * 自有线程安全操作,跨线程写在对端断开时可能崩溃(实测)。改为:
 * 总线回调把消息入 pending 队列;客户端周期发 "ping"(浏览器 JS /
 * 测试脚本),ws_data 在 civetweb 自有线程排水队列——写连接只发生在
 * 属于它的线程上。
 */

#define WS_PENDING 16
static char s_pending[WS_PENDING][256];
static int s_pending_head = 0, s_pending_tail = 0;
static pthread_mutex_t s_pending_mtx = PTHREAD_MUTEX_INITIALIZER;

static void ws_enqueue(const char *json)
{
    pthread_mutex_lock(&s_pending_mtx);
    int next = (s_pending_head + 1) % WS_PENDING;
    if (next == s_pending_tail) {           /* 满:丢最旧 */
        s_pending_tail = (s_pending_tail + 1) % WS_PENDING;
    }
    snprintf(s_pending[s_pending_head], sizeof(s_pending[0]), "%s", json);
    s_pending_head = next;
    pthread_mutex_unlock(&s_pending_mtx);
}

static int ws_connect(const struct mg_connection *conn, void *ud)
{
    (void)conn; (void)ud;
    return 0;                               /* 接受升级 */
}

static void ws_ready(struct mg_connection *conn, void *ud)
{
    (void)conn; (void)ud;
}

static int ws_data(struct mg_connection *conn, int bits, char *data,
                   size_t len, void *ud)
{
    (void)bits; (void)ud; (void)data; (void)len;
    /* 客户端任意消息(如 "ping")触发:排水 pending(本线程写,安全) */
    for (;;) {
        char msg[256];
        pthread_mutex_lock(&s_pending_mtx);
        if (s_pending_tail == s_pending_head) {
            pthread_mutex_unlock(&s_pending_mtx);
            break;
        }
        snprintf(msg, sizeof(msg), "%s", s_pending[s_pending_tail]);
        s_pending_tail = (s_pending_tail + 1) % WS_PENDING;
        pthread_mutex_unlock(&s_pending_mtx);
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, msg, strlen(msg));
    }
    return 1;                               /* 保持连接 */
}

static void ws_close(const struct mg_connection *conn, void *ud)
{
    (void)conn; (void)ud;
}

/* ---- 鉴权 ---- */

static int check_token(struct mg_connection *conn)
{
    const char *tok = mg_get_header(conn, "X-Auth-Token");
    if (!tok)
        return 0;
    pthread_mutex_lock(&s_mtx);
    int ok = 0;
    for (int i = 0; i < WEB_MAX_TOKENS; i++) {
        if (s_tokens[i].used && s_tokens[i].expiry > time(NULL) &&
            !strcmp(s_tokens[i].token, tok)) {
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return ok;
}

static void http_json(struct mg_connection *conn, int code, const char *json)
{
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n", code, strlen(json));
    mg_write(conn, hdr, strlen(hdr));
    mg_write(conn, json, strlen(json));
}

static void http_401(struct mg_connection *conn)
{
    http_json(conn, 401, "{\"msg\":\"未登录或 token 过期\"}");
}

/* ---- 登录 ---- */

static int web_credential_verify(const char *user, const char *pwd)
{
    /* 凭据存 device_config(web_pwd_hash/web_pwd_salt hex);
     * 首次使用自动以默认 admin/admin 初始化(首启强制改密 TODO 上位机侧) */
    char salt_hex[33], hash_hex[65];
    char u[16];
    if (db_config_get("web_user", u, sizeof(u)) != DG_OK) {
        uint8_t salt[16], hash[32];
        RAND_bytes(salt, sizeof(salt));
        dg_pbkdf2_sha256("admin", salt, sizeof(salt), DG_PBKDF2_ITERS, hash);
        /* hex 化入库 */
        char s[33], h[65];
        for (int i = 0; i < 16; i++) sprintf(s + i * 2, "%02x", salt[i]);
        for (int i = 0; i < 32; i++) sprintf(h + i * 2, "%02x", hash[i]);
        db_config_set("web_user", "admin");
        db_config_set("web_pwd_salt", s);
        db_config_set("web_pwd_hash", h);
    }
    if (db_config_get("web_user", u, sizeof(u)) != DG_OK ||
        db_config_get("web_pwd_salt", salt_hex, sizeof(salt_hex)) != DG_OK ||
        db_config_get("web_pwd_hash", hash_hex, sizeof(hash_hex)) != DG_OK)
        return 0;
    if (strcmp(u, user) != 0)
        return 0;

    uint8_t salt[16], expect[32], calc[32];
    for (int i = 0; i < 16; i++)
        sscanf(salt_hex + i * 2, "%2hhx", &salt[i]);
    for (int i = 0; i < 32; i++)
        sscanf(hash_hex + i * 2, "%2hhx", &expect[i]);
    if (dg_pbkdf2_sha256(pwd, salt, sizeof(salt), DG_PBKDF2_ITERS, calc) != DG_OK)
        return 0;
    return dg_constant_time_cmp(calc, expect, 32) == 0;
}

static int handle_login(struct mg_connection *conn, void *ud)
{
    (void)ud;
    char body[256] = { 0 };
    int n = mg_read(conn, body, sizeof(body) - 1);
    if (n <= 0) {
        http_json(conn, 400, "{\"msg\":\"坏请求\"}");
        return 200;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        http_json(conn, 400, "{\"msg\":\"坏 json\"}");   /* 坏 json 不崩 */
        return 200;
    }
    cJSON *ju = cJSON_GetObjectItem(j, "user");
    cJSON *jp = cJSON_GetObjectItem(j, "pwd");
    const char *user = cJSON_IsString(ju) ? ju->valuestring : "";
    const char *pwd = cJSON_IsString(jp) ? jp->valuestring : "";

    char resp[128];
    if (web_credential_verify(user, pwd)) {
        char token[33];
        uint8_t rnd[16];
        RAND_bytes(rnd, sizeof(rnd));
        for (int i = 0; i < 16; i++)
            sprintf(token + i * 2, "%02x", rnd[i]);
        token[32] = '\0';
        pthread_mutex_lock(&s_mtx);
        for (int i = 0; i < WEB_MAX_TOKENS; i++) {
            if (!s_tokens[i].used || s_tokens[i].expiry < time(NULL)) {
                snprintf(s_tokens[i].token, sizeof(s_tokens[i].token), "%s", token);
                s_tokens[i].expiry = time(NULL) + WEB_TOKEN_TTL_S;
                s_tokens[i].used = true;
                break;
            }
        }
        pthread_mutex_unlock(&s_mtx);
        snprintf(resp, sizeof(resp), "{\"token\":\"%s\"}", token);
        http_json(conn, 200, resp);
    } else {
        http_json(conn, 401, "{\"msg\":\"账号或密码错误\"}");
    }
    cJSON_Delete(j);
    return 200;
}

/* ---- 日志查询 ---- */

static const char *method_name(int32_t m)
{
    switch (m) {
    case DG_METHOD_FACE_1N: return "人脸1:N";
    case DG_METHOD_FACE_11: return "人脸1:1";
    case DG_METHOD_FINGER:  return "指纹";
    case DG_METHOD_PWD:     return "密码";
    default:                return "IC卡";
    }
}

static const char *result_name(int32_t r)
{
    return (r == DG_RESULT_PASS) ? "通过" : "拒绝";
}

static int handle_logs(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!check_token(conn)) {
        http_401(conn);
        return 200;
    }

    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1;
    q.page_size = 20;
    q.descending = true;

    /* 时间段(可选):from/to 格式 YYYY-MM-DD;边界含端点 */
    const struct mg_request_info *info = mg_get_request_info(conn);
    const char *qs = info->query_string;
    char vbuf[32] = "";
    if (qs && mg_get_var(qs, strlen(qs), "from", vbuf, sizeof(vbuf)) > 0 && vbuf[0]) {
        struct tm tmv = { 0 };
        if (sscanf(vbuf, "%d-%d-%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
            tmv.tm_year -= 1900;
            tmv.tm_mon -= 1;
            q.ts_from = (int64_t)mktime(&tmv);
        }
    }
    if (qs && mg_get_var(qs, strlen(qs), "to", vbuf, sizeof(vbuf)) > 0 && vbuf[0]) {
        struct tm tmv = { 0 };
        if (sscanf(vbuf, "%d-%d-%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
            tmv.tm_year -= 1900;
            tmv.tm_mon -= 1;
            tmv.tm_mday += 1;               /* 结束日含当天:次日 0 点前 */
            q.ts_to = (int64_t)mktime(&tmv) - 1;
        }
    }
    if (qs && mg_get_var(qs, strlen(qs), "page", vbuf, sizeof(vbuf)) > 0 && vbuf[0]) {
        int pg = atoi(vbuf);
        if (pg < 1) {
            http_json(conn, 400, "{\"msg\":\"坏参数:page\"}");
            return 200;
        }
        q.page = (uint32_t)pg;
    }

    access_log_t rows[32];
    log_page_t out = { .logs = rows, .max = 32 };
    if (db_log_query(&q, &out) != DG_OK) {
        http_json(conn, 400, "{\"msg\":\"坏参数\"}");
        return 200;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "page", (double)q.page);
    uint32_t pages = (out.total + q.page_size - 1) / q.page_size;
    cJSON_AddNumberToObject(root, "pages", (double)pages);
    cJSON_AddNumberToObject(root, "total", (double)out.total);
    cJSON *arr = cJSON_AddArrayToObject(root, "logs");
    char tsbuf[24];
    for (uint32_t i = 0; i < out.count; i++) {
        cJSON *o = cJSON_CreateObject();
        time_t ts = (time_t)rows[i].ts;
        struct tm tmv;
        localtime_r(&ts, &tmv);
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv);
        cJSON_AddStringToObject(o, "time", tsbuf);
        cJSON_AddStringToObject(o, "user_id",
                                rows[i].has_user ? rows[i].user_id : "-");
        cJSON_AddStringToObject(o, "user_name",
                                rows[i].has_user ? rows[i].user_name : "陌生人");
        cJSON_AddStringToObject(o, "method_name", method_name(rows[i].method));
        cJSON_AddStringToObject(o, "result_name", result_name(rows[i].result));
        cJSON_AddNumberToObject(o, "result", rows[i].result);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    http_json(conn, 200, json);
    free(json);
    cJSON_Delete(root);
    return 200;
}

/* ---- 设备信息 ---- */

static int handle_device(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!check_token(conn)) {
        http_401(conn);
        return 200;
    }
    uint32_t total = 0;
    db_user_count(&total);
    (void)total;
    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1; q.page_size = 1;
    access_log_t row;
    log_page_t page = { .logs = &row, .max = 1 };
    db_log_query(&q, &page);

    char body[160];
    snprintf(body, sizeof(body),
             "{\"version\":\"B4-dev\",\"uptime_s\":%ld,\"log_total\":%u}",
             (long)(time(NULL) - 0), page.total);
    http_json(conn, 200, body);
    return 200;
}

/* ---- NTP 触发(阻塞秒级;web 线程可接受) ---- */

static int handle_ntp(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!check_token(conn)) {
        http_401(conn);
        return 200;
    }
    int rc = ntp_service_trigger();
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":%s}", rc == DG_OK ? "true" : "false");
    http_json(conn, 200, body);
    return 200;
}

/* ---- OTA 上传 ---- */

static int handle_ota(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!check_token(conn)) {
        http_401(conn);
        return 200;
    }
    const char *ver = mg_get_header(conn, "X-OTA-Version");
    const char *size_s = mg_get_header(conn, "X-OTA-Size");
    const char *sha = mg_get_header(conn, "X-OTA-SHA256");
    const char *off_s = mg_get_header(conn, "X-OTA-Offset");
    if (!ver || !size_s || !sha) {
        http_json(conn, 400, "{\"msg\":\"缺少 manifest 头\"}");
        return 200;
    }

    ota_manifest_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.version, sizeof(m.version), "%s", ver);
    m.size = (uint32_t)strtoul(size_s, NULL, 10);
    snprintf(m.sha256, sizeof(m.sha256), "%s", sha);
    uint32_t offset = off_s ? (uint32_t)strtoul(off_s, NULL, 10) : 0;

    bool resumed = false;
    int rc = ota_begin(&m, offset, &resumed);
    if (rc == DG_ERR_PARAM) {
        http_json(conn, 400, "{\"msg\":\"包大小超限或参数非法\"}");
        return 200;
    }
    if (rc == DG_ERR_STATE) {
        http_json(conn, 409, "{\"msg\":\"续传偏移不符,请重传\"}");
        return 200;
    }
    if (rc == DG_ERR_BUSY) {
        http_json(conn, 409, "{\"msg\":\"其他上传进行中\"}");
        return 200;
    }
    if (rc != DG_OK) {
        http_json(conn, 500, "{\"msg\":\"开始失败\"}");
        return 200;
    }

    /* 流式收包落盘 */
    char buf[8192];
    int64_t remain = (int64_t)m.size - (int64_t)offset;
    while (remain > 0) {
        int n = mg_read(conn, buf, remain > (int64_t)sizeof(buf)
                            ? (int)sizeof(buf) : (int)remain);
        if (n <= 0)
            break;
        size_t got;
        if (ota_write_chunk((uint8_t *)buf, (size_t)n, &got) != DG_OK) {
            ota_abort();
            http_json(conn, 400, "{\"msg\":\"写入失败(超大小?)\"}");
            return 200;
        }
        remain -= n;
    }

    char path[64];
    rc = ota_finish(path, sizeof(path));
    if (rc != DG_OK) {
        http_json(conn, 422, "{\"msg\":\"校验失败(sha256/大小)\"}");
        return 200;
    }
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"staged\":\"%s\"%s}", path,
             resumed ? ",\"resumed\":true" : "");
    http_json(conn, 200, resp);
    return 200;
}

/* ---- 事件推送(总线 → WS) ---- */

static int on_auth_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_auth_result_t *r = (const ev_auth_result_t *)e->data;
    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"auth\",\"time\":%lld,\"user_id\":\"%s\","
             "\"user_name\":\"%s\",\"method\":%d,\"result\":%d,\"reason\":%d}",
             (long long)r->ts, r->user_id, r->user_name, r->method,
             r->result, r->reason);
    ws_enqueue(json);
    return 0;
}

static int on_ntp_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ntp_result_t *r = (const ev_ntp_result_t *)e->data;
    char json[64];
    snprintf(json, sizeof(json), "{\"type\":\"ntp\",\"ok\":%s}",
             r->ok ? "true" : "false");
    ws_enqueue(json);
    return 0;
}

static int handle_index(struct mg_connection *conn, void *ud)
{
    (void)ud;
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
              "Content-Length: %zu\r\n\r\n", strlen(DG_WEB_INDEX_HTML));
    mg_write(conn, DG_WEB_INDEX_HTML, strlen(DG_WEB_INDEX_HTML));
    return 200;
}

int web_server_start(void)
{
    if (s_ctx)
        return DG_OK;

    memset(s_tokens, 0, sizeof(s_tokens));

    const char *opts[] = {
        "listening_ports", "8080",
        "num_threads", "4",
        "request_timeout_ms", "10000",
        NULL,
    };
    s_ctx = mg_start(NULL, NULL, opts);
    if (!s_ctx) {
        DG_LOGE(TAG, "web 启动失败");
        return DG_ERR_IO;
    }

    mg_set_request_handler(s_ctx, "/", handle_index, NULL);
    mg_set_request_handler(s_ctx, "/api/login", handle_login, NULL);
    mg_set_request_handler(s_ctx, "/api/logs", handle_logs, NULL);
    mg_set_request_handler(s_ctx, "/api/device", handle_device, NULL);
    mg_set_request_handler(s_ctx, "/api/ntp", handle_ntp, NULL);
    mg_set_request_handler(s_ctx, "/api/ota/upload", handle_ota, NULL);
    mg_set_websocket_handler(s_ctx, "/api/ws", ws_connect, ws_ready, ws_data,
                             ws_close, NULL);

    s_sub_cnt = 0;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_NTP_RESULT, on_ntp_result, NULL);

    DG_LOGI(TAG, "web 上位机就绪 :8080");
    return DG_OK;
}

void web_server_stop(void)
{
    if (s_ctx) {
        mg_stop(s_ctx);
        s_ctx = NULL;
    }
    for (int i = 0; i < s_sub_cnt; i++)
        event_bus_unsubscribe(s_subs[i]);
    s_sub_cnt = 0;
}
