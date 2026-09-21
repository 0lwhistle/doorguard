/*
 * test_npu_pre.c — letterbox 坐标数学单元测试(宿主,无需 RGA/板子)
 *
 * 只测**纯数学**(npu_letterbox_plan / npu_letterbox_unmap):
 * 缩放系数、居中补边、以及"模型输入中心 ↔ 源图中心"这个最有力的自检。
 * RGA 那条硬件路径没法在宿主上测,留到板上验——这正是把数学与硬件拆开的原因。
 */
#include "dg_test.h"
#include "npu_pre.h"

#include <math.h>

/* ---- 16:9 相机 → 方形模型输入 ------------------------------------------- */

static void test_plan_1280x720(void)
{
    npu_letterbox_t lb;
    npu_letterbox_plan(1280, 720, 320, 320, &lb);

    DG_CHECK(lb.scale > 0.2499f && lb.scale < 0.2501f);   /* min(0.25, 0.444) */
    DG_CHECK(lb.fit_w == 320);                            /* 宽先顶满 */
    DG_CHECK(lb.fit_h == 180);
    DG_CHECK(lb.pad_x == 0);
    DG_CHECK(lb.pad_y == 70);                             /* (320-180)/2 */
}

static void test_plan_square(void)
{
    npu_letterbox_t lb;
    npu_letterbox_plan(320, 320, 320, 320, &lb);
    DG_CHECK(lb.scale > 0.9999f && lb.scale < 1.0001f);
    DG_CHECK(lb.pad_x == 0 && lb.pad_y == 0);
    DG_CHECK(lb.fit_w == 320 && lb.fit_h == 320);
}

static void test_plan_invalid(void)
{
    npu_letterbox_t lb;
    npu_letterbox_plan(0, 720, 320, 320, &lb);
    DG_CHECK(lb.scale == 0.0f);          /* 非法输入清零,不产生除零 */
    npu_letterbox_plan(-1, -1, 0, 0, &lb);
    DG_CHECK(lb.scale == 0.0f);
    npu_letterbox_plan(1280, 720, 320, 320, NULL);        /* NULL 安全 */
}

/* ---- 逆映射:最有力的自检是"中心映中心" ------------------------------- */

static void test_unmap_center(void)
{
    npu_letterbox_t lb;
    npu_letterbox_plan(1280, 720, 320, 320, &lb);

    /* 模型输入画布中心 (160,160) 应落到源图中心 (640,360) */
    float x = 0, y = 0;
    npu_letterbox_unmap(&lb, 160.0f, 160.0f, &x, &y);
    DG_CHECK(fabsf(x - 640.0f) < 0.5f);
    DG_CHECK(fabsf(y - 360.0f) < 0.5f);
}

/* 逆映射与"缩放+补边"的正向关系一致:模型坐标 = 源坐标×scale + pad */
static void test_unmap_forward_consistency(void)
{
    npu_letterbox_t lb;
    npu_letterbox_plan(1280, 720, 320, 320, &lb);

    const float sx = 100.0f, sy = 50.0f;                  /* 源图上的点 */
    const float mx = sx * lb.scale + (float)lb.pad_x;     /* 它该落在哪 */
    const float my = sy * lb.scale + (float)lb.pad_y;

    float x = 0, y = 0;
    npu_letterbox_unmap(&lb, mx, my, &x, &y);
    DG_CHECK(fabsf(x - sx) < 0.01f);
    DG_CHECK(fabsf(y - sy) < 0.01f);
}

/* 非法计划时退化为恒等(不产生 NaN 坐标) */
static void test_unmap_degenerate(void)
{
    npu_letterbox_t lb;
    npu_letterbox_plan(0, 0, 0, 0, &lb);
    float x = -1, y = -1;
    npu_letterbox_unmap(&lb, 3.0f, 4.0f, &x, &y);
    DG_CHECK(x == 3.0f && y == 4.0f);
}

int main(void)
{
    test_plan_1280x720();
    test_plan_square();
    test_plan_invalid();
    test_unmap_center();
    test_unmap_forward_consistency();
    test_unmap_degenerate();
    DG_TEST_EXIT();
}
