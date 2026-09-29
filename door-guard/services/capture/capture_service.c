/*
 * capture_service.c — 取流状态服务实现
 *
 * 板上接入点(Phase 8):camera_board 的 V4L2 取流线程失败/断流时,
 * 帧事实(camera_last_frame_ms)停止刷新;本服务 1s 巡检判停。
 *
 * 为什么不用 camera_latest()!=NULL 判就绪(旧实现):它只能发现"从未
 * 出流",发现不了"流过但死了"——传感器死机后 latest 永远是旧帧,UI
 * 冻在最后一帧(2026-09-30 用户实拍现象:偶发死机、重启都救不回)。
 *
 * 状态机(WAIT→FLOW→DOWN):
 *   WAIT   流未起,不发布(UI 不因开机过渡期闪提示)
 *   FLOW   帧新鲜(2s 内有新帧);进入时发布 ready=true
 *   DOWN   流起了但 5s 无新帧(或 8s 宽限后从未出帧);发布 ready=false
 * 纯逻辑在 capture_health_eval(宿主可测,test_capture_health.c),
 * 发布/holder 置态等副作用在 poll_task(板上线程)。
 */
#include "capture_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "holder.h"
#include "tasker.h"
#include "timeutil.h"

#include "modules/camera/camera.h"

#include <string.h>

static const char *TAG = "[CAPTURE]";

/* 判停阈值:30fps 流 2s 无新帧即可疑,5s 无新帧判死(检出延迟 5~6s);
 * 流起了但从未出帧的宽限给 8s(3A 收敛 + 驱动首帧延迟) */
#define CAP_FRESH_MS 2000
#define CAP_STALL_MS 5000
#define CAP_NEVER_MS 8000

typedef enum {
    CAP_H_WAIT = 0,                        /* 流未起 */
    CAP_H_FLOW,                            /* 帧新鲜 */
    CAP_H_DOWN,                            /* 断流(含硬件级死机) */
} cap_health_t;

/* eval 输入/输出:状态跨 tick 持有在服务;wait_t0=首次见流时刻(宽限起点) */
typedef struct {
    cap_health_t st;
    int64_t wait_t0;
} cap_health_state_t;

typedef struct {
    bool publish;                          /* 本 tick 需发布状态变化 */
    bool ready;                            /* 发布值 */
    bool down;                             /* 刚进入 DOWN(holder→ERROR) */
    bool recovered;                        /* 刚恢复 FLOW(holder→READY) */
} cap_health_out_t;

static cap_health_state_t s_h = { CAP_H_WAIT, 0 };
static volatile bool s_ready_published = true; /* 未发布过=乐观 true(见头注) */
static struct task_node s_tick_node;
static bool s_running = false;

static bool frame_fresh(int64_t last_frame_ms, int64_t now)
{
    return last_frame_ms > 0 && now - last_frame_ms <= CAP_FRESH_MS;
}

/* 纯函数:每 tick 喂一次流事实,得转移与发布需求 */
static cap_health_out_t health_eval(cap_health_state_t *h, bool stream_on,
                                    int64_t last_frame_ms, int64_t now)
{
    cap_health_out_t o = { false, false, false, false };

    switch (h->st) {
    case CAP_H_WAIT:
        if (!stream_on)
            break;
        if (h->wait_t0 == 0)
            h->wait_t0 = now;
        if (frame_fresh(last_frame_ms, now)) {
            h->st = CAP_H_FLOW;
            o.publish = true;
            o.ready = true;
        } else if (now - h->wait_t0 >= CAP_NEVER_MS) {
            h->st = CAP_H_DOWN;
            o.publish = true;
            o.down = true;
        }
        break;
    case CAP_H_FLOW:
        if (frame_fresh(last_frame_ms, now))
            break;
        if (last_frame_ms > 0 && now - last_frame_ms >= CAP_STALL_MS) {
            h->st = CAP_H_DOWN;
            o.publish = true;
            o.down = true;
        }
        break;
    case CAP_H_DOWN:
        if (frame_fresh(last_frame_ms, now)) {
            h->st = CAP_H_FLOW;
            h->wait_t0 = 0;
            o.publish = true;
            o.ready = true;
            o.recovered = true;
        }
        break;
    }
    return o;
}

static enum task_t poll_task(void *ctx)
{
    (void)ctx;
    if (!s_running)
        return TASK_OK;

    cap_health_out_t o = health_eval(&s_h, camera_stream_on(),
                                     camera_last_frame_ms(), now_ms());
    if (!o.publish)
        return TASK_OK;

    ev_capture_state_t ev = { .ready = o.ready };
    EVENT_BUS_PUBLISH(EV_CAPTURE_STATE, &ev);
    s_ready_published = o.ready;
    /* holder 是运行期健康登记表:断流=ERROR(看门狗不管选修模块,不重启),
     * 恢复=READY——holder_is_module_ready("camera") 从此可答"相机能不能用" */
    holder_set_module_state("camera", o.ready ? HOLDER_MODULE_STATE_READY
                                              : HOLDER_MODULE_STATE_ERROR);
    if (o.down)
        DG_LOGE(TAG, "相机断流(5s 无新帧),置不可用;硬件死机须断电重启恢复");
    else if (o.recovered)
        DG_LOGI(TAG, "相机流恢复");
    return TASK_OK;
}

bool capture_camera_ready(void)
{
    return s_ready_published;
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
