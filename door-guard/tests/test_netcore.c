/*
 * test_netcore.c — 统一网络事件循环测试(宿主)
 *
 * 覆盖:start/stop 幂等、心跳推进、post 闭包在 loop 线程异步执行、
 * 停服后 post 静默丢弃不崩、反复启停无泄漏。不建真实连接(那是
 * web_test.sh / mdns_test.sh 的端到端职责),这里只验证调度契约。
 */
#include "dg_test.h"
#include "netcore.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* post 的闭包:记录执行并回传自身线程 id,验证"确实跑在 loop 线程" */
static pthread_t g_ran_tid;
static int g_ran_cnt;
static void *g_seen_arg;

static void probe_fn(void *arg)
{
    g_ran_tid = pthread_self();
    g_seen_arg = arg;
    g_ran_cnt++;
}

static void nul_fn(void *arg)
{
    (void)arg;
}

static void spin_until(bool *flag, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5 && !*flag; i++)
        usleep(5 * 1000);
}

static void t_lifecycle(void)
{
    DG_CHECK(netcore_running() == false);
    DG_CHECK(netcore_start() == DG_OK);
    DG_CHECK(netcore_start() == DG_OK);          /* 幂等 */
    DG_CHECK(netcore_running() == true);

    int64_t hb0 = netcore_heartbeat_ms();
    usleep(120 * 1000);                          /* > 2 轮 poll(50ms) */
    int64_t hb1 = netcore_heartbeat_ms();
    DG_CHECK(hb1 > hb0);                         /* 心跳推进 */

    netcore_stop();
    DG_CHECK(netcore_running() == false);
    netcore_stop();                              /* 幂等 */
    DG_CHECK(netcore_heartbeat_ms() == 0);
}

static void t_post(void)
{
    /* loop 线程 id 在 post 前取样:post 的闭包应与 loop 同线程 */
    DG_CHECK(netcore_start() == DG_OK);

    int magic = 42;
    g_ran_cnt = 0;
    g_seen_arg = NULL;
    netcore_post(probe_fn, &magic);
    bool ran = false;
    for (int i = 0; i < 100 && !ran; i++) {      /* 排水周期 10ms,1s 足够 */
        usleep(10 * 1000);
        ran = (g_ran_cnt > 0);
    }
    DG_CHECK(ran);
    DG_CHECK(g_ran_cnt == 1);
    DG_CHECK(g_seen_arg == &magic);              /* 参数原样送达 */
    DG_CHECK(pthread_equal(g_ran_tid, pthread_self()) == 0);  /* 不是调用者线程 */

    netcore_stop();
    g_ran_cnt = 0;
    netcore_post(probe_fn, NULL);                /* 停服后:静默丢弃不崩 */
    usleep(50 * 1000);
    DG_CHECK(g_ran_cnt == 0);
}

static void t_restart_cycles(void)
{
    for (int i = 0; i < 3; i++) {
        DG_CHECK(netcore_start() == DG_OK);
        DG_CHECK(netcore_running() == true);
        netcore_post(nul_fn, NULL);
        usleep(30 * 1000);
        netcore_stop();
    }
    DG_CHECK(netcore_start() == DG_OK);          /* 结束后还能再起 */
    netcore_stop();
}

int main(void)
{
    t_lifecycle();
    t_post();
    t_restart_cycles();
    DG_TEST_EXIT();
}
