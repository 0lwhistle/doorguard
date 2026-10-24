/*
 * test_ota_http.c — ota_http 手写 HTTP 客户端宿主测试(B8-4,2026-10-25)
 *
 * 测试内起回环假源(TCP 127.0.0.1:0,逐连接按剧本应答),覆盖:
 *   200+定长 闭环读取;chunked 拒收;缺 Content-Length 拒收;404;
 *   同 host 302 跟随;/ 相对 Location 跟随;相对名 Location 拒绝;
 *   重定向超环数拒绝。零外部依赖。
 */
#include "dg_test.h"
#include "ota_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 假源:按剧本逐连接应答 ---- */

typedef struct {
    const char *resp;      /* 整段应答(头+体),NULL=直接断开 */
} srv_step_t;

static srv_step_t s_steps[8];
static int s_step_cnt;
static int s_step_idx;
static int s_listen = -1;
static char s_url[64];     /* http://127.0.0.1:<port>/pkg.bin */
static pthread_t s_tid;
static volatile int s_srv_run;
static int s_req_cnt;

static void *srv_thread(void *arg)
{
    (void)arg;
    while (s_srv_run) {
        int c = accept(s_listen, NULL, NULL);
        if (c < 0)
            continue;
        s_req_cnt++;
        char req[512] = { 0 };
        size_t got = 0;
        while (got < sizeof(req) - 1 &&
               recv(c, req + got, 1, 0) == 1) {
            got++;
            if (got >= 4 && !memcmp(req + got - 4, "\r\n\r\n", 4))
                break;
        }
        if (s_step_idx < s_step_cnt) {
            const char *r = s_steps[s_step_idx++].resp;
            if (r)
                (void)!send(c, r, strlen(r), 0);
        }
        close(c);                            /* Connection: close 语义 */
    }
    return NULL;
}

static void srv_start(void)
{
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    DG_CHECK(s_listen >= 0);
    DG_CHECK(bind(s_listen, (struct sockaddr *)&a, sizeof(a)) == 0);
    DG_CHECK(listen(s_listen, 4) == 0);
    socklen_t len = sizeof(a);
    DG_CHECK(getsockname(s_listen, (struct sockaddr *)&a, &len) == 0);
    snprintf(s_url, sizeof(s_url), "http://127.0.0.1:%u/pkg.bin",
             (unsigned)ntohs(a.sin_port));
    s_srv_run = 1;
    s_step_idx = 0;
    s_req_cnt = 0;
    DG_CHECK(pthread_create(&s_tid, NULL, srv_thread, NULL) == 0);
}

static void srv_stop(void)
{
    s_srv_run = 0;
    shutdown(s_listen, SHUT_RDWR);
    close(s_listen);
    s_listen = -1;
    pthread_join(s_tid, NULL);
}

#define BODY "OTA-FW-PAYLOAD-0123456789"

static void reset_steps(void)
{
    memset(s_steps, 0, sizeof(s_steps));
    s_step_cnt = 0;
    s_step_idx = 0;
    s_req_cnt = 0;
}

int main(void)
{
    srv_start();

    /* H1 200+定长:open 报长度,read 取满 */
    printf("[H1] 200 + content-length: full read\n");
    reset_steps();
    static char r1[128];
    snprintf(r1, sizeof(r1),
             "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" BODY,
             (int)strlen(BODY));
    s_steps[0].resp = r1;
    s_step_cnt = 1;
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        DG_CHECK(ota_http_open(src, s_url, &clen) == DG_OK);
        DG_CHECK(clen == strlen(BODY));
        uint8_t buf[256];
        size_t total = 0, got = 0;
        while (ota_http_read(src, buf, sizeof(buf), &got) == DG_OK && got > 0) {
            DG_CHECK(total + got <= clen);
            DG_CHECK(memcmp(buf, BODY + total, got) == 0);
            total += got;
        }
        DG_CHECK(total == clen);
        ota_http_close(src);
        ota_http_src_free(src);
    }

    /* H2 chunked 拒收 */
    printf("[H2] chunked refused\n");
    reset_steps();
    s_steps[0].resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "4\r\nOTA-\r\n0\r\n\r\n";
    s_step_cnt = 1;
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        DG_CHECK(ota_http_open(src, s_url, &clen) == DG_ERR_UNSUPPORTED);
        ota_http_src_free(src);
    }

    /* H3 缺 Content-Length 拒收 */
    printf("[H3] missing content-length refused\n");
    reset_steps();
    s_steps[0].resp = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" BODY;
    s_step_cnt = 1;
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        DG_CHECK(ota_http_open(src, s_url, &clen) == DG_ERR_UNSUPPORTED);
        ota_http_src_free(src);
    }

    /* H4 404 */
    printf("[H4] 404 not found\n");
    reset_steps();
    s_steps[0].resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    s_step_cnt = 1;
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        DG_CHECK(ota_http_open(src, s_url, &clen) == DG_ERR_NOT_FOUND);
        ota_http_src_free(src);
    }

    /* H5 302 → / 相对 Location → 200(同 host 拼接) */
    printf("[H5] 302 with absolute-path location followed\n");
    reset_steps();
    s_steps[0].resp =
        "HTTP/1.1 302 Found\r\nLocation: /real.bin\r\nContent-Length: 0\r\n\r\n";
    static char r5[128];
    snprintf(r5, sizeof(r5),
             "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" BODY,
             (int)strlen(BODY));
    s_steps[1].resp = r5;
    s_step_cnt = 2;
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        DG_CHECK(ota_http_open(src, s_url, &clen) == DG_OK);
        DG_CHECK(clen == strlen(BODY));
        ota_http_close(src);
        ota_http_src_free(src);
        DG_CHECK(s_req_cnt == 2);            /* 确实发了第二跳 */
    }

    /* H6 相对名 Location(无 / 无 http://)拒绝 */
    printf("[H6] relative-name location refused\n");
    reset_steps();
    s_steps[0].resp =
        "HTTP/1.1 302 Found\r\nLocation: other.host/fw.bin\r\nContent-Length: 0\r\n\r\n";
    s_step_cnt = 1;
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        DG_CHECK(ota_http_open(src, s_url, &clen) == DG_ERR_UNSUPPORTED);
        ota_http_src_free(src);
    }

    /* H7 重定向超环数 */
    printf("[H7] redirect loop capped\n");
    reset_steps();
    for (int i = 0; i < 6 && i < 8; i++) {
        s_steps[i].resp =
            "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n";
        s_step_cnt++;
    }
    {
        void *src = ota_http_src_new();
        uint32_t clen = 0;
        int rc = ota_http_open(src, s_url, &clen);
        DG_CHECK(rc != DG_OK);               /* 环数上限:必不成功 */
        ota_http_src_free(src);
    }

    srv_stop();
    DG_TEST_EXIT();
}
