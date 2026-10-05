/*
 * ota_http.c — OTA 升级包内置 HTTP 直链传输
 *
 * 取舍(2026-10-05):固件包是 MB 级载荷,走 HTTP 直链而不是过 broker;
 * 实现按"够用且显式失败"原则——
 *   - 仅 http://(https 返回 UNSUPPORTED,提示经反向代理暴露直链,或后续
 *     在传输注入点上实现 TLS;公网部署侧自决);
 *   - 必须带 Content-Length(Transfer-Encoding: chunked 显式拒绝,提示
 *     用静态直链;流式解码不值得为固件场景引入);
 *   - 跟随 301/302/303/307/308 重定向至多 3 次(对象存储/反代常态);
 *   - 连接 5s / 收发 15s 超时,绝不无限挂死下载线程。
 * 重定向后的 body 首块缓存于 leftover(头与体同包到达),read 先排缓存。
 */
#include "ota_update.h"
#include "dg_log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *TAG = "[OTA-HTTP]";

#define HTTP_HDR_CAP   8192
#define HTTP_CHUNK     (16 * 1024)
#define HTTP_TIMEOUT_S 15
#define HTTP_CONN_TIMEOUT_S 5
#define HTTP_MAX_REDIRECT   3

typedef struct {
    int fd;
    uint8_t leftover[HTTP_CHUNK];
    size_t l_len, l_pos;
} http_src_t;

/* ---- socket 工具 ---- */

static int connect_timeout(const char *host, const char *port, int secs)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            break;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
        if (select(fd + 1, NULL, &w, NULL, &tv) != 1) {
            close(fd);
            fd = -1;
            continue;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err == 0) {
            fcntl(fd, F_SETFL, fl);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        struct timeval tv = { .tv_sec = HTTP_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

/* 大小写不敏感子串(避免依赖 GNU strcasestr 特性宏) */
static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] &&
               tolower((unsigned char)hay[i]) == (unsigned char)needle[i])
            i++;
        if (i == n)
            return true;
    }
    return false;
}

/* 大小写不敏感的头部取值(在 [hdr, hdr+hdrlen) 内找 name: value) */
static bool hdr_get(const char *hdr, size_t hdrlen, const char *name,
                    char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = hdr, *end = hdr + hdrlen;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (line > nlen && !strncasecmp(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < p + line && (*v == ' ' || *v == '\t'))
                v++;
            size_t vn = (size_t)(p + line - v);
            while (vn && (v[vn - 1] == '\r' || v[vn - 1] == ' '))
                vn--;
            if (vn >= cap)
                vn = cap - 1;
            memcpy(out, v, vn);
            out[vn] = '\0';
            return true;
        }
        p += line + (eol ? 1 : 0);
    }
    return false;
}

/* 收满 \r\n\r\n(返回头部长度,含结束空行;-1 = 错误/-2 = 截断) */
static int recv_headers(int fd, char *buf, size_t cap, size_t *body_in_buf)
{
    size_t got = 0;
    for (;;) {
        if (got + 1 >= cap)
            return -2;
        ssize_t n = recv(fd, buf + got, cap - got - 1, 0);
        if (n <= 0)
            return -1;
        got += (size_t)n;
        buf[got] = '\0';
        char *sep = strstr(buf, "\r\n\r\n");
        if (sep) {
            *body_in_buf = got - (size_t)(sep + 4 - buf);
            return (int)(sep + 4 - buf);
        }
    }
}

/* http://host[:port]/path → 三段;host/port/path 原地切;path 默认 / */
static bool url_split(const char *url, char *host, size_t hcap,
                      char *port, size_t pcap, const char **path)
{
    if (strncmp(url, "http://", 7))
        return false;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const size_t auth = slash ? (size_t)(slash - p) : strlen(p);
    if (auth == 0 || auth >= hcap)
        return false;
    memcpy(host, p, auth);
    host[auth] = '\0';
    *path = slash ? slash : "/";
    snprintf(port, pcap, "80");
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        snprintf(port, pcap, "%s", colon + 1);
    }
    return true;
}

static int http_open_once(http_src_t *s, const char *url,
                          uint32_t *content_len, char *redirect,
                          size_t rcap)
{
    char host[128], port[8];
    const char *path;
    if (!url_split(url, host, sizeof(host), port, sizeof(port), &path)) {
        DG_LOGE(TAG, "URL 非法或非 http 直链:%s", url);
        return DG_ERR_PARAM;
    }

    s->fd = connect_timeout(host, port, HTTP_CONN_TIMEOUT_S);
    if (s->fd < 0) {
        DG_LOGE(TAG, "连接失败 %s:%s", host, port);
        return DG_ERR_NETWORK;
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s:%s\r\n"
                      "User-Agent: doorguard-ota\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      path, host, port);
    if (rl <= 0 || (size_t)rl >= sizeof(req) ||
        send(s->fd, req, (size_t)rl, 0) != rl) {
        DG_LOGE(TAG, "请求发送失败");
        return DG_ERR_NETWORK;
    }

    char hdr[HTTP_HDR_CAP];
    size_t body_got = 0;
    int hl = recv_headers(s->fd, hdr, sizeof(hdr), &body_got);
    if (hl < 0) {
        DG_LOGE(TAG, "响应头接收失败/超时(%s)", hl == -2 ? "头过长" : "EOF");
        return DG_ERR_NETWORK;
    }

    int code = 0;
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &code) != 1) {
        DG_LOGE(TAG, "响应行解析失败");
        return DG_ERR_NETWORK;
    }
    if (code == 301 || code == 302 || code == 303 || code == 307 ||
        code == 308) {
        if (!hdr_get(hdr, (size_t)hl, "Location", redirect, rcap)) {
            DG_LOGE(TAG, "%d 重定向缺 Location", code);
            return DG_ERR_NETWORK;
        }
        return DG_ERR_NOT_FOUND;            /* 内部约定:调用方识别重定向 */
    }
    if (code != 200) {
        DG_LOGE(TAG, "HTTP %d(源侧拒绝)", code);
        return code == 404 ? DG_ERR_NOT_FOUND : DG_ERR_NETWORK;
    }

    char val[64];
    if (hdr_get(hdr, (size_t)hl, "Transfer-Encoding", val, sizeof(val)) &&
        ci_contains(val, "chunked")) {
        DG_LOGE(TAG, "源为 chunked 流式编码:固件下载需要定长直链"
                     "(静态文件服务器直出即可)");
        return DG_ERR_UNSUPPORTED;
    }
    if (!hdr_get(hdr, (size_t)hl, "Content-Length", val, sizeof(val))) {
        DG_LOGE(TAG, "响应缺 Content-Length(连接关闭语义不可用于校验)");
        return DG_ERR_UNSUPPORTED;
    }
    *content_len = (uint32_t)strtoul(val, NULL, 10);

    if (body_got > sizeof(s->leftover))
        body_got = sizeof(s->leftover);
    memcpy(s->leftover, hdr + hl, body_got);
    s->l_len = body_got;
    s->l_pos = 0;
    return DG_OK;
}

void *ota_http_src_new(void)
{
    return calloc(1, sizeof(http_src_t));
}

void ota_http_src_free(void *src)
{
    free(src);
}

int ota_http_open(void *src, const char *url, uint32_t *content_len)
{
    http_src_t *s = (http_src_t *)src;
    if (!s || !url || !content_len)
        return DG_ERR_PARAM;
    s->fd = -1;
    s->l_len = s->l_pos = 0;

    char cur[256];
    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop <= HTTP_MAX_REDIRECT; hop++) {
        char redirect[256] = "";
        int rc = http_open_once(s, cur, content_len, redirect,
                                sizeof(redirect));
        if (rc == DG_OK)
            return DG_OK;
        if (s->fd >= 0) {
            close(s->fd);
            s->fd = -1;
        }
        if (rc != DG_ERR_NOT_FOUND || !redirect[0])
            return rc;                      /* 真错误(或约定外的 NOT_FOUND) */
        if (!redirect[0] || strlen(redirect) >= sizeof(cur)) {
            DG_LOGE(TAG, "重定向 Location 非法");
            return DG_ERR_NETWORK;
        }
        /* Location 可能是相对路径:仅支持同 host 绝对与 / 开头两种 */
        if (!strncmp(redirect, "http://", 7)) {
            snprintf(cur, sizeof(cur), "%s", redirect);
        } else if (redirect[0] == '/') {
            char host[128], port[8];
            const char *path;
            url_split(cur, host, sizeof(host), port, sizeof(port), &path);
            snprintf(cur, sizeof(cur), "http://%s:%s%s", host, port,
                     redirect);
        } else {
            DG_LOGE(TAG, "重定向为相对名(%s):不跟随,请用直链", redirect);
            return DG_ERR_UNSUPPORTED;
        }
        DG_LOGI(TAG, "跟随重定向 → %s", cur);
    }
    DG_LOGE(TAG, "重定向超过 %d 次", HTTP_MAX_REDIRECT);
    return DG_ERR_NETWORK;
}

int ota_http_read(void *src, uint8_t *buf, size_t cap, size_t *got)
{
    http_src_t *s = (http_src_t *)src;
    *got = 0;
    if (s->l_pos < s->l_len) {              /* 先排头后缓存的首块体 */
        size_t n = s->l_len - s->l_pos;
        if (n > cap)
            n = cap;
        memcpy(buf, s->leftover + s->l_pos, n);
        s->l_pos += n;
        *got = n;
        return DG_OK;
    }
    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return DG_ERR_TIMEOUT;
        return DG_ERR_NETWORK;
    }
    *got = (size_t)n;                        /* 0 = 对端关流 = 收满 */
    return DG_OK;
}

void ota_http_close(void *src)
{
    http_src_t *s = (http_src_t *)src;
    if (s && s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
