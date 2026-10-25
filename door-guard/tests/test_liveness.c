/*
 * test_liveness.c — 动作活体状态机宿主测试(B8,无需板子/NPU/相机)
 *
 * 驱动方式:合成 5 关键点序列喂 liveness_service_on_face,订阅
 * EV_LIVENESS_PROMPT 断言出题/通过/收起;判定只用同轴比例,合成脸的
 * 万分比坐标与真实帧形无关。覆盖:出题结构、完成通过、错误动作不动步、
 * 超时重出、离场复位(陈旧通过不继承)、非 5 点降级、模式门、随机性。
 * cfg 用 /tmp 临时文件装载(liveness_timeout_ms 可调小做超时用例)。
 */
#include "dg_test.h"
#include "liveness_service.h"
#include "events.h"
#include "event_bus.h"
#include "cfg.h"
#include "vision_service.h"              /* DG_VMODE_*(头文件枚举,不链 dg_vision) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 提示记录器 ---- */

#define REC_MAX 128
static ev_liveness_t s_recs[REC_MAX];
static int s_cnt;

static int on_prompt(const event_t *e, void *ud)
{
    (void)ud;
    if (s_cnt < REC_MAX)
        s_recs[s_cnt++] = *(const ev_liveness_t *)e->data;
    return 0;
}

static void rec_reset(void)
{
    s_cnt = 0;
}

static void pump(int ms)
{
    usleep((useconds_t)ms * 1000);      /* 总线分发线程 10ms 轮询 */
}

static const ev_liveness_t *last_prompt(int8_t phase)
{
    for (int i = s_cnt - 1; i >= 0; i--)
        if (s_recs[i].phase == phase)
            return &s_recs[i];
    return NULL;
}

/* ---- 合成 5 点脸(万分比)与动作注入 ----
 * 眼(3000,4000)/(7000,4000),鼻(5000,5600),嘴角(3800,9200)/(6200,9200);
 * IPD=4000(x 轴),眼-嘴 5200(y 轴) */

static void mk_face(dg_face_pt_t *p, int nose_dx, int nose_dy)
{
    p[0].x = 3000; p[0].y = 4000;       /* 左眼 */
    p[1].x = 7000; p[1].y = 4000;       /* 右眼 */
    p[2].x = 5000 + nose_dx; p[2].y = 5600 + nose_dy; /* 鼻尖 */
    p[3].x = 3800; p[3].y = 9200;       /* 左嘴角 */
    p[4].x = 6200; p[4].y = 9200;       /* 右嘴角 */
}

static void feed(int frames, int nose_dx, int nose_dy)
{
    dg_face_pt_t p[5];
    mk_face(p, nose_dx, nose_dy);
    for (int i = 0; i < frames; i++)
        DG_CHECK(liveness_service_on_face(p, 5, 0) == DG_OK);
}

/* 完成一个动作(幅度取阈值 1.1 倍以上,留余量):
 * 摇头 = x 偏移 +0.32×IPD 持续 → 对侧 −0.26×IPD 持续
 * 点头 = y 下移 +0.16×眼嘴距 持续 → 回基线持续 */
static void do_action(int act)
{
    if (act == DG_LIVE_ACTION_SHAKE) {
        feed(6, (int)(0.32f * 4000), 0);
        feed(6, (int)(-0.26f * 4000), 0);
        feed(2, 0, 0);
    } else {
        feed(6, 0, (int)(0.16f * 5200));
        feed(6, 0, 0);
        feed(2, 0, 0);
    }
}

static void publish_face_lost(void)
{
    EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
}

static void publish_set_mode(int32_t mode)
{
    ev_vision_mode_t m;
    memset(&m, 0, sizeof(m));
    m.mode = mode;
    EVENT_BUS_PUBLISH(EV_VISION_SET_MODE, &m);
}

/* 静置若干帧(不触发任何动作的小抖动)喂够 ARMING+出题 */
static void arm_challenge(void)
{
    feed(30, 0, 0);
    pump(60);
}

/* 交互式完成当前序列:按提示逐动作执行,返回是否走到 PASSED */
static void perform_sequence(void)
{
    int done = 0;
    for (int guard = 0; guard < 12; guard++) {
        pump(40);
        const ev_liveness_t *st = last_prompt(DG_LIVE_PHASE_STEP);
        if (!st)
            break;
        if (st->step > done) {
            done = st->step;
            do_action(st->action);
        }
        if (last_prompt(DG_LIVE_PHASE_PASSED))
            return;
    }
    DG_CHECK(last_prompt(DG_LIVE_PHASE_PASSED) != NULL);
}

/* ---- cfg 装载(/tmp 临时文件;键集只放 liveness 三键,其余走内置默认) ---- */

static void cfg_setup(void)
{
    const char *def_path = "/tmp/dg_test_liveness_def.json";
    const char *cur_path = "/tmp/dg_test_liveness_cur.json";
    FILE *f = fopen(def_path, "wb");
    DG_CHECK(f != NULL);
    fputs("{\"face\":{\"liveness_actions_min\":2,\"liveness_actions_max\":3,"
          "\"liveness_timeout_ms\":5000}}", f);
    fclose(f);
    remove(cur_path);                   /* cur 缺席 = 全默认 */
    DG_CHECK(cfg_load(def_path, cur_path) == DG_OK);
}

/* ---- 用例 ---- */

static void t01_arm_and_pass(void)
{
    printf("[L01] 静态脸只出题不放行;按提示完成→PASSED→pass 粘滞\n");
    DG_CHECK(liveness_service_pass() == false);         /* 初始不放行 */
    rec_reset();
    arm_challenge();
    const ev_liveness_t *st = last_prompt(DG_LIVE_PHASE_STEP);
    DG_CHECK(st != NULL);                               /* 出题了 */
    DG_CHECK(st->step == 1);
    DG_CHECK(st->total >= 2 && st->total <= 3);
    DG_CHECK(st->action == DG_LIVE_ACTION_NOD ||
             st->action == DG_LIVE_ACTION_SHAKE);
    DG_CHECK(liveness_service_pass() == false);         /* 未完成不放行 */

    perform_sequence();
    DG_CHECK(last_prompt(DG_LIVE_PHASE_PASSED) != NULL);
    DG_CHECK(liveness_service_pass() == true);
    feed(10, 0, 0);                                     /* 在场内保持粘滞 */
    pump(30);
    DG_CHECK(liveness_service_pass() == true);
}

static void t02_wrong_action_no_advance(void)
{
    printf("[L02] 幅度不足的小动作不推进步(防抖)\n");
    publish_face_lost();
    pump(30);
    rec_reset();
    arm_challenge();
    feed(20, (int)(0.05f * 4000), (int)(0.02f * 5200)); /* 远低于阈值 */
    pump(30);
    const ev_liveness_t *st = last_prompt(DG_LIVE_PHASE_STEP);
    DG_CHECK(st != NULL && st->step == 1);              /* 还在第一步 */
    DG_CHECK(liveness_service_pass() == false);
    perform_sequence();
    DG_CHECK(liveness_service_pass() == true);
}

static void t03_lost_clears_pass(void)
{
    printf("[L03] 离场复位:通过不继承给下一场(照片顶替防线)\n");
    publish_face_lost();
    pump(30);
    DG_CHECK(liveness_service_pass() == false);         /* 立即失效 */
    rec_reset();
    arm_challenge();                                    /* 新一场重新出题 */
    DG_CHECK(last_prompt(DG_LIVE_PHASE_STEP) != NULL);
    DG_CHECK(liveness_service_pass() == false);
    perform_sequence();
    DG_CHECK(liveness_service_pass() == true);
}

static void t04_timeout_redeal(void)
{
    printf("[L04] 单步超时重出题(限时 1s 档)\n");
    DG_CHECK(cfg_set_int("liveness_timeout_ms", 1000) == DG_OK);
    publish_face_lost();
    pump(30);
    rec_reset();
    arm_challenge();
    DG_CHECK(last_prompt(DG_LIVE_PHASE_STEP) != NULL);
    int prompts_before = s_cnt;
    usleep(1300000);                    /* 跨过 1s 限时 */
    feed(15, 0, 0);                     /* 下一帧判超时→重采基线→重出题 */
    pump(60);
    DG_CHECK(s_cnt > prompts_before);   /* 重出题:又有新 STEP */
    const ev_liveness_t *st = last_prompt(DG_LIVE_PHASE_STEP);
    DG_CHECK(st && st->step == 1);
    DG_CHECK(liveness_service_pass() == false);
    perform_sequence();
    DG_CHECK(liveness_service_pass() == true);
    DG_CHECK(cfg_set_int("liveness_timeout_ms", 5000) == DG_OK);
}

static void t05_degrade_non5(void)
{
    printf("[L05] 非 5 点持续>2s 降级放行;离场恢复严格\n");
    publish_face_lost();
    pump(30);
    rec_reset();
    arm_challenge();                    /* 5 点先正常在场 */
    dg_face_pt_t p6[6];
    for (int i = 0; i < 6; i++) {
        p6[i].x = 3000 + i * 500;
        p6[i].y = 4000 + i * 300;
    }
    for (int i = 0; i < 80; i++) {      /* 80×30ms ≈ 2.4s > 2s 降级线 */
        DG_CHECK(liveness_service_on_face(p6, 6, 0) == DG_OK);
        usleep(30000);
    }
    pump(30);
    DG_CHECK(liveness_service_pass() == true);          /* 降级放行 */
    publish_face_lost();
    pump(30);
    DG_CHECK(liveness_service_pass() == false);         /* 离场收回 */
}

static void t06_mode_gate(void)
{
    printf("[L06] 非匹配模式不出题;回到 1N 恢复\n");
    publish_set_mode(DG_VMODE_DETECT_ONLY);
    pump(30);
    rec_reset();
    feed(30, 0, 0);
    pump(30);
    DG_CHECK(last_prompt(DG_LIVE_PHASE_STEP) == NULL);  /* 不出题 */
    DG_CHECK(liveness_service_pass() == false);
    publish_set_mode(DG_VMODE_DETECT_1N);
    pump(30);
    arm_challenge();
    DG_CHECK(last_prompt(DG_LIVE_PHASE_STEP) != NULL);
    perform_sequence();
    DG_CHECK(liveness_service_pass() == true);
}

static void t07_random_shapes(void)
{
    printf("[L07] 随机性:步数 2/3 都出现,首步动作两种都有\n");
    int seen2 = 0, seen3 = 0, nod_first = 0, shake_first = 0;
    for (int i = 0; i < 24; i++) {
        publish_face_lost();
        pump(25);
        rec_reset();
        arm_challenge();
        const ev_liveness_t *st = last_prompt(DG_LIVE_PHASE_STEP);
        DG_CHECK(st != NULL);
        if (st->total == 2) seen2++;
        if (st->total == 3) seen3++;
        if (st->action == DG_LIVE_ACTION_NOD) nod_first++;
        else shake_first++;
        if (st->total == 2)
            do_action(st->action);      /* 顺手走完,避免下一场前动作悬着 */
    }
    printf("    步数2=%d 步数3=%d 首步点头=%d 摇头=%d\n",
           seen2, seen3, nod_first, shake_first);
    DG_CHECK(seen2 > 0 && seen3 > 0);
    DG_CHECK(nod_first > 0 && shake_first > 0);
    publish_face_lost();
    pump(25);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(event_bus_subscribe(EV_LIVENESS_PROMPT, on_prompt, NULL) != 0);
    cfg_setup();
    DG_CHECK(liveness_service_start() == DG_OK);

    t01_arm_and_pass();
    t02_wrong_action_no_advance();
    t03_lost_clears_pass();
    t04_timeout_redeal();
    t05_degrade_non5();
    t06_mode_gate();
    t07_random_shapes();

    event_bus_deinit();
    DG_TEST_EXIT();
}
