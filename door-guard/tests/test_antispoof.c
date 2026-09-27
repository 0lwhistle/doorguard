/*
 * test_antispoof.c — 反欺骗纯算法件测试
 *
 * scale_box 的期望值由官方 python 实现(CropImage._get_new_box)生成后
 * 套用 C 侧偶对齐收缩规则(见 tools/convert_antispoof 的对拍脚本);
 * 数值直接冻结在这里:官方语义改了这里立刻红。
 */
#include "face_antispoof.h"
#include "dg_test.h"

static void test_scale_box(void)
{
    int x, y, w, h;
    /* 居中脸,scale 被窄边压上限(期望值:官方式 (0,0,478,478)) */
    antispoof_scale_box(100, 100, 200, 200, 640, 480, ANTISPOOF_SCALE_A,
                        &x, &y, &w, &h);
    DG_CHECK(x == 0 && y == 0 && w == 478 && h == 478);

    /* 宽屏中央脸,scale 全额生效 */
    antispoof_scale_box(300, 200, 160, 160, 720, 1280, ANTISPOOF_SCALE_A,
                        &x, &y, &w, &h);
    DG_CHECK(x == 164 && y == 64 && w == 432 && h == 432);

    /* 右+上越界:整体平移夹回(官方式 (318,0,400,400)) */
    antispoof_scale_box(600, 100, 100, 100, 720, 480, ANTISPOOF_SCALE_B,
                        &x, &y, &w, &h);
    DG_CHECK(x == 318 && y == 0 && w == 400 && h == 400);

    /* 左上角小脸:框比 scale 后取景更大 → 只能取到 2.68 倍左右 */
    antispoof_scale_box(0, 0, 50, 50, 720, 1280, ANTISPOOF_SCALE_A,
                        &x, &y, &w, &h);
    DG_CHECK(x == 0 && y == 0 && w == 134 && h == 134);

    /* 退化框:全零输出,不越界 */
    antispoof_scale_box(0, 0, 0, 0, 720, 1280, ANTISPOOF_SCALE_A, &x, &y, &w, &h);
    DG_CHECK(x == 0 && y == 0 && w == 0 && h == 0);

    /* 偶对齐与边界收缩的不变量:随机框跑一遍,结果必须仍在图内且偶对齐 */
    for (int i = 0; i < 64; i++) {
        const int sw = 720, sh = 1280;
        int bx = (i * 37) % sw, by = (i * 53) % sh;
        int bw = 40 + (i * 29) % 200, bh = 40 + (i * 31) % 200;
        antispoof_scale_box(bx, by, bw, bh, sw, sh,
                            ANTISPOOF_SCALE_A + (float)i * 0.01f, &x, &y, &w, &h);
        DG_CHECK(w > 0 && h > 0);
        DG_CHECK((x % 2 == 0) && (y % 2 == 0) && (w % 2 == 0) && (h % 2 == 0));
        DG_CHECK(x >= 0 && y >= 0 && x + w <= sw && y + h <= sh);
    }
}

static void test_rgb_to_bgr(void)
{
    /* 首尾字节互换、G 不动;长度奇数像素也要对 */
    const uint8_t rgb[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    uint8_t bgr[9] = { 0 };
    antispoof_rgb_to_bgr(rgb, 3, bgr);
    DG_CHECK(bgr[0] == 3 && bgr[1] == 2 && bgr[2] == 1);
    DG_CHECK(bgr[3] == 6 && bgr[4] == 5 && bgr[5] == 4);
    DG_CHECK(bgr[6] == 9 && bgr[7] == 8 && bgr[8] == 7);
    /* NULL 安全 */
    antispoof_rgb_to_bgr(NULL, 3, bgr);
    antispoof_rgb_to_bgr(rgb, 3, NULL);
    DG_CHECK(1);
}

static void test_smooth_median(void)
{
    antispoof_smooth_t s;
    antispoof_smooth_reset(&s);
    /* 稳定真脸:输出=输入 */
    for (int i = 0; i < ANTISPOOF_SMOOTH_N; i++)
        DG_CHECK(antispoof_smooth_push(&s, 0.90f) > 0.89f);
    /* 单帧离群(一次毛刺)被中位数压住 */
    DG_CHECK(antispoof_smooth_push(&s, 0.10f) > 0.5f);
    /* 持续离群:窗口滚过后中位数跟着走——攻击者骗不过滑动窗 */
    DG_CHECK(antispoof_smooth_push(&s, 0.10f) > 0.5f);
    DG_CHECK(antispoof_smooth_push(&s, 0.10f) < 0.5f);
    /* reset 后首次 push 立即有输出(不用等窗口填满) */
    antispoof_smooth_reset(&s);
    DG_CHECK(antispoof_smooth_push(&s, 0.30f) > 0.29f);
}

static void test_verdict(void)
{
    DG_CHECK(antispoof_is_spoof(0.30f, 0.50f));      /* 低于阈值 = 疑似假体 */
    DG_CHECK(!antispoof_is_spoof(0.60f, 0.50f));
    DG_CHECK(!antispoof_is_spoof(0.50f, 0.50f));     /* 等于阈值不算(边界温和) */
}

int main(void)
{
    test_scale_box();
    test_rgb_to_bgr();
    test_smooth_median();
    test_verdict();
    DG_TEST_EXIT();
}
