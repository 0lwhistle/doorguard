/*
 * test_timeutil.c — 公共时基测试(C2)
 * 单调性/两种时钟语义分名/与秒的一致性。
 */
#include "dg_test.h"
#include "timeutil.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static void nap_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void)
{
    /* REALTIME:非负、与 now_s 一致(±2s 容时钟滴答) */
    const int64_t ms1 = now_ms();
    const int64_t s1 = now_s();
    DG_CHECK(ms1 > 1500000000000LL);            /* 2017 之后(纪元基数 sane) */
    DG_CHECK(llabs(ms1 / 1000 - s1) <= 2);

    nap_ms(20);
    DG_CHECK(now_ms() >= ms1);                  /* 墙钟不倒退(测试窗口内) */

    /* MONOTONIC:严格用于耗时,两次取值不减且 20ms 睡眠可观测 */
    const int64_t mo1 = now_mono_ms();
    nap_ms(20);
    const int64_t mo2 = now_mono_ms();
    DG_CHECK(mo2 >= mo1);
    DG_CHECK(mo2 - mo1 >= 15);                  /* 至少睡到了大半个 20ms */

    /* 两种时钟数值域不同(REALTIME 带纪元基数,MONOTONIC 是开机起的相对值):
     * 混用探测——MONOTONIC 远小于 REALTIME(开机时长 < 纪元) */
    DG_CHECK(now_mono_ms() < now_ms());

    printf("  now_ms=%" PRId64 " now_mono_ms=%" PRId64 " now_s=%" PRId64 "\n",
           ms1, mo1, s1);
    DG_TEST_EXIT();
}
