/*
 * sim_vision.c — PC 模拟器视觉 mock(#ifdef DG_SIM,main 装配专用)
 *
 * 循环脚本:人脸出现(黄框)→ 每两轮一次命中(user 10001)/ 否则超时失败,
 * 用于模拟器全流程人工走通(验收项)。板上不编译此文件。
 */
#include "event_bus.h"
#include "events.h"
#include "dg_log.h"

#include <pthread.h>
#include <unistd.h>

static bool s_hit_next = true;

static void *sim_vision_thread(void *arg)
{
    (void)arg;
    for (;;) {
        /* 人脸出现:黄框 */
        ev_face_box_t box = { .x = 200, .y = 250, .w = 320, .h = 420,
                              .state = DG_BOX_DETECTED };
        EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
        usleep(800 * 1000);

        if (s_hit_next) {
            /* 1:N 命中(user 10001;可在用户管理页添加) */
            ev_match_t m;
            memset(&m, 0, sizeof(m));
            m.matched = true;
            snprintf(m.user_id, sizeof(m.user_id), "%s", "10001");
            snprintf(m.user_name, sizeof(m.user_name), "%s", "张三");
            m.role = DG_ROLE_NORMAL;
            m.score_permille = 952;
            EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &m);
        } else {
            /* 未命中:框消失,由主页 1.5s 窗口判定失败 */
            EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
        }
        s_hit_next = !s_hit_next;
        usleep(3500 * 1000);
    }
    return NULL;
}

void sim_vision_start(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, sim_vision_thread, NULL) == 0)
        pthread_detach(tid);
    DG_LOGI("[SIM]", "视觉 mock 已启动(命中/失败交替)");
}
