/*
 * test_ntp.c — NTP 服务 socket 应答层回环测试(B8-5,2026-10-25)
 *
 * 测试内起最小 SNTP 假源(UDP 127.0.0.1:123),应答「当前系统时间」的
 * 48B NTP v4 服务端包(步进≈0,不扰动宿主时钟)。cfg ntp_server 指向
 * 127.0.0.1(数值主机名,免 DNS)。断言:trigger 异步回执 EV_NET_NTP_RESULT
 * ok。宿主无外网(ping 探活失败)时 SKIP——socket 层用例只在真在线环境跑。
 */
#include "dg_test.h"
#include "event_bus.h"
#include "events.h"
#include "netcore.h"
#include "ntp_service.h"
#include "cfg.h"
#include "net_info.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int s_srv = -1;
static pthread_t s_tid;
static volatile int s_srv_run;
static volatile int s_req_cnt;

static void *srv_thread(void *arg)
{
    (void)arg;
    while (s_srv_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        uint8_t req[128];
        ssize_t n = recvfrom(s_srv, req, sizeof(req), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 0)
            continue;                        /* 超时轮询 s_srv_run */
        if (n < 48)
            continue;
        s_req_cnt++;
        /* 当前系统时间 → NTP 时间戳(1900 基准);步进≈0,不动宿主时钟 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t sec = (uint64_t)ts.tv_sec + 2208988800ull;
        uint64_t frac = (uint64_t)((double)ts.tv_nsec / 1e9 * 4294967296.0);
        uint8_t rep[48] = { 0 };
        rep[0] = 0x24;                       /* LI=0 VN=4 Mode=4(server) */
        rep[1] = 1;                          /* stratum(0=KISS-OF-DEATH,mg_sntp 拒收) */
        for (int i = 0; i < 8; i++)
            rep[40 + i] = (uint8_t)(sec >> (56 - 8 * i));   /* transmit sec */
        for (int i = 0; i < 8; i++)
            rep[44 + 4 + i] = (uint8_t)(frac >> (56 - 8 * i)); /* transmit frac */
        (void)!sendto(s_srv, rep, sizeof(rep), 0,
                      (struct sockaddr *)&from, flen);
    }
    return NULL;
}

static pthread_mutex_t s_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static bool s_ntp_ok;
static bool s_ntp_got;

static int on_ntp_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ntp_result_t *ev = (const ev_ntp_result_t *)e->data;
    pthread_mutex_lock(&s_ev_mu);
    s_ntp_got = true;
    s_ntp_ok = ev->ok;
    pthread_mutex_unlock(&s_ev_mu);
    return 0;
}

int main(void)
{
    /* 内存态 cfg + ntp_server=127.0.0.1(数值主机免 DNS) */
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/dg_ntp_%d", (int)getpid());
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0)
        exit(1);
    DG_CHECK(cfg_load(NULL, NULL) == DG_OK); /* 无路径 = 纯内存模式 */
    DG_CHECK(cfg_set_str("ntp_server", "127.0.0.1") == DG_OK);

    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(event_bus_subscribe(EV_NET_NTP_RESULT, on_ntp_result, NULL) != NULL);
    DG_CHECK(netcore_start() == DG_OK);
    DG_CHECK(ntp_service_start() == DG_OK);

    if (!net_info_is_online()) {
        printf("[NTP] 宿主离线(ping 探活失败):socket 层用例 SKIP\n");
        netcore_stop();
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        (void)system(cmd);
        DG_TEST_EXIT();
    }

    /* 假源就位 → 触发 → 等回执(≤10s) */
    s_srv = socket(AF_INET, SOCK_DGRAM, 0);
    DG_CHECK(s_srv >= 0);
    struct timeval rxto = { 1, 0 };      /* 收包超时:收尾 join 不挂死 */
    setsockopt(s_srv, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(123);
    DG_CHECK(bind(s_srv, (struct sockaddr *)&a, sizeof(a)) == 0);
    s_srv_run = 1;
    DG_CHECK(pthread_create(&s_tid, NULL, srv_thread, NULL) == 0);

    DG_CHECK(ntp_service_trigger() == DG_OK);
    for (int i = 0; i < 1000 && !s_ntp_got; i++) {
        pthread_mutex_lock(&s_ev_mu);
        bool got = s_ntp_got;
        pthread_mutex_unlock(&s_ev_mu);
        if (got)
            break;
        usleep(10 * 1000);
    }
    DG_CHECK(s_ntp_got);                     /* 回执必达 */
    DG_CHECK(s_ntp_ok);                      /* 假源应答被接受 */
    DG_CHECK(s_req_cnt >= 1);                /* 真发过 SNTP 请求 */

    s_srv_run = 0;
    close(s_srv);
    pthread_join(s_tid, NULL);
    netcore_stop();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
    DG_TEST_EXIT();
}
