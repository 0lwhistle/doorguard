/*
 * web_server.c — 内嵌 web 上位机实现(mongoose 统一事件循环)
 *
 * 传输层跑在 modules/net/netcore 的单 loop 线程上(2026-09-22 起替代
 * civetweb 4-worker 线程池):所有连接只在 loop 线程读写,总线回调等
 * 其他线程经 netcore_post 投递——跨线程碰连接的数据竞争从结构上排除
 * (旧"推送线程 + 连接表锁 + 连接锁"协议随之废除)。
 *
 * 路由表(method 严格校验:改状态的接口不接受 GET——原来 GET /api/ntp
 * 也能触发联网校时,属错误面过宽的坑):
 *   GET  /                单页应用(hash 路由回退;CSS/JS 见 /assets 目录)
 *   GET  /assets/...      构建产物(资源表按精确路径命中)
 *   POST /api/login       登录 → {token,user,pwd_default,expires_in}
 *   POST /api/logout      注销当前 token
 *   GET  /api/device      设备信息(版本/运行时长/用户数/日志数/IP/mDNS/账号/NTP)
 *   GET  /api/logs        门禁日志查询(时间段 + 用户 ID + 分页,JSON)
 *   POST /api/ntp         触发一次 NTP 校正(异步,结果走 WebSocket)
 *   GET  /api/network     网络配置快照(接口/IP/掩码/网关/模式;未拿到=0.0.0.0)
 *   POST /api/network     应用网络配置(DHCP 或 静态 IP/掩码/网关;持久化+后台应用)
 *   POST /api/account     改账号/口令(需旧口令;成功后所有会话失效)
 *   POST /api/ota/upload  OTA 包流式接收(MG_EV_HTTP_HDRS + MG_EV_READ 喂入,
 *                         按 ota_can_accept() 限流,绝不阻塞 loop;见 ota_service.h)
 *   GET  /api/ws          WebSocket:实时推送认证事件/NTP 结果/网络地址变化
 *
 * 地址变化监视:loop 里 5s 定时轮询 net_info_read(纯 getifaddrs,无阻塞),
 * 与上次快照不同才发布 EV_NET_ADDR——DHCP 续租换址/静态应用后,上位机经
 * WebSocket 实时收到新地址,不用等 30s 轮询。
 *
 * WebSocket 推送模型:总线回调只把消息入队(总线线程绝不碰连接),随后
 * netcore_post 让 loop 线程排空队列并逐连接 mg_ws_send——与旧"civetweb
 * 推送线程"行为等价,但队列消费方就是连接的唯一合法访问线程,连表无锁。
 *
 * OTA 流式收包:mongoose 对收包不背压(不消费 recv 它也会继续读到
 * MG_MAX_RECV_SIZE 上限后断连),所以按 can_accept 限流喂入;极端慢盘时
 * 最多缓冲 3MB 由 mongoose 显式断连,客户端可用 X-OTA-Offset 断点续传
 * (有界失败 + 可恢复,优于秒级阻塞整个 loop)。
 */
#include "web_server.h"
#include "web_auth.h"
#include "web_session.h"
#include "web_pages.h"
#include "ota/ota_service.h"
#include "ntp/ntp_service.h"
#include "net_info.h"
#include "net_cfg.h"
#include "mdns/mdns_responder.h"#include "netcore.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "cfg.h"
#include "storage.h"
#include "valid.h"

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
#define LOG_PAGE_MAX     100         /* 单页上限:别让一次查询拖住唯一 loop */

static bool s_started = false;
static struct mg_connection *s_lsn = NULL;   /* HTTP 监听(loop 线程私有) */
static event_subscription_t *s_subs[8];
static int s_sub_cnt = 0;
static time_t s_started_at = 0;
static int s_port = 8080;

/* 最近一次 NTP 结果(设备信息里显示"已同步/未同步 + 时间") */
static bool s_ntp_ok = false;
static int64_t s_ntp_ts = 0;

/* ---- WebSocket 连接表(loop 线程私有,无锁)+ 推送队列(跨线程,互斥) ---- */

static struct mg_connection *s_ws[WS_MAX_CONN];

static char s_queue[WS_QUEUE][WS_MSG_MAX];
static int s_q_head = 0, s_q_tail = 0;
static int s_q_dropped = 0;
static pthread_mutex_t s_ws_mtx = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_unlock(&s_ws_mtx);
}

/* loop 线程(netcore_post 闭包):排空队列,逐连接推送。
 * 连接表只在 loop 线程读写,CLOSE 事件已把死连接摘除,发送天然安全 */
static void ws_drain_cb(void *arg)
{
    (void)arg;
    char msg[WS_MSG_MAX];
    pthread_mutex_lock(&s_ws_mtx);
    bool have = (s_q_tail != s_q_head);
    if (have) {
        snprintf(msg, sizeof(msg), "%s", s_queue[s_q_tail]);
        s_q_tail = (s_q_tail + 1) % WS_QUEUE;
    }
    pthread_mutex_unlock(&s_ws_mtx);

    if (!have)
        return;
    for (int i = 0; i < WS_MAX_CONN; i++) {
        if (s_ws[i])
            mg_ws_send(s_ws[i], msg, strlen(msg), WEBSOCKET_OP_TEXT);
    }
}

static void ws_push_async(void)
{
    netcore_post(ws_drain_cb, NULL);
}

static void ws_add(struct mg_connection *c)
{
    for (int i = 0; i < WS_MAX_CONN; i++) {
        if (!s_ws[i]) {
            s_ws[i] = c;
            DG_LOGI(TAG, "WebSocket 已连接");
            return;
        }
    }
    DG_LOGW(TAG, "WebSocket 连接数达上限,拒绝");   /* 表满:直接关 */
    c->is_draining = 1;
}

static void ws_remove(struct mg_connection *c)
{
    for (int i = 0; i < WS_MAX_CONN; i++) {
        if (s_ws[i] == c) {
            s_ws[i] = NULL;
            DG_LOGI(TAG, "WebSocket 已断开");
            return;
        }
    }
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

static void http_send(struct mg_connection *c, int code, const char *ctype,
                      const char *body, size_t len, const char *extra_hdr)
{
    mg_printf(c,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %zu\r\n"
              "X-Content-Type-Options: nosniff\r\n"
              "Cache-Control: no-store\r\n"
              "%s"
              "Connection: close\r\n\r\n",
              code, reason_phrase(code), ctype, len,
              extra_hdr ? extra_hdr : "");
    if (body && len)
        mg_send(c, body, len);
    c->is_draining = 1;                  /* 发完即关:与旧行为一致,无连接复用 */
}

static void json_reply(struct mg_connection *c, int code, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (!s) {
        http_send(c, 500, "application/json; charset=utf-8",
                  "{\"msg\":\"内部错误\"}", strlen("{\"msg\":\"内部错误\"}"), NULL);
        return;
    }
    http_send(c, code, "application/json; charset=utf-8", s, strlen(s), NULL);
    free(s);
}

static void json_msg(struct mg_connection *c, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "msg", msg ? msg : "");
    json_reply(c, code, root);
    cJSON_Delete(root);
}

/* 来源 IPv4(风控按键值;mongoose 无现成字符串,自行展开) */
static const char *remote_ip(struct mg_connection *c, char *buf, size_t cap)
{
    const uint8_t *p = c->rem.addr.ip;
    snprintf(buf, cap, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

static bool method_is(struct mg_http_message *hm, const char *want)
{
    return mg_strcmp(hm->method, mg_str(want)) == 0;
}

static bool uri_is(struct mg_http_message *hm, const char *want)
{
    return mg_strcmp(hm->uri, mg_str(want)) == 0;
}

/* ---- 鉴权 ---- */

/* 浏览器 WebSocket 不能自定义请求头 → token 走查询串;其余接口 X-Auth-Token。
 * mongoose 的头值不是 NUL 结尾,必须先拷贝再用 */
static const char *header_or_query_token(struct mg_http_message *hm, char *buf,
                                         size_t cap)
{
    struct mg_str *h = mg_http_get_header(hm, "X-Auth-Token");
    if (h && h->len > 0 && h->len < cap) {
        memcpy(buf, h->buf, h->len);
        buf[h->len] = '\0';
        return buf;
    }
    if (mg_http_get_var(&hm->query, "token", buf, cap) > 0 && buf[0])
        return buf;
    return NULL;
}

static bool check_token(struct mg_http_message *hm)
{
    char buf[64];
    const char *tok = header_or_query_token(hm, buf, sizeof(buf));
    return web_session_validate(tok, time(NULL));
}

/* 未授权统一回复 */
static void reply_unauthorized(struct mg_connection *c)
{
    json_msg(c, 401, "未登录或会话已过期,请重新登录");
}

/* 请求体拷贝(JSON 解析需要 NUL 结尾;超限拒绝) */
static bool body_copy(struct mg_http_message *hm, char *buf, size_t cap)
{
    if (hm->body.len == 0 || hm->body.len >= cap)
        return false;
    memcpy(buf, hm->body.buf, hm->body.len);
    buf[hm->body.len] = '\0';
    return true;
}

/* ---- 登录 / 注销 ---- */

static void handle_login(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[256] = { 0 };
    if (!body_copy(hm, body, sizeof(body))) {
        json_msg(c, 400, "坏请求");
        return;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        json_msg(c, 400, "坏 json");      /* 坏 json 不崩 */
        return;
    }
    cJSON *ju = cJSON_GetObjectItem(j, "user");
    cJSON *jp = cJSON_GetObjectItem(j, "pwd");
    const char *user = cJSON_IsString(ju) ? ju->valuestring : "";
    const char *pwd = cJSON_IsString(jp) ? jp->valuestring : "";
    char ipbuf[48];
    const char *ip = remote_ip(c, ipbuf, sizeof(ipbuf));

    int retry = 0;
    if (web_auth_login_blocked(ip, &retry)) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "Retry-After: %d\r\n", retry > 0 ? retry : 1);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "msg", "尝试次数过多,请稍后再试");
        cJSON_AddNumberToObject(root, "retry_after_s", retry);
        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            http_send(c, 429, "application/json; charset=utf-8", s, strlen(s), hdr);
            free(s);
        }
        cJSON_Delete(root);
        cJSON_Delete(j);
        return;
    }

    if (web_auth_verify(user, pwd) != DG_OK) {
        bool locked = web_auth_login_fail(ip);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "msg",
                                locked ? "尝试次数过多,已临时锁定"
                                       : "账号或密码错误");
        cJSON_AddBoolToObject(root, "locked", locked);
        json_reply(c, 401, root);
        cJSON_Delete(root);
        cJSON_Delete(j);
        return;
    }

    web_auth_login_ok(ip);
    char token[WEB_TOKEN_LEN + 1] = "";
    int expires_in = 0;
    if (web_session_create(token, sizeof(token), &expires_in) != DG_OK) {
        json_msg(c, 500, "会话创建失败");
        cJSON_Delete(j);
        return;
    }

    char userbuf[WEB_AUTH_USER_MAX] = "";
    web_auth_get_user(userbuf, sizeof(userbuf));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token", token);
    cJSON_AddNumberToObject(root, "expires_in", expires_in);
    cJSON_AddStringToObject(root, "user", userbuf);
    cJSON_AddBoolToObject(root, "pwd_default", web_auth_is_default());
    json_reply(c, 200, root);
    cJSON_Delete(root);
    cJSON_Delete(j);
    DG_LOGI(TAG, "登录成功(来源 %s,账号 %s)", ip, userbuf);
}

static void handle_logout(struct mg_connection *c, struct mg_http_message *hm)
{
    char buf[64];
    const char *tok = header_or_query_token(hm, buf, sizeof(buf));
    if (!web_session_validate(tok, time(NULL))) {
        reply_unauthorized(c);
        return;
    }
    web_session_revoke(tok);
    json_msg(c, 200, "已注销");
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

static void handle_device(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
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

    net_info_addr_t addr;
    net_info_read(&addr);
    char ifname[32] = "";
    snprintf(ifname, sizeof(ifname), "%s", addr.ifname);
    /* have_ip 仍单独给前端(它要把"未联网"与"地址 0.0.0.0"区分展示) */

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
    uint64_t db_bytes = 0, disk_free = 0;
    db_storage_stats(&db_bytes, &disk_free);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", DG_FW_VERSION);
    cJSON_AddNumberToObject(root, "uptime_s", (double)up);
    cJSON_AddStringToObject(root, "uptime_text", uptext);
    cJSON_AddNumberToObject(root, "users", (double)users);
    cJSON_AddNumberToObject(root, "log_total", (double)page.total);
    cJSON_AddStringToObject(root, "ip", addr.ip);
    cJSON_AddStringToObject(root, "ifname", ifname);
    cJSON_AddBoolToObject(root, "have_ip", addr.have_ip);
    cJSON_AddStringToObject(root, "mdns_host", host);
    cJSON_AddStringToObject(root, "mdns_url", url);
    cJSON_AddBoolToObject(root, "mdns_running", mdns_running());
    cJSON_AddBoolToObject(root, "online", net_info_is_online());
    cJSON_AddNumberToObject(root, "web_port", s_port);
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

    json_reply(c, 200, root);
    cJSON_Delete(root);
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

static void handle_logs(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }

    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1;
    q.page_size = 20;
    q.descending = true;

    char v[96];
    if (mg_http_get_var(&hm->query, "from", v, sizeof(v)) > 0 && v[0]) {
        if (parse_date(v, false, &q.ts_from) != DG_OK) {
            json_msg(c, 400, "日期格式应为 YYYY-MM-DD");
            return;
        }
    }
    if (mg_http_get_var(&hm->query, "to", v, sizeof(v)) > 0 && v[0]) {
        if (parse_date(v, true, &q.ts_to) != DG_OK) {
            json_msg(c, 400, "日期格式应为 YYYY-MM-DD");
            return;
        }
    }
    if (mg_http_get_var(&hm->query, "user_id", v, sizeof(v)) > 0 && v[0]) {
        size_t ulen = strlen(v);
        if (ulen >= sizeof(q.user_id)) {
            json_msg(c, 400, "用户 ID 过长");
            return;
        }
        memcpy(q.user_id, v, ulen + 1);   /* 长度已校验,避免 snprintf 截断告警 */
    }
    if (mg_http_get_var(&hm->query, "page", v, sizeof(v)) > 0 && v[0]) {
        int pg = atoi(v);
        if (pg < 1) {
            json_msg(c, 400, "坏参数:page");
            return;
        }
        q.page = (uint32_t)pg;
    }
    if (mg_http_get_var(&hm->query, "page_size", v, sizeof(v)) > 0 && v[0]) {
        int ps = atoi(v);
        if (ps < 1 || ps > LOG_PAGE_MAX) {
            json_msg(c, 400, "坏参数:page_size");
            return;
        }
        q.page_size = (uint32_t)ps;
    }

    access_log_t rows[LOG_PAGE_MAX];
    log_page_t out = { .logs = rows, .max = LOG_PAGE_MAX };
    if (db_log_query(&q, &out) != DG_OK) {
        json_msg(c, 400, "坏参数");
        return;
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
    json_reply(c, 200, root);
    cJSON_Delete(root);
}

/* ---- NTP(异步:SNTP 在 netcore loop 内执行,这里只受理 + 去重) ---- */

static pthread_mutex_t s_ntp_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_ntp_running = false;

static void handle_ntp(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }

    pthread_mutex_lock(&s_ntp_mtx);
    if (s_ntp_running) {
        pthread_mutex_unlock(&s_ntp_mtx);
        json_msg(c, 409, "校正进行中,请稍候");
        return;
    }
    s_ntp_running = true;
    pthread_mutex_unlock(&s_ntp_mtx);

    int rc = ntp_service_trigger();
    if (rc == DG_ERR_NOT_INIT) {
        pthread_mutex_lock(&s_ntp_mtx);
        s_ntp_running = false;
        pthread_mutex_unlock(&s_ntp_mtx);
        json_msg(c, 500, "校正服务不可用");
        return;
    }
    /* 202:已受理(未联网等失败也走 WS 推送,与旧版语义一致) */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "pending", true);
    cJSON_AddStringToObject(root, "note", "结果将经 WebSocket 推送");
    json_reply(c, 202, root);
    cJSON_Delete(root);
}

/* ---- 网络配置(读:实际地址快照;写:持久化 + 后台线程应用) ---- */

/* 把实际地址 + 期望配置拼成对外 JSON;ip/mask/gw 已由 net_info 兜底 0.0.0.0 */
static void network_json(cJSON *root)
{
    net_info_addr_t a;
    net_info_read(&a);
    const dg_cfg_t *cfg = cfg_get();
    const char *mode = (cfg && cfg->net_mode[0]) ? cfg->net_mode : "dhcp";

    cJSON_AddStringToObject(root, "ifname", a.ifname);
    cJSON_AddStringToObject(root, "ip", a.ip);
    cJSON_AddStringToObject(root, "netmask", a.mask);
    cJSON_AddStringToObject(root, "gateway", a.gw);
    cJSON_AddBoolToObject(root, "have_ip", a.have_ip);
    cJSON_AddBoolToObject(root, "online", net_info_is_online());
    cJSON_AddStringToObject(root, "mode", mode);
    if (cfg && strcmp(mode, "static") == 0) {
        cJSON *conf = cJSON_AddObjectToObject(root, "configured");
        cJSON_AddStringToObject(conf, "ip", cfg->net_ip);
        cJSON_AddStringToObject(conf, "netmask", cfg->net_mask);
        cJSON_AddStringToObject(conf, "gateway", cfg->net_gw);
    }
}

static void handle_network_get(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    cJSON *root = cJSON_CreateObject();
    network_json(root);
    json_reply(c, 200, root);
    cJSON_Delete(root);
}

/* 应用放在独立线程:net_cfg_apply 内部 system()(dhcpcd 交互)会阻塞秒级,
 * 决不能占住 loop;先睡 300ms 让 200 响应(乃至旧地址上最后的包)先出门。
 * notify_ui=true(设备端屏幕发起)时额外发 EV_NET_CFG_RESULT 供屏幕弹窗回执 */
typedef struct {
    net_cfg_req_t cfg;
    bool notify_ui;
} net_apply_job_t;

static void *apply_worker(void *arg)
{
    net_apply_job_t *job = arg;
    const net_cfg_req_t *r = &job->cfg;
    usleep(300 * 1000);

    net_info_addr_t got;
    int rc = net_cfg_apply(r, &got);
    if (rc != DG_OK) {
        DG_LOGW(TAG, "网络配置应用失败(%d),配置仍已持久化", rc);
    } else {
        /* 广播新地址:WS 推送、NTP 补同步、mDNS 重通告(mdns 自轮询)都挂
         * 在这条事件上;apply_worker 是独立线程,publish 线程安全 */
        ev_net_addr_t ev;
        memset(&ev, 0, sizeof(ev));
        snprintf(ev.ifname, sizeof(ev.ifname), "%s", got.ifname);
        snprintf(ev.ip, sizeof(ev.ip), "%s", got.ip);
        snprintf(ev.mask, sizeof(ev.mask), "%s", got.mask);
        snprintf(ev.gw, sizeof(ev.gw), "%s", got.gw);
        ev.have_ip = got.have_ip;
        EVENT_BUS_PUBLISH(EV_NET_ADDR, &ev);
    }
    if (job->notify_ui) {
        ev_net_cfg_result_t res;
        memset(&res, 0, sizeof(res));
        res.ok = (rc == DG_OK);
        res.err = rc;
        snprintf(res.ip, sizeof(res.ip), "%s", got.ip);
        EVENT_BUS_PUBLISH(EV_NET_CFG_RESULT, &res);
    }
    free(job);
    return NULL;
}

/* 公共入口:持久化 + 起应用线程。HTTP 与设备端事件两路共用。
 * 返回 DG_OK = 已受理(异步应用);DG_ERR_PARAM = 地址不合法 */
static int network_request(const net_cfg_req_t *r, bool notify_ui)
{
    if (net_cfg_validate(r) != DG_OK)
        return DG_ERR_PARAM;

    /* 先持久化再应用:应用失败(接口暂不可用)配置仍在,重启/插线后由
     * 开机装配或再次手动应用接管 */
    cfg_set_str("net_mode", r->is_static ? "static" : "dhcp");
    if (r->is_static) {
        cfg_set_str("net_ip", r->ip);
        cfg_set_str("net_mask", r->mask);
        cfg_set_str("net_gw", r->gw);
    }
    cfg_flush();

    net_apply_job_t *job = malloc(sizeof(*job));
    if (!job)
        return DG_ERR_IO;
    job->cfg = *r;
    job->notify_ui = notify_ui;
    pthread_t tid;
    if (pthread_create(&tid, NULL, apply_worker, job) == 0)
        pthread_detach(tid);
    else {
        free(job);                           /* 罕见:配置已存,提示手动重启生效 */
        return DG_ERR_IO;
    }
    return DG_OK;
}

/* 设备端屏幕设置(总线事件入口):UI 不碰 net_cfg,发事件由网络族落地 */
static int on_net_cfg_set(const event_t *e, void *ud)
{
    (void)ud;
    const ev_net_cfg_set_t *req = (const ev_net_cfg_set_t *)e->data;
    net_cfg_req_t r;
    memset(&r, 0, sizeof(r));
    r.is_static = req->is_static;
    snprintf(r.ip, sizeof(r.ip), "%s", req->ip);
    snprintf(r.mask, sizeof(r.mask), "%s", req->mask);
    snprintf(r.gw, sizeof(r.gw), "%s", req->gw);
    network_request(&r, true);
    return 0;
}

static void handle_network_set(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    char body[512] = { 0 };
    if (!body_copy(hm, body, sizeof(body))) {
        json_msg(c, 400, "坏请求");
        return;
    }
    cJSON *req = cJSON_Parse(body);
    if (!req) {
        json_msg(c, 400, "JSON 解析失败");
        return;
    }
    const cJSON *jmode = cJSON_GetObjectItemCaseSensitive(req, "mode");
    const char *mode = cJSON_IsString(jmode) ? jmode->valuestring : "";

    net_cfg_req_t r;
    memset(&r, 0, sizeof(r));
    if (strcmp(mode, "dhcp") == 0) {
        r.is_static = false;
    } else if (strcmp(mode, "static") == 0) {
        r.is_static = true;
        const cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(req, "ip")) && cJSON_IsString(v))
            snprintf(r.ip, sizeof(r.ip), "%s", v->valuestring);
        if ((v = cJSON_GetObjectItemCaseSensitive(req, "netmask")) && cJSON_IsString(v))
            snprintf(r.mask, sizeof(r.mask), "%s", v->valuestring);
        if ((v = cJSON_GetObjectItemCaseSensitive(req, "gateway")) && cJSON_IsString(v))
            snprintf(r.gw, sizeof(r.gw), "%s", v->valuestring);
    } else {
        cJSON_Delete(req);
        json_msg(c, 400, "mode 应为 dhcp 或 static");
        return;
    }
    if (net_cfg_validate(&r) != DG_OK) {
        cJSON_Delete(req);
        json_msg(c, 400, "地址不合法:IP/掩码须为合法点分格式且掩码连续,网关可为空");
        return;
    }

    if (network_request(&r, false) != DG_OK) {
        cJSON_Delete(req);
        json_msg(c, 500, "受理失败(地址不合法或内存不足)");
        return;
    }

    cJSON_Delete(req);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "pending", true);
    cJSON_AddStringToObject(root, "note",
                            "已受理:配置已保存并开始应用;若地址变化,请用新地址重新访问");
    json_reply(c, 202, root);
    cJSON_Delete(root);
}

/* ---- 账号/口令 ---- */

static void handle_account(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    char body[512] = { 0 };
    if (!body_copy(hm, body, sizeof(body))) {
        json_msg(c, 400, "坏请求");
        return;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        json_msg(c, 400, "坏 json");
        return;
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
        json_reply(c, 200, root);
        char ipbuf[48];
        DG_LOGI(TAG, "web 凭据已由 web 端修改(来源 %s)",
                remote_ip(c, ipbuf, sizeof(ipbuf)));
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
        json_reply(c, 400, root);
    }
    cJSON_Delete(root);
}

/* ---- OTA 上传(流式:HDRS 起、READ 喂、收完落位) ---- */

static struct mg_connection *s_ota_c = NULL;   /* 当前上传连接(loop 私有) */
static int64_t s_ota_remain = 0;               /* 还差多少字节 */

static void ota_fail(struct mg_connection *c, int code, const char *msg)
{
    ota_abort();
    s_ota_c = NULL;
    json_msg(c, code, msg);
}

static void ota_complete(void)
{
    struct mg_connection *c = s_ota_c;
    s_ota_c = NULL;
    if (!c)
        return;
    char path[64];
    int rc = ota_finish(path, sizeof(path));
    if (rc != DG_OK) {
        json_msg(c, 422, "校验失败(sha256/大小不符)");
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "staged", path);
    cJSON_AddBoolToObject(root, "resumed", false);
    json_reply(c, 200, root);
    cJSON_Delete(root);
}

/* 从连接 recv 缓冲喂 ota(限流:只取环形缓冲放得下的量),返回是否继续 */
static void ota_feed_from(struct mg_connection *c)
{
    while (s_ota_c == c && s_ota_remain > 0 && c->recv.len > 0) {
        size_t take = c->recv.len;
        if (take > (size_t)s_ota_remain)
            take = (size_t)s_ota_remain;
        size_t room = ota_can_accept();
        if (room == 0)
            return;                          /* 环满:等写线程排干,RETRY 兜底 */
        if (take > room)
            take = room;

        size_t got = 0;
        if (ota_write_chunk((const uint8_t *)c->recv.buf, take, &got) != DG_OK) {
            ota_fail(c, 400, "写入失败(超大小?)");
            return;
        }
        mg_iobuf_del(&c->recv, 0, take);
        s_ota_remain -= (int64_t)take;
    }
    if (s_ota_c == c && s_ota_remain == 0)
        ota_complete();
}

/* 环满且数据已到齐的兜底:netcore_post 稍后重喂(写线程排水是毫秒级) */
static void ota_retry_cb(void *arg)
{
    (void)arg;
    if (s_ota_c && s_ota_remain > 0)
        ota_feed_from(s_ota_c);
}

/* MG_EV_HTTP_HDRS:/api/ota/upload 的头部先至——校验并起会话,正文走 READ。
 * 摘除头部块会让 mongoose 卸载本连接的 HTTP 解析器(官方大包上传约定):
 * 此后正文按原始字节经 MG_EV_READ 到达,不再有 MG_EV_HTTP_MSG,整包不进内存 */
static void ota_start(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!method_is(hm, "POST")) {
        json_msg(c, 405, "OTA 上传只接受 POST");
        return;
    }
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    const char *names[] = { "X-OTA-Version", "X-OTA-Size", "X-OTA-SHA256" };
    char ver[32] = "", size_s[24] = "", sha[65] = "";
    struct mg_str *h;
    if ((h = mg_http_get_header(hm, names[0])) != NULL && h->len < sizeof(ver))
        memcpy(ver, h->buf, h->len);
    if ((h = mg_http_get_header(hm, names[1])) != NULL && h->len < sizeof(size_s))
        memcpy(size_s, h->buf, h->len);
    if ((h = mg_http_get_header(hm, names[2])) != NULL && h->len < sizeof(sha))
        memcpy(sha, h->buf, h->len);
    if (!ver[0] || !size_s[0] || !sha[0]) {
        json_msg(c, 400, "缺少 manifest 头(X-OTA-Version/-Size/-SHA256)");
        return;
    }

    ota_manifest_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.version, sizeof(m.version), "%s", ver);
    m.size = (uint32_t)strtoul(size_s, NULL, 10);
    snprintf(m.sha256, sizeof(m.sha256), "%s", sha);

    char off_s[24] = "";
    uint32_t offset = 0;
    if ((h = mg_http_get_header(hm, "X-OTA-Offset")) != NULL &&
        h->len < sizeof(off_s)) {
        memcpy(off_s, h->buf, h->len);
        offset = (uint32_t)strtoul(off_s, NULL, 10);
    }

    bool resumed = false;
    int rc = ota_begin(&m, offset, &resumed);
    if (rc == DG_ERR_PARAM) {
        json_msg(c, 400, "包大小超限或参数非法");
        return;
    }
    if (rc == DG_ERR_STATE) {
        json_msg(c, 409, "续传偏移不符,请重传");
        return;
    }
    if (rc == DG_ERR_BUSY) {
        json_msg(c, 409, "其他上传进行中");
        return;
    }
    if (rc != DG_OK) {
        json_msg(c, 500, "开始失败");
        return;
    }

    s_ota_c = c;
    s_ota_remain = (int64_t)m.size - (int64_t)offset;
    /* 头部同拍已带正文:先摘头(触发 mongoose 卸载解析器),再喂正文。
     * 摘头后正文从 recv[0] 开始,ota_feed_from 的偏移才正确 */
    size_t hdr_len = (size_t)(hm->body.buf - (const char *)c->recv.buf);
    mg_iobuf_del(&c->recv, 0, hdr_len);
    ota_feed_from(c);
}

/* ---- 静态资源(内嵌前端产物) ---- */

/* 按精确路径查资源表(pages/ 由 gen_pages.sh 生成,见 web_pages.h) */
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

/* ---- 路由分派 ---- */

static void handle_ws(struct mg_connection *c, struct mg_http_message *hm)
{
    /* token 校验失败必须显式回 401 再拒绝:否则客户端只看到连接被关,
     * 无法区分"密码错"和"网络断"(历史实测坑, mongoose 下天然没有
     * civetweb "状态行被吞"的问题——升级由这里显式触发) */
    char token[64] = "";
    bool ok = mg_http_get_var(&hm->query, "token", token, sizeof(token)) > 0 &&
              web_session_validate(token, time(NULL));
    if (!ok) {
        char ipbuf[48];
        DG_LOGW(TAG, "WebSocket 未授权连接被拒(来源 %s)",
                remote_ip(c, ipbuf, sizeof(ipbuf)));
        json_msg(c, 401, "未登录或会话已过期");
        return;
    }
    ws_add(c);
    mg_ws_upgrade(c, hm, NULL);
}

/* ===== 新增 handler(插在 route_t 定义之前)===== */

/* ---- 用户管理(业务语义与设备端用户编辑页一致:同一 storage 权威) ---- */

/* 错误码 → 文案(与设备端 page_user_edit 的 err_text 同语义映射) */
static const char *user_err_text(int rc)
{
    switch (rc) {
    case DG_ERR_NO_PASSWORD: return "请先设置密码";
    case DG_ERR_DUP_UID:     return "该用户ID已存在";
    case DG_ERR_DUP_IC:      return "该卡已绑定其他用户";
    case DG_ERR_DUP_FACE:    return "该人脸已绑定其他用户";
    case DG_ERR_DUP_FINGER:  return "该指纹已绑定其他用户";
    case DG_ERR_USER_LIMIT:  return "用户数已达上限";
    case DG_ERR_NOT_FOUND:   return "用户不存在";
    case DG_ERR_BAD_NAME:    return dg_valid_hint(DG_ERR_BAD_NAME);
    case DG_ERR_BAD_PWD:     return dg_valid_hint(DG_ERR_BAD_PWD);
    case DG_ERR_BAD_UID:     return dg_valid_hint(DG_ERR_BAD_UID);
    default:                 return "操作失败";
    }
}

/* 请求体解析:整个 body 一次读入 + cJSON 解析 */
static cJSON *body_json(struct mg_http_message *hm)
{
    char buf[1024];
    if (!body_copy(hm, buf, sizeof(buf)))
        return NULL;
    cJSON *body = cJSON_Parse(buf);
    return body;                        /* 失败返回 NULL,调用方回 400 */
}

static bool jstr(const cJSON *body, const char *key, char *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!cJSON_IsString(it) || it->valuestring == NULL)
        return false;
    if (strlen(it->valuestring) >= cap)
        return false;
    memcpy(out, it->valuestring, strlen(it->valuestring) + 1);
    return true;
}

static bool jrole(const cJSON *body, int32_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(body, "role");
    if (cJSON_IsNumber(it)) {
        if (it->valuedouble < 0 || it->valuedouble > 2)
            return false;
        *out = (int32_t)it->valuedouble;
    }
    return true;                        /* 缺省 = 不改(添加时默认普通) */
}

static bool jflags(const cJSON *body, uint32_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(body, "auth_flags");
    if (cJSON_IsNumber(it)) {
        double d = it->valuedouble;
        if (d < 1 || d > (double)DG_AUTH_ALL || d != (double)(uint32_t)d)
            return false;
        *out = (uint32_t)d;
        if (*out & ~(uint32_t)DG_AUTH_ALL)
            return false;
    }
    return true;
}

static void reply_user_err(struct mg_connection *c, int rc)
{
    json_msg(c, rc == DG_ERR_NOT_FOUND ? 404 : 400, user_err_text(rc));
}

static void handle_users_list(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    uint32_t page = 1, page_size = 20;
    char v[16];
    if (mg_http_get_var(&hm->query, "page", v, sizeof(v)) > 0 && v[0]) {
        int pg = atoi(v);
        if (pg < 1) {
            json_msg(c, 400, "坏参数:page");
            return;
        }
        page = (uint32_t)pg;
    }
    if (mg_http_get_var(&hm->query, "page_size", v, sizeof(v)) > 0 && v[0]) {
        int ps = atoi(v);
        if (ps < 1 || ps > 200) {
            json_msg(c, 400, "坏参数:page_size(1~200)");
            return;
        }
        page_size = (uint32_t)ps;
    }

    /* 字典序全量 ID 再内存分页(loop 线程独占,静态避免 64KB 栈) */
    static char ids[DG_USER_MAX][DG_UID_LEN];
    uint32_t n = 0;
    if (db_user_list_ids(ids, DG_USER_MAX, &n) != DG_OK) {
        json_msg(c, 500, "枚举用户失败");
        return;
    }
    uint32_t pages = (n + page_size - 1) / page_size;
    if (pages == 0)
        pages = 1;
    uint32_t start = (page - 1) * page_size;
    uint32_t end = start + page_size;
    if (end > n)
        end = n;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "page", (double)page);
    cJSON_AddNumberToObject(root, "pages", (double)pages);
    cJSON_AddNumberToObject(root, "total", (double)n);
    cJSON *arr = cJSON_AddArrayToObject(root, "users");
    for (uint32_t i = start; i < end; i++) {
        user_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        if (db_user_get(ids[i], &rec) != DG_OK)
            continue;                   /* 列表与库竞态(他处刚删):跳过自愈 */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "uid", rec.user_id);
        cJSON_AddStringToObject(o, "name", rec.user_name);
        cJSON_AddNumberToObject(o, "role", (double)rec.role);
        cJSON_AddNumberToObject(o, "auth_flags", (double)rec.auth_flags);
        cJSON_AddBoolToObject(o, "has_face", rec.face_vec_len > 0);
        cJSON_AddBoolToObject(o, "has_finger", rec.finger_vec_len > 0);
        cJSON_AddNumberToObject(o, "created_at", (double)rec.created_at);
        cJSON_AddNumberToObject(o, "updated_at", (double)rec.updated_at);
        cJSON_AddItemToArray(arr, o);
    }
    json_reply(c, 200, root);
    cJSON_Delete(root);
}

static void handle_users_add(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    cJSON *body = body_json(hm);
    if (!body) {
        json_msg(c, 400, "坏请求体");
        return;
    }

    char uid[DG_UID_LEN], name[DG_NAME_LEN], pwd[DG_PWD_MAX_LEN];
    int32_t role = DG_ROLE_NORMAL;
    uint32_t flags = DG_AUTH_FACE | DG_AUTH_PWD;   /* 与设备端新建默认一致 */

    bool ok = jstr(body, "uid", uid, sizeof(uid)) &&
              jstr(body, "name", name, sizeof(name)) &&
              jstr(body, "pwd", pwd, sizeof(pwd)) &&
              jrole(body, &role) && jflags(body, &flags);
    cJSON_Delete(body);
    if (!ok) {
        json_msg(c, 400, "缺少必填字段或取值非法(uid/name/pwd 必填)");
        return;
    }

    /* 与 UI 弹窗同源的即时校验(dg_valid_* 是唯一权威,这里先给友好错误,
     * db_user_add 内还有同规则兜底) */
    int vrc = dg_valid_uid(uid);
    if (vrc != DG_OK) {
        json_msg(c, 400, dg_valid_hint(vrc));
        return;
    }
    vrc = dg_valid_name(name);
    if (vrc != DG_OK) {
        json_msg(c, 400, dg_valid_hint(vrc));
        return;
    }
    vrc = dg_valid_pwd(pwd);
    if (vrc != DG_OK) {
        json_msg(c, 400, dg_valid_hint(vrc));
        return;
    }

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", uid);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", name);
    rec.role = role;
    rec.auth_flags = flags;
    int rc = db_user_set_password(&rec, pwd);
    if (rc == DG_OK)
        rc = db_user_add(&rec);
    memset(pwd, 0, sizeof(pwd));
    if (rc != DG_OK) {
        reply_user_err(c, rc);
        return;
    }
    DG_LOGI(TAG, "web 添加用户 %s(%s)", uid, name);
    json_msg(c, 200, "已添加");
}

static void handle_users_update(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    cJSON *body = body_json(hm);
    if (!body) {
        json_msg(c, 400, "坏请求体");
        return;
    }
    char uid[DG_UID_LEN], name[DG_NAME_LEN];
    int32_t role = -1;
    uint32_t flags = 0;
    bool has_name = jstr(body, "name", name, sizeof(name));
    bool has_role = false, has_flags = false;
    const cJSON *rit = cJSON_GetObjectItemCaseSensitive(body, "role");
    if (cJSON_IsNumber(rit)) {
        has_role = true;
        role = (int32_t)rit->valuedouble;
    }
    const cJSON *fit = cJSON_GetObjectItemCaseSensitive(body, "auth_flags");
    if (cJSON_IsNumber(fit)) {
        has_flags = true;
        flags = (uint32_t)fit->valuedouble;
    }
    bool has_uid = jstr(body, "uid", uid, sizeof(uid));
    cJSON_Delete(body);

    if (!has_uid || (!has_name && !has_role && !has_flags)) {
        json_msg(c, 400, "需要 uid 与至少一个待改字段");
        return;
    }
    if (has_role && (role < 0 || role > 2)) {
        json_msg(c, 400, "role 取值 0 普通/1 管理员/2 黑名单");
        return;
    }
    if (has_flags &&
        (flags == 0 || flags > (uint32_t)DG_AUTH_ALL)) {
        json_msg(c, 400, "auth_flags 必须是 1~15 的验证方式组合");
        return;
    }

    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    int rc = db_user_get(uid, &rec);
    if (rc != DG_OK) {
        reply_user_err(c, rc);
        return;
    }
    if (has_name) {
        rc = dg_valid_name(name);
        if (rc != DG_OK) {
            json_msg(c, 400, dg_valid_hint(rc));
            return;
        }
        snprintf(rec.user_name, sizeof(rec.user_name), "%s", name);
    }
    if (has_role)
        rec.role = role;
    if (has_flags)
        rec.auth_flags = flags;
    rc = db_user_update(&rec);
    if (rc != DG_OK) {
        reply_user_err(c, rc);
        return;
    }
    DG_LOGI(TAG, "web 编辑用户 %s", uid);
    json_msg(c, 200, "已保存");
}

static void handle_users_pwd(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    cJSON *body = body_json(hm);
    if (!body) {
        json_msg(c, 400, "坏请求体");
        return;
    }
    char uid[DG_UID_LEN], pwd[DG_PWD_MAX_LEN];
    bool ok = jstr(body, "uid", uid, sizeof(uid)) &&
              jstr(body, "pwd", pwd, sizeof(pwd));
    cJSON_Delete(body);
    if (!ok) {
        json_msg(c, 400, "需要 uid 与 pwd");
        return;
    }
    int rc = dg_valid_pwd(pwd);
    if (rc != DG_OK) {
        json_msg(c, 400, dg_valid_hint(rc));
        return;
    }
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rc = db_user_get(uid, &rec);
    if (rc == DG_OK) {
        rc = db_user_set_password(&rec, pwd);
        if (rc == DG_OK)
            rc = db_user_update(&rec);   /* set_password 只算哈希,落库要 update */
    }
    memset(pwd, 0, sizeof(pwd));
    if (rc != DG_OK) {
        reply_user_err(c, rc);
        return;
    }
    DG_LOGI(TAG, "web 重置用户密码 %s", uid);
    json_msg(c, 200, "密码已更新");
}

/* 删除/清人脸经 enroll 服务事件(与设备端同一契约:DB+特征库+头像一起动) */
static void handle_users_delete(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    cJSON *body = body_json(hm);
    if (!body) {
        json_msg(c, 400, "坏请求体");
        return;
    }
    char uid[DG_UID_LEN];
    bool ok = jstr(body, "uid", uid, sizeof(uid));
    cJSON_Delete(body);
    if (!ok) {
        json_msg(c, 400, "需要 uid");
        return;
    }
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    int rc = db_user_get(uid, &rec);
    if (rc != DG_OK) {
        reply_user_err(c, rc);
        return;
    }
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = DG_ENROLL_DELETE;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
    DG_LOGI(TAG, "web 删除用户 %s(经 enroll 服务)", uid);
    json_msg(c, 202, "删除请求已受理");
}

static void handle_users_face_clear(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    cJSON *body = body_json(hm);
    if (!body) {
        json_msg(c, 400, "坏请求体");
        return;
    }
    char uid[DG_UID_LEN];
    bool ok = jstr(body, "uid", uid, sizeof(uid));
    cJSON_Delete(body);
    if (!ok) {
        json_msg(c, 400, "需要 uid");
        return;
    }
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    int rc = db_user_get(uid, &rec);
    if (rc != DG_OK) {
        reply_user_err(c, rc);
        return;
    }
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", uid);
    ev.kind = DG_ENROLL_FACE_CLEAR;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
    DG_LOGI(TAG, "web 清除人脸 %s(经 enroll 服务)", uid);
    json_msg(c, 202, "清除请求已受理");
}

/* ---- 门禁/系统设置(meta 表权威范围;写走 cfg_set_* 同一入口) ---- */

typedef struct {
    const char *key;
    const char *label;
    const char *unit;
    bool is_dbl;
    double lo, hi, step;
    int def_i;                          /* int 项默认展示(仅提示用) */
} web_set_item_t;

static const web_set_item_t s_set_items[] = {
    /* 键           标签              单位   dbl   lo     hi     step */
    { "door_open_ms",      "开门时长",        "ms", false, 1000, 10000, 500,  3000 },
    { "pwd_fail_lock_n",   "密码连错锁定次数", "次", false,    1,    10,   1,     5 },
    { "pwd_fail_lock_s",   "锁定时长",        "s",  false,   10,  3600,  10,    60 },
    { "standby_timeout_s", "待机超时",        "s",  false,   15,    60,   5,    30 },
    { "menu_timeout_s",    "菜单超时",        "s",  false,    5,   120,   5,    15 },
    { "lost_hold_ms",      "脸框消失滞回",    "ms", false,    0,  2000,  50,   200 },
    { "min_face_px",       "识别最小人脸",    "px", false,   40,   400,  10,    80 },
    { "face_dup_threshold", "录入人脸查重阈值", "",  true,  0.50,  1.00, 0.05, 0.75 },
    { "liveness_enable",   "活体检测开关",    "",   false,    0,     1,   1,     0 },
    { "face_match_threshold", "1:N 识别阈值",    "",   true,  0.30,  1.00, 0.01,  0 },
    { "det_threshold",     "检测出框阈值",    "",   true,  0.30,  0.95, 0.01,  0 },
    { "det_score_min",     "检测分下限",      "",   true,  0.30,  1.00, 0.01,  0 },
    { "blur_min",          "清晰度下限",      "",   true,  0.0, 50000,  5,     0 },
};

static bool cfg_value_of(const dg_cfg_t *c, const char *key, double *out)
{
    if (!strcmp(key, "door_open_ms"))            *out = c->door_open_ms;
    else if (!strcmp(key, "pwd_fail_lock_n"))    *out = c->pwd_fail_lock_n;
    else if (!strcmp(key, "pwd_fail_lock_s"))    *out = c->pwd_fail_lock_s;
    else if (!strcmp(key, "standby_timeout_s"))  *out = c->standby_timeout_s;
    else if (!strcmp(key, "menu_timeout_s"))     *out = c->menu_timeout_s;
    else if (!strcmp(key, "lost_hold_ms"))       *out = c->face_lost_hold_ms;
    else if (!strcmp(key, "min_face_px"))        *out = c->face_min_px;
    else if (!strcmp(key, "face_dup_threshold")) *out = c->face_dup_threshold;
    else if (!strcmp(key, "liveness_enable"))    *out = c->liveness_enable;
    else if (!strcmp(key, "face_match_threshold"))    *out = c->face_match_threshold;
    else if (!strcmp(key, "det_threshold"))      *out = c->face_det_threshold;
    else if (!strcmp(key, "det_score_min"))      *out = c->face_det_score_min;
    else if (!strcmp(key, "blur_min"))           *out = c->face_blur_min;
    else return false;
    return true;
}

static const web_set_item_t *set_item_find(const char *key)
{
    for (size_t i = 0; i < sizeof(s_set_items) / sizeof(s_set_items[0]); i++)
        if (!strcmp(s_set_items[i].key, key))
            return &s_set_items[i];
    return NULL;
}

static void handle_access_set_get(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    const dg_cfg_t *cfg = cfg_get();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < sizeof(s_set_items) / sizeof(s_set_items[0]); i++) {
        const web_set_item_t *it = &s_set_items[i];
        double val;
        if (!cfg_value_of(cfg, it->key, &val))
            continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", it->key);
        cJSON_AddStringToObject(o, "label", it->label);
        cJSON_AddStringToObject(o, "unit", it->unit);
        cJSON_AddBoolToObject(o, "is_dbl", it->is_dbl);
        cJSON_AddNumberToObject(o, "value", val);
        cJSON_AddNumberToObject(o, "min", it->lo);
        cJSON_AddNumberToObject(o, "max", it->hi);
        cJSON_AddNumberToObject(o, "step", it->step);
        cJSON_AddItemToArray(arr, o);
    }
    json_reply(c, 200, root);
    cJSON_Delete(root);
}

static void handle_access_set_post(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!check_token(hm)) {
        reply_unauthorized(c);
        return;
    }
    char buf[1024];
    if (!body_copy(hm, buf, sizeof(buf))) {
        json_msg(c, 400, "坏请求体");
        return;
    }
    cJSON *body = cJSON_Parse(buf);
    if (!body || !cJSON_IsObject(body)) {
        cJSON_Delete(body);
        json_msg(c, 400, "坏请求体(需要对象)");
        return;
    }

    /* 先整批校验(范围/类型),再统一应用:不给"改一半"的状态 */
    const cJSON *it;
    cJSON_ArrayForEach(it, body) {
        const web_set_item_t *m = set_item_find(it->string);
        if (!m) {
            json_msg(c, 400, "未知设置项");
            cJSON_Delete(body);
            return;
        }
        if (!cJSON_IsNumber(it)) {
            json_msg(c, 400, "设置项必须是数值");
            cJSON_Delete(body);
            return;
        }
        if (it->valuedouble < m->lo || it->valuedouble > m->hi) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s 需在 %g~%g %s内",
                     m->label, m->lo, m->hi, m->unit);
            json_msg(c, 400, msg);
            cJSON_Delete(body);
            return;
        }
    }
    cJSON_ArrayForEach(it, body) {
        const web_set_item_t *m = set_item_find(it->string);
        int rc;
        if (m->is_dbl)
            rc = cfg_set_dbl(m->key, it->valuedouble);
        else
            rc = cfg_set_int(m->key, (int)(it->valuedouble < 0 ? it->valuedouble - 0.5
                                                               : it->valuedouble + 0.5));
        if (rc != DG_OK) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s 应用失败(%d)", m->label, rc);
            json_msg(c, 500, msg);
            cJSON_Delete(body);
            return;
        }
    }
    cJSON_Delete(body);
    DG_LOGI(TAG, "web 更新门禁设置");
    json_msg(c, 200, "已保存");
}

typedef struct {
    const char *method;
    const char *uri;
    const char *method_msg;              /* 方法不符时回的 405 文案 */
    void (*handler)(struct mg_connection *, struct mg_http_message *);
} route_t;

static const route_t s_routes[] = {
    { "POST", "/api/login",   "登录接口只接受 POST",   handle_login },
    { "POST", "/api/logout",  "注销接口只接受 POST",   handle_logout },
    { "GET",  "/api/device",  "设备信息只接受 GET",    handle_device },
    { "GET",  "/api/logs",    "日志查询只接受 GET",    handle_logs },
    { "GET",  "/api/network", "网络配置只接受 GET",    handle_network_get },
    { "POST", "/api/network", "网络配置只接受 POST",   handle_network_set },
    { "POST", "/api/ntp",     "时间校正只接受 POST",   handle_ntp },
    { "POST", "/api/account", "账号修改只接受 POST",   handle_account },
    { "GET",  "/api/users",            "用户列表只接受 GET",    handle_users_list },
    { "POST", "/api/users/add",        "添加用户只接受 POST",   handle_users_add },
    { "POST", "/api/users/update",     "编辑用户只接受 POST",   handle_users_update },
    { "POST", "/api/users/pwd",        "改密码只接受 POST",     handle_users_pwd },
    { "POST", "/api/users/delete",     "删除用户只接受 POST",   handle_users_delete },
    { "POST", "/api/users/face_clear", "清除人脸只接受 POST",   handle_users_face_clear },
    { "GET",  "/api/access_set",       "门禁设置只接受 GET",    handle_access_set_get },
    { "POST", "/api/access_set",       "门禁设置只接受 POST",   handle_access_set_post },
};

/* 分派次序:精确路由 → /api/ws 升级 → 未知 /api/ 回 JSON 404 →
 * 资源表命中返回 → 单页应用回退(hash 路由下前端自己处理路径) */
static void route(struct mg_connection *c, struct mg_http_message *hm)
{
    char uri[128];
    if (hm->uri.len >= sizeof(uri)) {
        json_msg(c, 404, "接口不存在");
        return;
    }
    memcpy(uri, hm->uri.buf, hm->uri.len);
    uri[hm->uri.len] = '\0';

    /* 同一 URI 可注册多个方法(/api/access_set 有 GET+POST):URI 命中但
     * 方法不符时必须继续扫完——否则排在后面的正确方法条目永远轮不到,
     * 请求被先行 405(2026-09-27 宿主功能验收抓出)。全部扫完仍无方法
     * 匹配,才按首个命中条目的文案回 405 */
    const route_t *hit = NULL;
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        if (strcmp(uri, s_routes[i].uri) != 0)
            continue;
        if (hit == NULL)
            hit = &s_routes[i];
        if (method_is(hm, s_routes[i].method)) {
            s_routes[i].handler(c, hm);
            return;
        }
    }

    if (hit != NULL) {
        json_msg(c, 405, hit->method_msg);
        return;
    }
    if (strcmp(uri, "/api/ws") == 0) {
        handle_ws(c, hm);
        return;
    }

    const dg_web_asset_t *asset = asset_lookup(uri);
    if (asset) {
        http_send(c, 200, asset->mime, asset->data, asset->len, NULL);
        return;
    }
    if (strncmp(uri, "/api/", 5) == 0) {
        json_msg(c, 404, "接口不存在");
        return;
    }
    /* 非资源、非接口:交给单页应用(前端 hash 路由自行决定显示什么) */
    http_send(c, 200, "text/html; charset=utf-8", DG_WEB_INDEX_HTML,
              strlen(DG_WEB_INDEX_HTML), NULL);
}

/* ---- 连接事件入口(loop 线程) ---- */

static void http_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG) {
        route(c, ev_data);
    } else if (ev == MG_EV_HTTP_HDRS) {
        struct mg_http_message *hm = ev_data;
        if (uri_is(hm, "/api/ota/upload") && s_ota_c != c)
            ota_start(c, hm);
    } else if (ev == MG_EV_READ) {
        if (c == s_ota_c) {
            ota_feed_from(c);
            /* 环满且客户端数据已到齐(不再有 READ):定时重喂兜底 */
            if (s_ota_c == c && s_ota_remain > 0 && c->recv.len == 0)
                netcore_post(ota_retry_cb, NULL);
        }
    } else if (ev == MG_EV_WS_MSG) {
        /* 推送不依赖客户端消息;只需保持连接 */
    } else if (ev == MG_EV_CLOSE) {
        if (c == s_ota_c) {                  /* 客户端中途断开:保留 .part 供续传 */
            ota_abort();
            s_ota_c = NULL;
        }
        ws_remove(c);
    }
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
        ws_push_async();
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
    pthread_mutex_lock(&s_ntp_mtx);
    s_ntp_running = false;                   /* 一次校正落幕,允许下一轮 */
    pthread_mutex_unlock(&s_ntp_mtx);

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
        ws_push_async();
        free(s);
    }
    cJSON_Delete(root);
    return 0;
}

/* 活跃 WS 连接数:0 = 无客户端,周期推送直接跳过(不占推送队列挤实时事件) */
static int ws_active_count(void)
{
    int n = 0;
    for (int i = 0; i < WS_MAX_CONN; i++)
        if (s_ws[i])
            n++;
    return n;
}

/* loop 定时(5s):运行时长推送——上位机概览不再等 30s 轮询。
 * /proc/uptime 纯内存文件读取,与 net_watch 同级开销 */
static void uptime_push_cb(void *arg)
{
    (void)arg;
    if (!s_started || ws_active_count() == 0)
        return;
    int64_t up = read_uptime_s();
    char uptext[48];
    fmt_uptime(up, uptext, sizeof(uptext));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "uptime");
    cJSON_AddNumberToObject(root, "uptime_s", (double)up);
    cJSON_AddStringToObject(root, "uptime_text", uptext);
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        ws_enqueue(s);
        ws_push_async();
        free(s);
    }
    cJSON_Delete(root);
}

/* ---- 地址变化监视(loop 5s 定时)与 EV_NET_ADDR → WebSocket ---- */

/* 上次推送的地址快照(loop 线程私有);变化才推送,续租同址不刷屏 */
static net_info_addr_t s_last_addr;
static bool s_last_addr_valid = false;

static int on_net_addr(const event_t *e, void *ud)
{
    (void)ud;
    const ev_net_addr_t *a = (const ev_net_addr_t *)e->data;
    s_last_addr_valid = true;
    snprintf(s_last_addr.ifname, sizeof(s_last_addr.ifname), "%s", a->ifname);
    snprintf(s_last_addr.ip, sizeof(s_last_addr.ip), "%s", a->ip);
    snprintf(s_last_addr.mask, sizeof(s_last_addr.mask), "%s", a->mask);
    snprintf(s_last_addr.gw, sizeof(s_last_addr.gw), "%s", a->gw);
    s_last_addr.have_ip = a->have_ip;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "net");
    cJSON_AddStringToObject(root, "ifname", a->ifname);
    cJSON_AddStringToObject(root, "ip", a->ip);
    cJSON_AddStringToObject(root, "netmask", a->mask);
    cJSON_AddStringToObject(root, "gateway", a->gw);
    cJSON_AddBoolToObject(root, "have_ip", a->have_ip);
    cJSON_AddStringToObject(root, "msg", a->have_ip ? "设备地址已更新"
                                                    : "设备已无网络地址");
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        ws_enqueue(s);
        ws_push_async();
        free(s);
    }
    cJSON_Delete(root);
    return 0;
}

/* loop 线程定时回调:地址与上次不同(含"拿到/丢失地址")才发布事件。
 * net_info_read 纯 getifaddrs+/proc 读取,无阻塞,可安全待在 loop */
static void net_watch_cb(void *arg)
{
    (void)arg;
    net_info_addr_t now;
    if (net_info_read(&now) != DG_OK)
        return;
    if (s_last_addr_valid &&
        strcmp(now.ifname, s_last_addr.ifname) == 0 &&
        strcmp(now.ip, s_last_addr.ip) == 0 &&
        strcmp(now.mask, s_last_addr.mask) == 0 &&
        strcmp(now.gw, s_last_addr.gw) == 0 &&
        now.have_ip == s_last_addr.have_ip)
        return;

    ev_net_addr_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.ifname, sizeof(ev.ifname), "%s", now.ifname);
    snprintf(ev.ip, sizeof(ev.ip), "%s", now.ip);
    snprintf(ev.mask, sizeof(ev.mask), "%s", now.mask);
    snprintf(ev.gw, sizeof(ev.gw), "%s", now.gw);
    ev.have_ip = now.have_ip;
    EVENT_BUS_PUBLISH(EV_NET_ADDR, &ev);
    /* s_last_addr 由 on_net_addr 统一更新(订阅在同一进程,必达) */
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
    st.running = s_started;
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

/* loop 线程(netcore_post 闭包):建监听。注册动作与 poll 同线程,无并发。
 * 默认 80(HTTP 标准端口,URL 免带端口);绑定失败(非 root 环境如 PC 模拟器)
 * 自动回退 8080 并同步 mDNS 通告端口 */
static void web_setup(void *arg)
{
    (void)arg;
    char url[32];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", s_port);
    s_lsn = mg_http_listen(netcore_mgr(), url, http_handler, NULL);
    if (!s_lsn && s_port == 80) {
        DG_LOGW(TAG, "80 端口绑定失败(需要 root),回退 8080");
        s_port = 8080;
        mdns_set_port(s_port);
        snprintf(url, sizeof(url), "http://0.0.0.0:%d", s_port);
        s_lsn = mg_http_listen(netcore_mgr(), url, http_handler, NULL);
    }
    if (!s_lsn)
        DG_LOGE(TAG, "web 启动失败(端口 %d 被占?)", s_port);
    else
        DG_LOGI(TAG, "web 上位机就绪 :%d(版本 %s)", s_port, DG_FW_VERSION);
    mg_timer_add(netcore_mgr(), 5000, MG_TIMER_REPEAT, net_watch_cb, NULL);
    mg_timer_add(netcore_mgr(), 5000, MG_TIMER_REPEAT, uptime_push_cb, NULL);
    net_watch_cb(NULL);                  /* 起服即记录基线地址(变化才推) */
}

static void web_teardown(void *arg)
{
    (void)arg;
    if (s_ota_c) {
        ota_abort();
        s_ota_c = NULL;
    }
    /* is_closing:延迟到本轮 poll 末尾关闭;闭包跑在 poll 的定时器上下文里,
     * 立即 free(mg_close_conn)会让 poll 循环踩已释放内存(板上实测段错误) */
    for (int i = 0; i < WS_MAX_CONN; i++) {
        if (s_ws[i]) {
            s_ws[i]->is_closing = 1;
            s_ws[i] = NULL;
        }
    }
    if (s_lsn) {
        s_lsn->is_closing = 1;
        s_lsn = NULL;
    }
}

int web_server_start(void)
{
    if (s_started)
        return DG_OK;

    s_started_at = time(NULL);
    memset(s_ws, 0, sizeof(s_ws));

    if (web_auth_ensure() != DG_OK) {
        DG_LOGE(TAG, "凭据初始化失败(DB 不可用?),web 上位机不启动");
        return DG_ERR_IO;
    }
    if (!netcore_running()) {
        DG_LOGE(TAG, "netcore 未运行,web 上位机不启动");
        return DG_ERR_IO;
    }

    const dg_cfg_t *cfg = cfg_get();
    s_port = (cfg && cfg->web_port > 0) ? cfg->web_port : 8080;

    s_started = true;
    pthread_mutex_lock(&s_ws_mtx);
    s_q_head = s_q_tail = 0;
    s_q_dropped = 0;
    pthread_mutex_unlock(&s_ws_mtx);

    s_sub_cnt = 0;
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_AUTH_RESULT, on_auth_result, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_NTP_RESULT, on_ntp_result, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_WEB_STATE_REQ, on_web_state_req, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_WEB_SET, on_web_set, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_ADDR, on_net_addr, NULL);
    s_subs[s_sub_cnt++] = event_bus_subscribe(EV_NET_CFG_SET, on_net_cfg_set, NULL);

    netcore_post(web_setup, NULL);           /* 监听注册在 loop 线程执行 */
    publish_web_state();
    return DG_OK;
}

int64_t web_server_heartbeat_ms(void)
{
    return netcore_heartbeat_ms();           /* 心跳 = 事件循环线程活性 */
}

void web_server_stop(void)
{
    if (!s_started)
        return;
    s_started = false;

    for (int i = 0; i < s_sub_cnt; i++)
        event_bus_unsubscribe(s_subs[i]);
    s_sub_cnt = 0;

    netcore_post(web_teardown, NULL);        /* netcore 已停则静默丢弃(mgr_free 兜底) */
    web_session_revoke_all();
}
