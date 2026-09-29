/*
 * test_capture_health.c — capture_service 断流判定状态机(宿主 gcc)
 *
 * 停滞判定逻辑(health_eval)是 capture_service.c 的 static 纯函数:测试
 * 直接 include 该 .c,相机事实(camera_stream_on/camera_last_frame_ms)以
 * 同名桩替身供数——板上后端不参与链接。其余依赖(事件总线/tasker/holder)
 * 用真库,发布行为仅验证"publish 标志",不打真总线。
 */
#include "dg_test.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- 相机事实桩(先于被测 .c 定义,符号在链接期顶掉缺失) ---- */
static bool stub_stream_on = false;
static int64_t stub_last_frame_ms = 0;
bool camera_stream_on(void)
{
    return stub_stream_on;
}
int64_t camera_last_frame_ms(void)
{
    return stub_last_frame_ms;
}

#include "../services/capture/capture_service.c"

static void feed(cap_health_state_t *h, bool on, int64_t last, int64_t now,
                 cap_health_out_t *o)
{
    *o = health_eval(h, on, last, now);
}

int main(void)
{
    cap_health_out_t o;

    /* 1. 流未起:静默(WAIT 不发布,开机过渡期不吓 UI) */
    cap_health_state_t h1 = { CAP_H_WAIT, 0 };
    feed(&h1, false, 0, 0, &o);
    DG_CHECK(!o.publish && h1.st == CAP_H_WAIT);
    feed(&h1, false, 0, 100000, &o);
    DG_CHECK(!o.publish && h1.st == CAP_H_WAIT);

    /* 2. 流起 + 帧新鲜 → FLOW,发布 ready=true */
    cap_health_state_t h2 = { CAP_H_WAIT, 0 };
    feed(&h2, true, 1000, 1000, &o);
    DG_CHECK(o.publish && o.ready && !o.down && !o.recovered);
    DG_CHECK(h2.st == CAP_H_FLOW);

    /* 3. 帧变陈旧但未到 5s:静默(2s 新鲜界内与 2~5s 观察期都不发布) */
    feed(&h2, true, 1500, 3500, &o);          /* 龄 2000 = 新鲜界内 */
    DG_CHECK(!o.publish && h2.st == CAP_H_FLOW);
    feed(&h2, true, 1500, 5000, &o);          /* 龄 3500,观察期 */
    DG_CHECK(!o.publish && h2.st == CAP_H_FLOW);

    /* 4. 5s 无新帧 → DOWN,发布 ready=false */
    feed(&h2, true, 1500, 7000, &o);          /* 龄 5500 ≥ 5000 */
    DG_CHECK(o.publish && !o.ready && o.down && !o.recovered);
    DG_CHECK(h2.st == CAP_H_DOWN);

    /* 5. DOWN 中仍无帧:状态变化才发布,不刷屏 */
    feed(&h2, true, 1500, 60000, &o);
    DG_CHECK(!o.publish && h2.st == CAP_H_DOWN);

    /* 6. 恢复:帧回来 → FLOW,发布 ready=true 且带 recovered(holder 复位) */
    feed(&h2, true, 60000, 60500, &o);
    DG_CHECK(o.publish && o.ready && o.recovered && !o.down);
    DG_CHECK(h2.st == CAP_H_FLOW);

    /* 7. 流起了但从未出帧:8s 宽限后 DOWN(相机死在起流阶段的形态) */
    cap_health_state_t h3 = { CAP_H_WAIT, 0 };
    feed(&h3, true, 0, 1000, &o);             /* 首见流,宽限起点 */
    DG_CHECK(!o.publish && h3.st == CAP_H_WAIT);
    feed(&h3, true, 0, 8999, &o);             /* 龄 7999 < 8000 */
    DG_CHECK(!o.publish && h3.st == CAP_H_WAIT);
    feed(&h3, true, 0, 9001, &o);             /* ≥ 8000 */
    DG_CHECK(o.publish && !o.ready && o.down && h3.st == CAP_H_DOWN);

    /* 8. 流中途关掉(板上有 S60 重启等路径):FLOW 内帧陈旧照样判 DOWN */
    cap_health_state_t h4 = { CAP_H_FLOW, 0 };
    feed(&h4, false, 1000, 8000, &o);         /* 龄 7000 */
    DG_CHECK(o.publish && !o.ready && o.down && h4.st == CAP_H_DOWN);

    printf("capture health: ");
    DG_TEST_EXIT();
}
