/*
 * vision_sim.c — PC mock 视觉后端(原 sim/sim_vision.c 收编为服务后端)
 *
 * 1. 周期 mock(可选,DG_SIM_VISION=0 关):人脸出现→命中(user 10001)/
 *    超时失败交替,驱动主页全流程演示;
 * 2. 录入抓取:响应 EV_VISION_CAPTURE_REQ,提交确定性伪特征
 *    (user_id 哈希派生:同人同特征 → 重录查重命中;异人不同特征)。
 */
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "vision_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool s_started = false;

/* ---- 周期 mock 线程 ---- */

static void *mock_thread(void *arg)
{
    (void)arg;
    bool hit_next = true;
    for (;;) {
        ev_face_box_t box = { .x = 200, .y = 250, .w = 320, .h = 420,
                              .state = DG_BOX_DETECTED };
        EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
        usleep(800 * 1000);

        if (hit_next) {
            ev_match_t m;
            memset(&m, 0, sizeof(m));
            m.matched = true;
            snprintf(m.user_id, sizeof(m.user_id), "%s", "10001");
            snprintf(m.user_name, sizeof(m.user_name), "%s", "张三");
            m.role = DG_ROLE_NORMAL;
            m.score_permille = 952;
            EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &m);
        } else {
            EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
        }
        hit_next = !hit_next;
        usleep(3500 * 1000);
    }
    return NULL;
}

/* ---- 录入抓取:确定性伪特征(演示/测试用) ---- */

static int on_capture_req(const event_t *e, void *ud)
{
    (void)ud;
    const ev_capture_req_t *r = (const ev_capture_req_t *)e->data;

    /* 伪特征 = user_id 字节重复展开(同人恒定,异人不同;查重可判定) */
    uint8_t feat[64];
    size_t len = 32;
    uint8_t seed = 0;
    for (const char *p = r->user_id; *p; p++)
        seed ^= (uint8_t)*p;
    memset(feat, seed, sizeof(feat));

    static uint32_t s_seq = 0;              /* 槽位句柄:进程内单调 */
    uint32_t seq = ++s_seq;
    vision_service_submit_feature(r->user_id, seq, feat, len);
    return 0;
}

/* 后端启动:PC 恒成功(mock 与摄像头 sim 解耦;工作模式由服务层持有,
 * 本后端不按模式分支:PC 只验证 UI/FSM 链路,人脸口径以板上 ROCKIVA 为准) */
int vision_backend_start(bool enable_mock)
{
    if (s_started)
        return DG_OK;
    s_started = true;

    /* 录入抓取订阅:bus 分发线程回调,无需独立线程 */
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req, NULL);

    if (enable_mock) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, mock_thread, NULL) == 0)
            pthread_detach(tid);
        DG_LOGI("[VISION]", "mock 周期事件已启动");
    }
    return DG_OK;
}
