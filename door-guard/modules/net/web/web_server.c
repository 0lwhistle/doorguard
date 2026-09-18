/*
 * web_server.c — 内嵌 web 上位机实现(civetweb)
 *
 * 路由表(method 严格校验:改状态的接口不接受 GET——原来 GET /api/ntp
 * 也能触发联网校时,属错误面过宽的坑):
 *   GET  /                单页应用(HTML;CSS/JS 见 /app.css、/app.js)
 *   GET  /app.css /app.js 样式与脚本(蓝白主题 + 动效)
 *   POST /api/login       登录 → {token,user,pwd_default,expires_in}
 *   POST /api/logout      注销当前 token
 *   GET  /api/device      设备信息(版本/运行时长/用户数/日志数/IP/mDNS/账号/NTP)
 *   GET  /api/logs        门禁日志查询(时间段 + 用户 ID + 分页,JSON)
 *   POST /api/ntp         触发一次 NTP 校正(异步,结果走 WebSocket)
 *   POST /api/account     改账号/口令(需旧口令;成功后所有会话失效)
 *   POST /api/ota/upload  OTA 包流式接收(见 ota_service.h)
 *   GET  /api/ws          WebSocket:实时推送认证事件/NTP 结果
 *
 * WebSocket 推送模型(替代原"客户端每 2s 发 ping 才排水"的临时方案):
 *   总线回调只把消息入队(总线线程绝不碰连接);独立的推送线程出队后
 *   逐连接 mg_websocket_write。civetweb 的连接释放路径同样要拿
 *   conn 锁并先经过 close 回调,因此"持连接表锁 + mg_lock_connection"
 *   能保证写期间连接不会被释放(跨线程写导致崩溃的根因即此处)。
 */
#include "web_server.h"
#include "web_auth.h"
#include "web_session.h"
#include "web_pages.h"
#include "ota/ota_service.h"
#include "ntp/ntp_service.h"
#include "net_info.h"
#include "mdns/mdns_responder.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "cfg.h"
#include "storage.h"

#include "civetweb.h"

#include <cJSON.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DG_FW_VERSION
#define DG_FW_VERSION "unknown"      /* 交叉编译时由 CMake 注入 git describe */
#endif

static const char *TAG = "[WEB]";

#define WS_MAX_CONN      4
#define WS_QUEUE         24
#define WS_MSG_MAX       320
#define HTTP_REASON_OK    "200 OK"
#define LOG_PAGE_MAX     100         /* 单页上限:别让一次查询把 4 个 worker 拖住 */

static struct mg_context *s_ctx = NULL;
static event_subscription_t *s_subs[8];
static int s_sub_cnt = 0;
static time_t s_started_at = 0;

/* 最近一次 NTP 结果(设备信息里显示"已同步/未同步 + 时间") */
static bool s_ntp_ok = false;
static int64_t s_ntp_ts = 0;

/* 前置声明:WS 拒连要回一个完整的 401(实现见下方"HTTP 响应工具") */
static void json_msg(struct mg_connection *conn, int code, const char *msg);

/* ---- WebSocket 连接表 + 推送队列 ---- */

typedef struct {
    struct mg_connection *conn;
    bool                  used;
} ws_slot_t;

static ws_slot_t s_ws[WS_MAX_CONN];
static char s_queue[WS_QUEUE][WS_MSG_MAX];
static int s_q_head = 0, s_q_tail = 0;
static int s_q_dropped = 0;
static pthread_mutex_t s_ws_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ws_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_pusher;
static bool s_pusher_run = false;

/* 入队(任意线程;满则丢最旧并计数——宁可丢几条实时事件,也不能阻塞总线) */
static void ws_enqueue(const char *json)
{
    pthread_mutex_lock(&s_ws_mtx);
    int next = (s_q_head + 1) % WS_QUEUE;
    if (next == s_q_tail) {
        s_q_tail = (s_q_tail + 1) % WS_QUEUE;
        s_q_dropped++;
    }
    snprintf(s_queue[s_q_head], WS_MSG_MAX, "%s", json);
    s_q_head = next;
    pthread_cond_signal(&s_ws_cond);
    pthread_mutex_unlock(&s_ws_mtx);
}

/* 出队(推送线程) */
static bool ws_dequeue(char *out, size_t cap)
{
    if (s_q_tail == s_q_head)
        return false;
    snprintf(out, cap, "%s", s_queue[s_q_tail]);
    s_q_tail = (s_q_tail + 1) % WS_QUEUE;
    return true;
}

/* 推送线程:出队 → 广播。持表锁期间写连接,close 回调因此不会与其
 * 并发地把连接摘掉/释放(见文件头说明) */
static void *ws_pusher_thread(void *arg)
{
    (void)arg;
    while (1) {
        char msg[WS_MSG_MAX];
        pthread_mutex_lock(&s_ws_mtx);
        while (s_pusher_run && s_q_head == s_q_tail) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;                  /* 1s 超时兜底:保证能响应停止 */
            pthread_cond_timedwait(&s_ws_cond, &s_ws_mtx, &ts);
        }
        if (!s_pusher_run) {
            pthread_mutex_unlock(&s_ws_mtx);
            break;
        }
        bool have = ws_dequeue(msg, sizeof(msg));
        if (have) {
            for (int i = 0; i < WS_MAX_CONN; i++) {
                if (s_ws[i].used && s_ws[i].conn)
                    mg_websocket_write(s_ws[i].conn, MG_WEBSOCKET_OPCODE_TEXT,
                                       msg, strlen(msg));
            }
        }
        pthread_mutex_unlock(&s_ws_mtx);
    }
    return NULL;
}

static int ws_connect(const struct mg_connection *conn, void *ud)
{
    (void)ud;
    /* 浏览器 WebSocket 不能自定义请求头 → token 走查询串。
     * 校验失败必须显式回 401 再拒绝:否则客户端只看到连接被关,无法区分
     * "密码错"和"网络断" */
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char token[64] = "";
    if (ri && ri->query_string)
        mg_get_var(ri->query_string, strlen(ri->query_string), "token", token,
                   sizeof(token));
    if (!web_session_validate(token, time(NULL))) {
        DG_LOGW(TAG, "WebSocket 未授权连接被拒(来源 %s)",
                ri ? ri->remote_addr : "?");
        /* 不能用 mg_send_http_error:它先置 conn->status_code,随后的
         * mg_response_header_start 认为"响应已开始"而不再发状态行,
         * 客户端只收到裸 body(实测),401 状态丢失。这里自己写完整响应 */
        json_msg((struct mg_connection *)conn, 401, "未登录或会话已过期");
        return 1;                            /* 非 0:civetweb 放弃握手 */
    }
    return 0;
}

static void ws_ready(struct mg_connection *conn, void *ud)
{
    (void)ud;
    pthread_mutex_lock(&s_ws_mtx);
    for (int i = 0; i < WS_MAX_CONN; i++) {
        if (!s_ws[i].used) {
            s_ws[i].used = true;
            s_ws[i].conn = conn;
            break;
        }
    }
    pthread_mutex_unlock(&s_ws_mtx);
    DG_LOGI(TAG, "WebSocket 已连接");
}

static int ws_data(struct mg_connection *conn, int bits, char *data, size_t len,
                   void *ud)
{
    (void)conn; (void)bits; (void)data; (void)len; (void)ud;
    /* 推送不依赖客户端消息;这里只需保持连接(返回 1) */
    return 1;
}

static void ws_close(const struct mg_connection *conn, void *ud)
{
    (void)ud;
    pthread_mutex_lock(&s_ws_mtx);
    for (int i = 0; i < WS_MAX_CONN; i++) {
        if (s_ws[i].used && s_ws[i].conn == conn) {
            s_ws[i].used = false;
            s_ws[i].conn = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&s_ws_mtx);
    DG_LOGI(TAG, "WebSocket 已断开");
}

/* ---- HTTP 响应工具 ---- */

static const char *reason_phrase(int code)
{
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static void http_send(struct mg_connection *conn, int code, const char *ctype,
                      const char *body, size_t len, const char *extra_hdr)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s"
                     "Connection: close\r\n\r\n",
                     code, reason_phrase(code), ctype, len,
                     extra_hdr ? extra_hdr : "");
    if (n > 0)
        mg_write(conn, hdr, (size_t)n);
    if (body && len)
        mg_write(conn, body, len);
}

static void json_reply(struct mg_connection *conn, int code, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (!s) {
        http_send(conn, 500, "application/json; charset=utf-8",
                  "{\"msg\":\"内部错误\"}", strlen("{\"msg\":\"内部错误\"}"), NULL);
        return;
    }
    http_send(conn, code, "application/json; charset=utf-8", s, strlen(s), NULL);
    free(s);
}

static void json_msg(struct mg_connection *conn, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msg", msg ? msg : "");
    json_reply(conn, code, root);
    cJSON_Delete(root);
}

static const char *req_method(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    return (ri && ri->request_method) ? ri->request_method : "";
}

static bool method_is(struct mg_connection *conn, const char *want)
{
    return strcmp(req_method(conn), want) == 0;
}

static const char *remote_ip(struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    return (ri && ri->remote_addr) ? ri->remote_addr : "-";
}

/* ---- 鉴权 ---- */

static const char *header_or_query_token(struct mg_connection *conn, char *buf,
                                         size_t cap)
{
    const char *tok = mg_get_header(conn, "X-Auth-Token");
    if (tok && tok[0])
        return tok;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (ri && ri->query_string &&
        mg_get_var(ri->query_string, strlen(ri->query_string), "token", buf, cap) > 0 &&
        buf[0])
        return buf;
    return NULL;
}

static bool check_token(struct mg_connection *conn)
{
    char buf[64];
    const char *tok = header_or_query_token(conn, buf, sizeof(buf));
    return web_session_validate(tok, time(NULL));
}

/* 未授权统一回复 */
static void reply_unauthorized(struct mg_connection *conn)
{
    json_msg(conn, 401, "未登录或会话已过期,请重新登录");
}

/* ---- 登录 / 注销 ---- */

static int handle_login(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "POST")) {
        json_msg(conn, 405, "登录接口只接受 POST");
        return 200;
    }
    char body[256] = { 0 };
    int n = mg_read(conn, body, sizeof(body) - 1);
    if (n <= 0) {
        json_msg(conn, 400, "坏请求");
        return 200;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        json_msg(conn, 400, "坏 json");      /* 坏 json 不崩 */
        return 200;
    }
    cJSON *ju = cJSON_GetObjectItem(j, "user");
    cJSON *jp = cJSON_GetObjectItem(j, "pwd");
    const char *user = cJSON_IsString(ju) ? ju->valuestring : "";
    const char *pwd = cJSON_IsString(jp) ? jp->valuestring : "";
    const char *ip = remote_ip(conn);

    int retry = 0;
    if (web_auth_login_blocked(ip, &retry)) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "Retry-After: %d\r\n", retry > 0 ? retry : 1);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "msg", "尝试次数过多,请稍后再试");
        cJSON_AddNumberToObject(root, "retry_after_s", retry);
        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            http_send(conn, 429, "application/json; charset=utf-8", s, strlen(s), hdr);
            free(s);
        }
        cJSON_Delete(root);
        cJSON_Delete(j);
        return 200;
    }

    if (web_auth_verify(user, pwd) != DG_OK) {
        bool locked = web_auth_login_fail(ip);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "msg",
                                locked ? "尝试次数过多,已临时锁定"
                                       : "账号或密码错误");
        cJSON_AddBoolToObject(root, "locked", locked);
        json_reply(conn, 401, root);
        cJSON_Delete(root);
        cJSON_Delete(j);
        return 200;
    }

    web_auth_login_ok(ip);
    char token[WEB_TOKEN_LEN + 1] = "";
    int expires_in = 0;
    if (web_session_create(token, sizeof(token), &expires_in) != DG_OK) {
        json_msg(conn, 500, "会话创建失败");
        cJSON_Delete(j);
        return 200;
    }

    char userbuf[WEB_AUTH_USER_MAX] = "";
    web_auth_get_user(userbuf, sizeof(userbuf));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token", token);
    cJSON_AddNumberToObject(root, "expires_in", expires_in);
    cJSON_AddStringToObject(root, "user", userbuf);
    cJSON_AddBoolToObject(root, "pwd_default", web_auth_is_default());
    json_reply(conn, 200, root);
    cJSON_Delete(root);
    cJSON_Delete(j);
    DG_LOGI(TAG, "登录成功(来源 %s,账号 %s)", ip, userbuf);
    return 200;
}

static int handle_logout(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "POST")) {
        json_msg(conn, 405, "注销接口只接受 POST");
        return 200;
    }
    char buf[64];
    const char *tok = header_or_query_token(conn, buf, sizeof(buf));
    if (!web_session_validate(tok, time(NULL))) {
        reply_unauthorized(conn);
        return 200;
    }
    web_session_revoke(tok);
    json_msg(conn, 200, "已注销");
    return 200;
}

/* ---- 设备信息 ---- */

static int64_t read_uptime_s(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        return (int64_t)(time(NULL) - s_started_at);
    double up = 0;
    int got = fscanf(f, "%lf", &up);
    fclose(f);
    if (got != 1)
        return (int64_t)(time(NULL) - s_started_at);
    return (int64_t)up;
}

static void fmt_uptime(int64_t sec, char *out, size_t cap)
{
    if (sec < 60)
        snprintf(out, cap, "%lld 秒", (long long)sec);
    else if (sec < 3600)
        snprintf(out, cap, "%lld 分 %lld 秒", (long long)(sec / 60),
                 (long long)(sec % 60));
    else if (sec < 86400)
        snprintf(out, cap, "%lld 小时 %lld 分", (long long)(sec / 3600),
                 (long long)((sec % 3600) / 60));
    else
        snprintf(out, cap, "%lld 天 %lld 小时", (long long)(sec / 86400),
                 (long long)((sec % 86400) / 3600));
}

static int handle_device(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "GET")) {
        json_msg(conn, 405, "设备信息只接受 GET");
        return 200;
    }
    if (!check_token(conn)) {
        reply_unauthorized(conn);
        return 200;
    }
    uint32_t users = 0;
    db_user_count(&users);
    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1;
    q.page_size = 1;
    access_log_t row;
    log_page_t page = { .logs = &row, .max = 1 };
    db_log_query(&q, &page);

    char ip[64] = "";
    char ifname[32] = "";
    bool have_ip = (net_info_primary_ipv4(ip, sizeof(ip)) == DG_OK);
    net_info_primary_ifname(ifname, sizeof(ifname));

    char host[64] = "", url[128] = "";
    mdns_hostname(host, sizeof(host));
    mdns_url(url, sizeof(url));

    char userbuf[WEB_AUTH_USER_MAX] = "";
    web_auth_get_user(userbuf, sizeof(userbuf));

    int64_t up = read_uptime_s();
    char uptext[48];
    fmt_uptime(up, uptext, sizeof(uptext));

    /* 存储占用:库大小 + 分区余量。路径归 storage 管,这里不硬编码
     * (模拟器/板上库路径不同,写死会把上位机显示变成假数据) */
    const dg_cfg_t *cfg = cfg_get();
    uint64_t db_bytes = 0, disk_free = 0;
    db_storage_stats(&db_bytes, &disk_free);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", DG_FW_VERSION);
    cJSON_AddNumberToObject(root, "uptime_s", (double)up);
    cJSON_AddStringToObject(root, "uptime_text", uptext);
    cJSON_AddNumberToObject(root, "users", (double)users);
    cJSON_AddNumberToObject(root, "log_total", (double)page.total);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ifname", ifname);
    cJSON_AddBoolToObject(root, "have_ip", have_ip);
    cJSON_AddStringToObject(root, "mdns_host", host);
    cJSON_AddStringToObject(root, "mdns_url", url);
    cJSON_AddBoolToObject(root, "mdns_running", mdns_running());
    cJSON_AddBoolToObject(root, "online", net_info_is_online());
    cJSON_AddNumberToObject(root, "web_port", cfg ? cfg->web_port : 8080);
    cJSON_AddStringToObject(root, "web_user", userbuf);
    cJSON_AddBoolToObject(root, "pwd_default", web_auth_is_default());
    cJSON_AddNumberToObject(root, "sessions", web_session_count(time(NULL)));
    cJSON_AddNumberToObject(root, "db_bytes", (double)db_bytes);
    cJSON_AddNumberToObject(root, "disk_free_bytes", (double)disk_free);

    cJSON *ntp = cJSON_AddObjectToObject(root, "ntp");
    cJSON_AddBoolToObject(ntp, "ok", s_ntp_ok);
    char ntpbuf[32] = "";
    if (s_ntp_ts > 0) {
        time_t ts = (time_t)s_ntp_ts;
        struct tm tmv;
        localtime_r(&ts, &tmv);
        strftime(ntpbuf, sizeof(ntpbuf), "%Y-%m-%d %H:%M:%S", &tmv);
    }
    cJSON_AddStringToObject(ntp, "last_ok_at", ntpbuf);

    json_reply(conn, 200, root);
    cJSON_Delete(root);
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

static int parse_date(const char *s, bool end_of_day, int64_t *out)
{
    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
        return DG_ERR_PARAM;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31)
        return DG_ERR_PARAM;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d + (end_of_day ? 1 : 0);   /* 结束日含当天:算到次日 0 点前 */
    *out = (int64_t)mktime(&tmv) - (end_of_day ? 1 : 0);
    return DG_OK;
}

static int handle_logs(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "GET")) {
        json_msg(conn, 405, "日志查询只接受 GET");
        return 200;
    }
    if (!check_token(conn)) {
        reply_unauthorized(conn);
        return 200;
    }

    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1;
    q.page_size = 20;
    q.descending = true;

    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *qs = ri ? ri->query_string : NULL;
    char v[96];
    if (qs && mg_get_var(qs, strlen(qs), "from", v, sizeof(v)) > 0 && v[0]) {
        if (parse_date(v, false, &q.ts_from) != DG_OK) {
            json_msg(conn, 400, "日期格式应为 YYYY-MM-DD");
            return 200;
        }
    }
    if (qs && mg_get_var(qs, strlen(qs), "to", v, sizeof(v)) > 0 && v[0]) {
        if (parse_date(v, true, &q.ts_to) != DG_OK) {
            json_msg(conn, 400, "日期格式应为 YYYY-MM-DD");
            return 200;
        }
    }
    if (qs && mg_get_var(qs, strlen(qs), "user_id", v, sizeof(v)) > 0 && v[0]) {
        size_t ulen = strlen(v);
        if (ulen >= sizeof(q.user_id)) {
            json_msg(conn, 400, "用户 ID 过长");
            return 200;
        }
        memcpy(q.user_id, v, ulen + 1);   /* 长度已校验,避免 snprintf 截断告警 */
    }
    if (qs && mg_get_var(qs, strlen(qs), "page", v, sizeof(v)) > 0 && v[0]) {
        int pg = atoi(v);
        if (pg < 1) {
            json_msg(conn, 400, "坏参数:page");
            return 200;
        }
        q.page = (uint32_t)pg;
    }
    if (qs && mg_get_var(qs, strlen(qs), "page_size", v, sizeof(v)) > 0 && v[0]) {
        int ps = atoi(v);
        if (ps < 1 || ps > LOG_PAGE_MAX) {
            json_msg(conn, 400, "坏参数:page_size");
            return 200;
        }
        q.page_size = (uint32_t)ps;
    }

    access_log_t rows[LOG_PAGE_MAX];
    log_page_t out = { .logs = rows, .max = LOG_PAGE_MAX };
    if (db_log_query(&q, &out) != DG_OK) {
        json_msg(conn, 400, "坏参数");
        return 200;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "page", (double)q.page);
    uint32_t pages = (out.total + q.page_size - 1) / q.page_size;
    cJSON_AddNumberToObject(root, "pages", (double)pages);
    cJSON_AddNumberToObject(root, "total", (double)out.total);
    cJSON *arr = cJSON_AddArrayToObject(root, "logs");
    for (uint32_t i = 0; i < out.count; i++) {
        cJSON *o = cJSON_CreateObject();
        time_t ts = (time_t)rows[i].ts;
        struct tm tmv;
        localtime_r(&ts, &tmv);
        char tsbuf[24];
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv);
        cJSON_AddStringToObject(o, "time", tsbuf);
        cJSON_AddNumberToObject(o, "ts", (double)rows[i].ts);
        /* has_user=0 的日志没有用户(陌生人):用空串而不是 "-",
         * 前端按 result 着色,不靠字符串判断 */
        cJSON_AddStringToObject(o, "user_id",
                                rows[i].has_user ? rows[i].user_id : "");
        cJSON_AddStringToObject(o, "user_name",
                                rows[i].has_user ? rows[i].user_name : "陌生人");
        cJSON_AddStringToObject(o, "method_name", method_name(rows[i].method));
        cJSON_AddNumberToObject(o, "method", rows[i].method);
        cJSON_AddNumberToObject(o, "result", rows[i].result);
        cJSON_AddItemToArray(arr, o);
    }
    json_reply(conn, 200, root);
    cJSON_Delete(root);
    return 200;
}

/* ---- NTP(异步:chronyc waitsync 可阻塞十余秒,不能占住 worker 线程) ---- */

static pthread_mutex_t s_ntp_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_ntp_running = false;

static void *ntp_thread(void *arg)
{
    (void)arg;
    ntp_service_trigger();                   /* 结果经 EV_NET_NTP_RESULT 广播 */
    pthread_mutex_lock(&s_ntp_mtx);
    s_ntp_running = false;
    pthread_mutex_unlock(&s_ntp_mtx);
    return NULL;
}

static int handle_ntp(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "POST")) {
        json_msg(conn, 405, "时间校正只接受 POST");
        return 200;
    }
    if (!check_token(conn)) {
        reply_unauthorized(conn);
        return 200;
    }

    pthread_mutex_lock(&s_ntp_mtx);
    if (s_ntp_running) {
        pthread_mutex_unlock(&s_ntp_mtx);
        json_msg(conn, 409, "校正进行中,请稍候");
        return 200;
    }
    s_ntp_running = true;
    pthread_mutex_unlock(&s_ntp_mtx);

    pthread_t tid;
    if (pthread_create(&tid, NULL, ntp_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        pthread_mutex_lock(&s_ntp_mtx);
        s_ntp_running = false;
        pthread_mutex_unlock(&s_ntp_mtx);
        json_msg(conn, 500, "校正线程创建失败");
        return 200;
    }
    /* 202:已受理,结果走 WebSocket 推送(EV_NET_NTP_RESULT) */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "pending", true);
    cJSON_AddStringToObject(root, "note", "结果将经 WebSocket 推送");
    json_reply(conn, 202, root);
    cJSON_Delete(root);
    return 200;
}

/* ---- 账号/口令 ---- */

static int handle_account(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "POST")) {
        json_msg(conn, 405, "账号修改只接受 POST");
        return 200;
    }
    if (!check_token(conn)) {
        reply_unauthorized(conn);
        return 200;
    }
    char body[512] = { 0 };
    if (mg_read(conn, body, sizeof(body) - 1) <= 0) {
        json_msg(conn, 400, "坏请求");
        return 200;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        json_msg(conn, 400, "坏 json");
        return 200;
    }
    cJSON *jo = cJSON_GetObjectItem(j, "old_pwd");
    cJSON *ju = cJSON_GetObjectItem(j, "user");
    cJSON *jp = cJSON_GetObjectItem(j, "pwd");
    const char *old_pwd = cJSON_IsString(jo) ? jo->valuestring : "";
    const char *user = cJSON_IsString(ju) ? ju->valuestring : "";
    const char *pwd = cJSON_IsString(jp) ? jp->valuestring : "";

    char cur[WEB_AUTH_USER_MAX] = "";
    web_auth_get_user(cur, sizeof(cur));
    if (user[0] == '\0')
        user = cur;                          /* 只改口令:沿用当前账号 */

    int rc;
    if (strcmp(user, cur) == 0)
        rc = web_auth_change_pwd(old_pwd, pwd);   /* 账号没变:必须验旧口令 */
    else {
        /* 换账号也要旧口令:否则拿到 token 就能改掉别人账号 */
        if (web_auth_verify(cur, old_pwd) != DG_OK)
            rc = DG_ERR_WRONG_PASSWORD;
        else
            rc = web_auth_set(user, pwd);
    }

    cJSON_Delete(j);
    cJSON *root = cJSON_CreateObject();
    if (rc == DG_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "msg", "已保存,请用新凭据重新登录");
        json_reply(conn, 200, root);
        DG_LOGI(TAG, "web 凭据已由 web 端修改(来源 %s)", remote_ip(conn));
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        if (rc == DG_ERR_BAD_UID)
            cJSON_AddStringToObject(root, "msg",
                                    "账号需 3~31 位字母、数字、- 或 _,且以字母或数字开头");
        else if (rc == DG_ERR_BAD_PWD)
            cJSON_AddStringToObject(root, "msg", "口令需 4~31 位,不能含空格");
        else {
            cJSON_AddStringToObject(root, "msg", "旧口令错误或保存失败");
            rc = DG_ERR_WRONG_PASSWORD;
        }
        cJSON_AddNumberToObject(root, "err", rc);
        json_reply(conn, 400, root);
    }
    cJSON_Delete(root);
    return 200;
}

/* ---- OTA 上传 ---- */

static int handle_ota(struct mg_connection *conn, void *ud)
{
    (void)ud;
    if (!method_is(conn, "POST")) {
        json_msg(conn, 405, "OTA 上传只接受 POST");
        return 200;
    }
    if (!check_token(conn)) {
        reply_unauthorized(conn);
        return 200;
    }
    const char *ver = mg_get_header(conn, "X-OTA-Version");
    const char *size_s = mg_get_header(conn, "X-OTA-Size");
    const char *sha = mg_get_header(conn, "X-OTA-SHA256");
    const char *off_s = mg_get_header(conn, "X-OTA-Offset");
    if (!ver || !size_s || !sha) {
        json_msg(conn, 400, "缺少 manifest 头(X-OTA-Version/-Size/-SHA256)");
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
        json_msg(conn, 400, "包大小超限或参数非法");
        return 200;
    }
    if (rc == DG_ERR_STATE) {
        json_msg(conn, 409, "续传偏移不符,请重传");
        return 200;
    }
    if (rc == DG_ERR_BUSY) {
        json_msg(conn, 409, "其他上传进行中");
        return 200;
    }
    if (rc != DG_OK) {
        json_msg(conn, 500, "开始失败");
        return 200;
    }

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
            json_msg(conn, 400, "写入失败(超大小?)");
            return 200;
        }
        remain -= n;
    }

    char path[64];
    rc = ota_finish(path, sizeof(path));
    if (rc != DG_OK) {
        json_msg(conn, 422, "校验失败(sha256/大小不符)");
        return 200;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "staged", path);
    cJSON_AddBoolToObject(root, "resumed", resumed);
    json_reply(conn, 200, root);
    cJSON_Delete(root);
    DG_LOGI(TAG, "OTA 包已暂存: %s(版本 %s)", path, ver);
    return 200;
}

/* ---- 静态资源(内嵌前端产物) ---- */

/* 按**精确路径**查资源表(pages/ 由 gen_pages.sh 生成,见 web_pages.h) */
static const dg_web_asset_t *asset_lookup(const char *path)
{
    if (!path)
        return NULL;
    for (int i = 0; i < DG_WEB_ASSET_COUNT; i++) {
        if (strcmp(DG_WEB_ASSETS[i].path, path) == 0)
            return &DG_WEB_ASSETS[i];
    }
    return NULL;
}

/* 兜底路由(注册在 "/"):
 * civetweb 的匹配顺序是"精确 → 路径前缀 → 模式",而 "/" 作为模式能匹配
 * 任何 URI——所以它只能承担兜底,不能用它注册具体接口;反过来也意味着
 * 未知路径都会落到这里,由本函数分派:资源表命中则返回资源,否则单页应用
 * 回退(hash 路由下前端自己处理路径),/api/ 前缀回 JSON 404。
 * (历史坑:把 /api 前缀当 404 处理器单独注册会永远命中不到,因为 "/" 先命中。) */
static int handle_static(struct mg_connection *conn, void *ud)
{
    (void)ud;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = (ri && ri->local_uri) ? ri->local_uri : "";

    const dg_web_asset_t *asset = asset_lookup(uri);
    if (asset) {
        http_send(conn, 200, asset->mime, asset->data, asset->len, NULL);
        return 200;
    }
    if (strncmp(uri, "/api/", 5) == 0) {
        json_msg(conn, 404, "接口不存在");
        return 200;
    }
    /* 非资源、非接口:交给单页应用(前端 hash 路由自行决定显示什么) */
    http_send(conn, 200, "text/html; charset=utf-8", DG_WEB_INDEX_HTML,
              strlen(DG_WEB_INDEX_HTML), NULL);
    return 200;
}

/* ---- 事件 → WebSocket ---- */

static void fmt_ts(int64_t ts, char *out, size_t cap)
{
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* 用 cJSON 拼消息:用户姓名可能是任意 UTF-8(含引号/反斜杠),
 * 手写 snprintf 拼 JSON 会把日志推成坏 JSON(实测坑) */
static void ws_push_auth(const ev_auth_result_t *r)
{
    char tsbuf[32];
    fmt_ts(r->ts, tsbuf, sizeof(tsbuf));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "auth");
    cJSON_AddStringToObject(root, "time", tsbuf);
    cJSON_AddNumberToObject(root, "ts", (double)r->ts);
    cJSON_AddStringToObject(root, "user_id", r->user_id);
    cJSON_AddStringToObject(root, "user_name", r->user_name);
    cJSON_AddNumberToObject(root, "method", r->method);
    cJSON_AddStringToObject(root, "method_name", method_name(r->method));
    cJSON_AddNumberToObject(root, "result", r->result);
    cJSON_AddStringToObject(root, "result_name",
                            r->result == DG_RESULT_PASS ? "通过" : "拒绝");
    cJSON_AddNumberToObject(root, "reason", r->reason);
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        ws_enqueue(s);
        free(s);
    }
    cJSON_Delete(root);
}

static int on_auth_result(const event_t *e, void *ud)
{
    (void)ud;
    ws_push_auth((const ev_auth_result_t *)e->data);
    return 0;
}

static int on_ntp_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ntp_result_t *r = (const ev_ntp_result_t *)e->data;
    s_ntp_ok = r->ok;
    s_ntp_ts = r->ok ? r->synced_ts : 0;

    char tsbuf[32] = "";
    if (r->ok)
        fmt_ts(r->synced_ts, tsbuf, sizeof(tsbuf));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ntp");
    cJSON_AddBoolToObject(root, "ok", r->ok);
    cJSON_AddStringToObject(root, "time", tsbuf);
    cJSON_AddStringToObject(root, "msg",
                            r->ok ? "时间校正成功" : "时间校正失败(检查网络/时间服务器)");
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        ws_enqueue(s);
        free(s);
    }
    cJSON_Delete(root);
    return 0;
}

/* ---- 设备页 ↔ net:web 账号状态与设置 ---- */

static void publish_web_state(void)
{
    ev_web_state_t st;
    memset(&st, 0, sizeof(st));
    mdns_url(st.url, sizeof(st.url));                  /* 直接写进事件载荷 */
    mdns_hostname(st.host, sizeof(st.host));
    web_auth_get_user(st.user, sizeof(st.user));
    st.pwd_default = web_auth_is_default();
    st.running = (s_ctx != NULL);
    EVENT_BUS_PUBLISH(EV_NET_WEB_STATE, &st);
}

static int on_web_state_req(const event_t *e, void *ud)
{
    (void)e; (void)ud;
    publish_web_state();
    return 0;
}

static int on_web_set(const event_t *e, void *ud)
{
    (void)ud;
    const ev_web_set_t *req = (const ev_web_set_t *)e->data;
    ev_web_set_result_t res;
    memset(&res, 0, sizeof(res));

    char cur[WEB_AUTH_USER_MAX] = "";
    web_auth_get_user(cur, sizeof(cur));
    const char *user = req->user[0] ? req->user : cur;
    res.err = web_auth_set(user, req->pwd);   /* 空口令/非法账号由内部规则拒绝 */
    res.ok = (res.err == DG_OK);
    if (res.ok) {
        DG_LOGI(TAG, "web 凭据已由设备菜单修改(账号 %s)", user);
        publish_web_state();                  /* 名称/默认口令标记可能变了 */
    }
    EVENT_BUS_PUBLISH(EV_NET_WEB_SET_RESULT, &res);
    return 0;
}

/* ---- 生命周期 ---- */

int web_server_start(void)
{
    if (s_ctx)
        return DG_OK;

    s_started_at = time(NULL);
    memset(s_ws, 0, sizeof(s_ws));

    if (web_auth_ensure() != DG_OK) {
        DG_LOGE(TAG, "凭据初始化失败(DB 不可用?),web 上位机不启动");
        return DG_ERR_IO;
    }


    const dg_cfg_t *cfg = cfg_get();
    int port = (cfg && cfg->web_port > 0) ? cfg->web_port : 8080;
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);

    const char *opts[] = {
        "listening_ports", port_s,
        "num_threads", "4",
        "request_timeout_ms", "10000",
        NULL,
    };
    s_ctx = mg_start(NULL, NULL, opts);
    if (!s_ctx) {
        DG_LOGE(TAG, "web 启动失败(端口 %d 被占?)", port);
        return DG_ERR_IO;
    }

    /* 静态资源与单页应用共用 "/" 兜底处理器(见 handle_static 注释) */
    mg_set_request_handler(s_ctx, "/", handle_static, NULL);
    mg_set_request_handler(s_ctx, "/api/login", handle_login, NULL);
    mg_set_request_handler(s_ctx, "/api/logout", handle_logout, NULL);
    mg_set_request_handler(s_ctx, "/api/device", handle_device, NULL);
    mg_set_request_handler(s_ctx, "/api/logs", handle_logs, NULL);
    mg_set_request_handler(s_ctx, "/api/ntp", handle_ntp, NULL);
    mg_set_request_handler(s_ctx, "/api/account", handle_account, NULL);
    mg_set_request_handler(s_ctx, "/api/ota/upload", handle_ota, NULL);
    mg_set_websocket_handler(s_ctx, "/api/ws", ws_connect, ws_ready, ws_data,
                             ws_close, NULL);

    s_sub_cnt = 0;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_NTP_RESULT, on_ntp_result, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_WEB_STATE_REQ, on_web_state_req, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_WEB_SET, on_web_set, NULL);

    pthread_mutex_lock(&s_ws_mtx);
    s_q_head = s_q_tail = 0;
    s_q_dropped = 0;
    s_pusher_run = true;
    pthread_mutex_unlock(&s_ws_mtx);
    if (pthread_create(&s_pusher, NULL, ws_pusher_thread, NULL) != 0) {
        DG_LOGE(TAG, "推送线程创建失败:实时事件将不可用");
        s_pusher_run = false;
    }

    publish_web_state();
    DG_LOGI(TAG, "web 上位机就绪 :%d(版本 %s)", port, DG_FW_VERSION);
    return DG_OK;
}

void web_server_stop(void)
{
    for (int i = 0; i < s_sub_cnt; i++)
        event_bus_unsubscribe(s_subs[i]);
    s_sub_cnt = 0;

    pthread_mutex_lock(&s_ws_mtx);
    if (s_pusher_run) {
        s_pusher_run = false;
        pthread_cond_broadcast(&s_ws_cond);
        pthread_mutex_unlock(&s_ws_mtx);
        pthread_join(s_pusher, NULL);
    } else {
        pthread_mutex_unlock(&s_ws_mtx);
    }

    if (s_ctx) {
        mg_stop(s_ctx);
        s_ctx = NULL;
    }
    web_session_revoke_all();
}
