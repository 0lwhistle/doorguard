/*
 * test_card_dedup.c — IC 防重窗纯函数单测(ICCARD_PROTOCOL §7.1)
 *
 * 规则:同一卡号 window 内只放行一次(窗内重复忽略);窗外放行;
 * 不同卡号不互斥;窗口随 door_open_ms 可调。
 */
#include "dg_test.h"
#include "card_provider.h"

#include <string.h>

static void t_window(void)
{
    printf("[P1] 同卡窗内忽略/窗外放行\n");
    card_dedup_t st;
    memset(&st, 0, sizeof(st));

    DG_CHECK(card_dedup_allow(&st, "04A3B2C1", 1000, 3000));
    DG_CHECK(!card_dedup_allow(&st, "04A3B2C1", 1500, 3000));   /* 窗内 */
    DG_CHECK(!card_dedup_allow(&st, "04A3B2C1", 3999, 3000));   /* 恰好窗沿内 */
    DG_CHECK(card_dedup_allow(&st, "04A3B2C1", 4001, 3000));    /* 窗外 */

    printf("[P2] 不同卡号不互斥\n");
    memset(&st, 0, sizeof(st));
    DG_CHECK(card_dedup_allow(&st, "AA", 1000, 3000));
    DG_CHECK(card_dedup_allow(&st, "BB", 1100, 3000));          /* 换卡立即放行 */
    DG_CHECK(!card_dedup_allow(&st, "BB", 1200, 3000));         /* 最近卡才受窗 */

    printf("[P3] 边界:窗口为 0/空卡号/坏参数\n");
    memset(&st, 0, sizeof(st));
    DG_CHECK(card_dedup_allow(&st, "AA", 1000, 0));             /* 窗 0 = 不去重 */
    DG_CHECK(card_dedup_allow(&st, "AA", 1000, 0));
    DG_CHECK(!card_dedup_allow(&st, "", 1000, 3000));           /* 空卡号拒绝 */
    DG_CHECK(!card_dedup_allow(NULL, "AA", 1000, 3000));
}

int main(void)
{
    t_window();
    DG_TEST_EXIT();
}
