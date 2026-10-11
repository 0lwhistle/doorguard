/*
 * test_fp_sync.c — 指纹两侧数量一致性策略(纯判定 + 状态/告知)
 *
 * 本模块的全部价值在边界:0/0(全新设备)、0/N、N/0、N/M、N/N。
 * 其中 0/0 最容易被误写成"两方有一方为 0 就清另一方"而落进单边分支;
 * 而 0/N 与 N/0 的处置方向**相反**,顺序判错就会删错边。
 * 纪律也在此固化:判定只吃"成功读到的两个数"——读失败不能折算成 0
 * (那是上层 provider 的重试/健康检查职责,见 fp_sync.h)。
 */
#include "fp_sync.h"
#include "dg_test.h"

static void test_decide(void)
{
    printf("[P1] fp_sync_decide 边界\n");

    /* 双双为 0:全新设备,无动作——必须先于单边分支判定 */
    DG_CHECK(fp_sync_decide(0, 0) == FP_SYNC_OK);

    /* 单边为 0:模组有模板无归属 → 清模组;板端有行无模板 → 清板端 */
    DG_CHECK(fp_sync_decide(0, 1) == FP_SYNC_CLEAR_MODULE);
    DG_CHECK(fp_sync_decide(0, 999) == FP_SYNC_CLEAR_MODULE);
    DG_CHECK(fp_sync_decide(1, 0) == FP_SYNC_CLEAR_DB);
    DG_CHECK(fp_sync_decide(2000, 0) == FP_SYNC_CLEAR_DB);

    /* 都非零且相等:一致(含 1/1 这种最小非平凡态) */
    DG_CHECK(fp_sync_decide(1, 1) == FP_SYNC_OK);
    DG_CHECK(fp_sync_decide(3, 3) == FP_SYNC_OK);
    DG_CHECK(fp_sync_decide(2000, 2000) == FP_SYNC_OK);

    /* 都非零但不等:两个方向都只告知,不动手 */
    DG_CHECK(fp_sync_decide(5, 7) == FP_SYNC_MISMATCH);
    DG_CHECK(fp_sync_decide(7, 5) == FP_SYNC_MISMATCH);
    DG_CHECK(fp_sync_decide(1, 999) == FP_SYNC_MISMATCH);
    DG_CHECK(fp_sync_decide(999, 1) == FP_SYNC_MISMATCH);
}

static void test_notice(void)
{
    printf("[P2] 告知:取走即清 + 优先级\n");
    int32_t n = -1;

    /* 空槽:没有待告知 */
    DG_CHECK(fp_sync_take_notice(&n) == false);

    /* 一次只出一条,取走即清 */
    fp_sync_push_notice(FP_NOTICE_MISMATCH);
    DG_CHECK(fp_sync_take_notice(&n) == true && n == FP_NOTICE_MISMATCH);
    DG_CHECK(fp_sync_take_notice(&n) == false);

    /* 低优先级不许顶掉高优先级:动作类在槽里时,读失败要丢掉 */
    fp_sync_push_notice(FP_NOTICE_CLEARED_DB);
    fp_sync_push_notice(FP_NOTICE_READ_FAIL);
    DG_CHECK(fp_sync_take_notice(&n) == true && n == FP_NOTICE_CLEARED_DB);

    /* 反过来(先读失败、后动作类)高优先顶上 */
    fp_sync_push_notice(FP_NOTICE_READ_FAIL);
    fp_sync_push_notice(FP_NOTICE_CLEARED_MODULE);
    DG_CHECK(fp_sync_take_notice(&n) == true && n == FP_NOTICE_CLEARED_MODULE);

    /* 同级后者覆盖:以最后一次动作为准 */
    fp_sync_push_notice(FP_NOTICE_CLEARED_MODULE);
    fp_sync_push_notice(FP_NOTICE_CLEARED_DB);
    DG_CHECK(fp_sync_take_notice(&n) == true && n == FP_NOTICE_CLEARED_DB);

    /* 不一致 > 读失败(用户要看到的那条别被信息类噪音盖掉) */
    fp_sync_push_notice(FP_NOTICE_READ_FAIL);
    fp_sync_push_notice(FP_NOTICE_MISMATCH);
    DG_CHECK(fp_sync_take_notice(&n) == true && n == FP_NOTICE_MISMATCH);

    /* 空指针不崩(UI 可能只想知道"有没有") */
    DG_CHECK(fp_sync_take_notice(NULL) == false);
    fp_sync_push_notice(FP_NOTICE_MISMATCH);
    DG_CHECK(fp_sync_take_notice(NULL) == true);
}

static void test_status(void)
{
    printf("[P3] 状态快照\n");
    fp_sync_status_t st;

    fp_sync_status(&st);
    DG_CHECK(st.checked == false);            /* 零初始化:还没检查过 */

    fp_sync_publish(3, 5, FP_SYNC_MISMATCH, 0, 12345);
    fp_sync_status(&st);
    DG_CHECK(st.checked == true);
    DG_CHECK(st.n_db == 3 && st.n_mod == 5);
    DG_CHECK(st.verdict == FP_SYNC_MISMATCH);
    DG_CHECK(st.err == 0 && st.at_ms == 12345);

    /* 再写一次覆盖(每次开机检查都刷新) */
    fp_sync_publish(0, 0, FP_SYNC_OK, 0, 99);
    fp_sync_status(&st);
    DG_CHECK(st.n_db == 0 && st.n_mod == 0 && st.verdict == FP_SYNC_OK);

    fp_sync_status(NULL);                     /* 空指针不崩 */
}

int main(void)
{
    test_decide();
    test_notice();
    test_status();
    DG_TEST_EXIT();
}
