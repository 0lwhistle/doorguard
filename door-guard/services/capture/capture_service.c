/*
 * capture_service.c — 取流状态服务实现
 *
 * 板上接入点(Phase 8):camera_board 的 V4L2 取流线程失败/断流时置
 * ready=false;服务 1s 巡检,状态变化才广播(避免事件刷屏)。
 */
#include "capture_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "tasker.h"

#include "modules/camera/camera.h"

#include <string.h>

static const char *TAG = "[CAPTURE]";

static struct task_node s_tick_node;
static bool s_running = false;
static bool s_last_ready = false;
static bool s_last_valid = false;

static enum task_t poll_task(void *ctx)
{
    (void)ctx;
    if (!s_running)
        return TASK_OK;

    bool ready = (camera_latest() != NULL);
    if (!s_last_valid || ready != s_last_ready) {
        ev_capture_state_t ev = { .ready = ready };
        EVENT_BUS_PUBLISH(EV_CAPTURE_STATE, &ev);
        s_last_ready = ready;
        s_last_valid = true;
        DG_LOGI(TAG, "相机状态:%s", ready ? "就绪" : "未就绪");
    }
    return TASK_OK;
}

int capture_service_start(void)
{
    if (s_running)
        return DG_OK;
    if (tasker_task_init_li(&s_tick_node, 1000, TASK_CNT_INF, "capture_poll",
                            poll_task, NULL) != TASK_OK ||
        tasker_enqueue(&s_tick_node) != TASK_OK) {
        DG_LOGE(TAG, "巡检任务入队失败");
        return DG_ERR_INTERNAL;
    }
    s_running = true;
    DG_LOGI(TAG, "capture 服务启动");
    return DG_OK;
}

void capture_service_stop(void)
{
    tasker_cancel_by_name("capture_poll");
    s_running = false;
}
