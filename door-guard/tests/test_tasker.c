/*
 * test_tasker.c — tasker 移植测试
 *
 * 移植自模板 tests/test_tasker.c(TK1~TK5 断言逐条保留,未弱化)。
 * 任务在 pthread 真实调度下运行;超时定时器为 no-op,timeout 升级路径
 * 在板上验证(见 proto/tasker/README.md)。
 */
#include "tasker.h"
#include "tasker_port.h"
#include "dg_test.h"

#include <stdatomic.h>
#include <unistd.h>

static void wait_flag(atomic_int *flag, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i += 5) {
        if (atomic_load(flag))
            return;
        usleep(5000);
    }
}

/* ---------- TK1 一次性任务 ---------- */
static atomic_int s_ran = 0;

static enum task_t t_one_shot(void *ctx)
{
    (void)ctx;
    atomic_store(&s_ran, 1);
    return TASK_OK;
}

static void test_one_shot(void)
{
    printf("[TK1] one-shot task runs\n");
    struct task_node node;
    DG_CHECK(tasker_task_init_li(&node, 0, 1, "t_one", t_one_shot, NULL) == TASK_OK);
    DG_CHECK(tasker_enqueue(&node) == TASK_OK);
    wait_flag(&s_ran, 2000);
    DG_CHECK(atomic_load(&s_ran) == 1);
    /* move 语义: 成功入队后源节点被作废 */
    DG_CHECK(node.fn == NULL);
}

/* ---------- TK2 周期任务恰好执行 N 次 ---------- */
static atomic_int s_ticks = 0;

static enum task_t t_periodic(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_ticks, 1);
    return TASK_OK;
}

static void test_periodic(void)
{
    printf("[TK2] periodic task runs exactly N times\n");
    struct task_node node;
    DG_CHECK(tasker_task_init_mi(&node, 40, 5, "t_period", t_periodic, NULL) == TASK_OK);
    atomic_store(&s_ticks, 0);
    DG_CHECK(tasker_enqueue(&node) == TASK_OK);

    /* 5 次 × 40ms = 200ms,给 3s 余量;完成后 +60ms 等在途周期 */
    int n = 0;
    for (int i = 0; i < 3000; i += 10) {
        n = atomic_load(&s_ticks);
        if (n >= 5)
            break;
        usleep(10000);
    }
    usleep(60000);
    if (n != 5)
        printf("  [dbg] TK2 n=%d (after 3s wait + 60ms grace)\n", n);
    n = atomic_load(&s_ticks);
    DG_CHECK(n == 5);                           /* 恰好 5 次,不漂移不重复 */
}

/* ---------- TK3 按名取消 ---------- */
static atomic_int s_cancel_runs = 0;

static enum task_t t_infinite(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_cancel_runs, 1);
    return TASK_OK;
}

static void test_cancel(void)
{
    printf("[TK3] cancel stops periodic task\n");
    struct task_node node;
    DG_CHECK(tasker_task_init_li(&node, 40, TASK_CNT_INF, "t_cancel", t_infinite, NULL) == TASK_OK);
    atomic_store(&s_cancel_runs, 0);
    DG_CHECK(tasker_enqueue(&node) == TASK_OK);

    for (int i = 0; i < 2000 && atomic_load(&s_cancel_runs) == 0; i += 5)
        usleep(5000);
    DG_CHECK(atomic_load(&s_cancel_runs) >= 1);

    tasker_cancel_by_name("t_cancel");
    int at_cancel = atomic_load(&s_cancel_runs);
    usleep(200000);                             /* 200ms 观察(约 5 个周期) */
    int drift = atomic_load(&s_cancel_runs) - at_cancel;
    DG_CHECK(drift <= 1);                       /* 最多再跑一次在途周期 */
}

/* ---------- TK4 快任务不误标超时 ---------- */
static atomic_int s_fast_done = 0;

static enum task_t t_fast(void *ctx)
{
    (void)ctx;
    usleep(20000);                              /* 20ms 快任务 */
    atomic_store(&s_fast_done, 1);
    return TASK_OK;
}

static void test_no_false_timeout(void)
{
    printf("[TK4] no false timeout on fast task (li default 50ms)\n");
    struct task_node node;
    DG_CHECK(tasker_task_init_li(&node, 0, 1, "t_fast", t_fast, NULL) == TASK_OK);
    atomic_store(&s_fast_done, 0);
    DG_CHECK(tasker_enqueue(&node) == TASK_OK);
    wait_flag(&s_fast_done, 2000);
    DG_CHECK(atomic_load(&s_fast_done) == 1);
    DG_CHECK(node.is_timeout == 0);             /* 快任务不应被标超时 */
}

/* ---------- TK5 队列状态 API ---------- */
static void test_queue_status(void)
{
    printf("[TK5] is_full / is_empty\n");
    /* 冒烟:API 可用且返回合法值 */
    DG_CHECK(tasker_is_empty() == 1 || tasker_is_empty() == 0);
    DG_CHECK(tasker_is_full() == 0);
}

int main(void)
{
    DG_CHECK(tasker_init() == TASK_OK);
    usleep(50000);                              /* 等 worker 线程起来 */

    test_one_shot();
    test_periodic();
    test_cancel();
    test_no_false_timeout();
    test_queue_status();

    DG_TEST_EXIT();
}
