/*
 * liveness_service.c — 动作活体状态机(B8;PROJECT_PLAN §4.4)
 *
 * 状态流:IDLE(无人脸)→ ARMING(采基线)→ CHALLENGE(逐动作判定)
 *   → PASSED(全过,在场内粘滞)→ 人脸离场回 IDLE。
 * 线程模型:on_face 由视觉后端线程逐帧调,pass() 由其识别节拍调,事件
 * 订阅回调在总线线程——全部状态经 s_mtx 串行化;事件发布走总线(非阻塞)。
 *
 * 几何判定(全部同轴比例,与帧宽高比无关;5 点 = 左眼/右眼/鼻/左嘴/右嘴):
 *   yaw  d = (鼻.x − 双眼中点.x) / 眼距         → 摇头:偏移超阈后翻到对侧
 *   nod  r = (鼻.y − 眼中点.y) / (嘴中点.y − 眼中点.y),d = r − 基线 r0
 *       → 点头:d 超阈(低头变大使鼻靠向嘴)后回到基线附近
 *   两轴解耦:抬头/点头不影响 yaw(x 轴),转头/歪头对 nod 的 y 比例影响
 *   是 cos 级压缩,阈值留有余量。
 *
 * 降级:on_face 点数 != 5(ROCKIVA 106 点布局未映射)持续 >2s → DEGRADED,
 * pass() 放行并告警一次——与单帧反欺骗「模型缺失自动放行」同一取舍:
 * 布局未知的点集上硬套 5 点几何 = 把门禁变成「永远不开门」。
 */
#include "liveness_service.h"
#include "dg_log.h"
#include "timeutil.h"
#include "cfg.h"
#include "events.h"
#include "event_bus.h"
#include "vision_service.h"              /* 仅取 DG_VMODE_* 枚举(头文件常量,
                                            不链 dg_vision,无循环依赖) */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[LIVENESS]";

/* ---- 判定参数(代码常量,板上标定后再考虑进配置;单位见各注释) ---- */
#define LV_ARM_FRAMES     8      /* 基线采样帧数(30fps ≈ 0.27s) */
#define LV_BASE_RING      4      /* 基线取末 4 帧均值 */
#define LV_HOLD_FRAMES    4      /* 判定沿需连续帧数(滤单帧抖动) */
#define LV_EXC_YAW        0.28f  /* 摇头起幅:|Δyaw| ≥ 0.28 眼距(≈19° 转头) */
#define LV_EXC_YAW_CROSS  0.20f  /* 摇头往返:翻到对侧 ≥ 0.20 眼距 */
#define LV_EXC_NOD        0.10f  /* 点头起幅:|Δr| ≥ 0.10(≈10° 低头) */
#define LV_RET_NOD        0.04f  /* 点头回位:回到基线 ±0.04 */
#define LV_PASS_STALE_MS  2000   /* PASSED 后无回灌超过该时长视为已离场 */
#define LV_N5_DEGRADE_MS  2000   /* 非 5 点回灌持续超过该时长 → 降级放行 */
#define LV_DEFAULT_MIN    2      /* cfg 缺失/越界时的内置兜底 */
#define LV_DEFAULT_MAX    3
#define LV_DEFAULT_TO_MS  5000

typedef enum {
    ST_IDLE = 0,                         /* 无人脸 */
    ST_ARMING,                           /* 采基线 */
    ST_CHALLENGE,                        /* 逐动作判定 */
    ST_PASSED,                           /* 序列全过 */
    ST_DEGRADED,                         /* 非 5 点后端,放行 */
} lv_state_t;

typedef struct {
    /* 基线(ring 缓冲末 N 帧均值) */
    float ring_yaw[LV_BASE_RING];
    float ring_nod[LV_BASE_RING];
    int   ring_n;
    /* 当前动作判定 */
    int   stage;                         /* 0=等起幅 1=等回位/对侧 */
    int   hold;                          /* 当前条件连续帧数 */
    int   dir;                           /* 起幅方向(+1/-1) */
    /* 序列 */
    int   seq[3];
    int   seq_len;
    int   step;                          /* 当前步下标(0 起) */
    int64_t step_start_ms;               /* 本步开始时刻(单调) */
    int64_t non5_since_ms;               /* 非 5 点回灌开始时刻(0=无) */
    bool  warned_degrade;                /* 降级告警只打一次 */
    bool  chip_shown;                    /* 提示条在屏(UI 需要成对 HIDE) */
} lv_run_t;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static lv_state_t s_state = ST_IDLE;
static lv_run_t s_run;
static float s_base_yaw, s_base_nod;     /* CHALLENGE 期冻结的基线 */
static bool  s_active = true;            /* 模式门:DETECT_1N/VERIFY_11 才出题;
                                            开机到首条 SET_MODE 之间按匹配态
                                            处理(开机默认即 DETECT_1N) */
/* 记账(原 B7 遗留接口,联调观测用) */
static struct {
    bool    has;
    int64_t ms;
    uint32_t n;
    int32_t quality;
} s_last;

static bool warned_stub;                 /* 旧桩语义的告警位(B8 前放行提示) */

/* ---- cfg 兜底读取(键缺失/越界一律回内置默认,防 0 值把限时变 0) ---- */

static int cfg_actions_min(void)
{
    int v = cfg_get()->liveness_actions_min;
    return (v >= 1 && v <= 3) ? v : LV_DEFAULT_MIN;
}

static int cfg_actions_max(void)
{
    int v = cfg_get()->liveness_actions_max;
    if (v < 1 || v > 3)
        v = LV_DEFAULT_MAX;
    if (v < cfg_actions_min())
        v = cfg_actions_min();
    return v;
}

static int cfg_timeout_ms(void)
{
    int v = cfg_get()->liveness_timeout_ms;
    return (v >= 500 && v <= 10000) ? v : LV_DEFAULT_TO_MS;
}

/* ---- 几何量(5 点,万分比;输入已在调用方校验 n==5) ---- */

static bool frame_metrics(const dg_face_pt_t *p, float *yaw, float *nod)
{
    const float eye_mx = (p[0].x + p[1].x) * 0.5f;
    const float eye_my = (p[0].y + p[1].y) * 0.5f;
    const float ipd_x = (float)p[1].x - (float)p[0].x;
    const float mouth_my = (p[3].y + p[4].y) * 0.5f;
    const float eye2mouth = mouth_my - eye_my;
    if (ipd_x < 100 || eye2mouth < 100)
        return false;                    /* 点集退化(侧脸/遮挡),本帧不可判 */
    *yaw = ((float)p[2].x - eye_mx) / ipd_x;
    *nod = ((float)p[2].y - eye_my) / eye2mouth;
    return true;
}

static void pub_prompt(int8_t action, int8_t step, int8_t total,
                       dg_live_phase_t phase)
{
    ev_liveness_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.action = action;
    ev.step = step;
    ev.total = total;
    ev.phase = (int8_t)phase;
    EVENT_BUS_PUBLISH(EV_LIVENESS_PROMPT, &ev);
    if (phase == DG_LIVE_PHASE_STEP)
        s_run.chip_shown = true;
    else
        s_run.chip_shown = false;
}

/* 重置到 IDLE(离场/失活);若提示在屏则补一条 HIDE 成对收起 */
static void reset_locked(const char *why)
{
    if (s_state == ST_IDLE && !s_run.chip_shown) {
        s_run.non5_since_ms = 0;
        return;
    }
    if (s_run.chip_shown)
        pub_prompt(0, 0, 0, DG_LIVE_PHASE_HIDE);
    s_state = ST_IDLE;
    s_run.stage = 0;
    s_run.hold = 0;
    s_run.ring_n = 0;
    s_run.non5_since_ms = 0;
    DG_LOGD(TAG, "复位(%s)", why);
}

/* 出新序列:长度与动作随机(相邻不重复);CHALLENGE 从第 0 步开始 */
static void deal_sequence(int64_t now)
{
    const int lo = cfg_actions_min();
    const int hi = cfg_actions_max();
    s_run.seq_len = lo + rand() % (hi - lo + 1);
    for (int i = 0; i < s_run.seq_len; i++) {
        int act = (rand() % 2) ? DG_LIVE_ACTION_NOD : DG_LIVE_ACTION_SHAKE;
        if (i > 0 && act == s_run.seq[i - 1])
            act = (act == DG_LIVE_ACTION_NOD) ? DG_LIVE_ACTION_SHAKE
                                              : DG_LIVE_ACTION_NOD;
        s_run.seq[i] = act;
    }
    s_run.step = 0;
    s_run.stage = 0;
    s_run.hold = 0;
    s_run.dir = 0;
    s_run.step_start_ms = now;
    pub_prompt((int8_t)s_run.seq[0], 1, (int8_t)s_run.seq_len,
               DG_LIVE_PHASE_STEP);
}

/* 单步判定推进;返回 true = 本步完成 */
static bool step_detect(float dyaw, float dnod)
{
    const int act = s_run.seq[s_run.step];
    const float exc = (act == DG_LIVE_ACTION_SHAKE) ? LV_EXC_YAW : LV_EXC_NOD;
    const float d = (act == DG_LIVE_ACTION_SHAKE) ? dyaw : dnod;

    if (s_run.stage == 0) {
        /* 起幅:|d| 超阈且方向一致持续 HOLD 帧(记录方向) */
        if (d > exc) {
            s_run.hold = (s_run.dir > 0) ? s_run.hold + 1 : 1;
            s_run.dir = 1;
        } else if (d < -exc) {
            s_run.hold = (s_run.dir < 0) ? s_run.hold + 1 : 1;
            s_run.dir = -1;
        } else {
            s_run.hold = 0;
        }
        if (s_run.hold >= LV_HOLD_FRAMES) {
            s_run.stage = 1;
            s_run.hold = 0;
        }
        return false;
    }

    /* 回位:点头 = 回到基线附近;摇头 = 翻到对侧(同侧回 0 不算往返) */
    const bool ret = (act == DG_LIVE_ACTION_SHAKE)
                         ? (d * (float)s_run.dir < -LV_EXC_YAW_CROSS)
                         : (d * (float)s_run.dir < LV_RET_NOD);
    s_run.hold = ret ? s_run.hold + 1 : 0;
    return s_run.hold >= LV_HOLD_FRAMES;
}

/* 基线采样:ring 末 N 帧均值;采满即冻结并出题 */
static void arm_frame(int64_t now, float yaw, float nod)
{
    s_run.ring_yaw[s_run.ring_n % LV_BASE_RING] = yaw;
    s_run.ring_nod[s_run.ring_n % LV_BASE_RING] = nod;
    s_run.ring_n++;
    if (s_run.ring_n < LV_ARM_FRAMES)
        return;
    for (int i = 0; i < LV_BASE_RING; i++) {
        s_base_yaw += s_run.ring_yaw[i];
        s_base_nod += s_run.ring_nod[i];
    }
    s_base_yaw /= LV_BASE_RING;
    s_base_nod /= LV_BASE_RING;
    s_state = ST_CHALLENGE;
    deal_sequence(now);
    DG_LOGI(TAG, "活体出题 %d 步", s_run.seq_len);
}

static void handle_frame(int64_t now, const dg_face_pt_t *pts, uint32_t n)
{
    /* 非 5 点:记开始时刻,持续超限降级;5 点恢复不解除(离场才复位) */
    if (n != DG_LIVE_KPS) {
        if (s_state == ST_IDLE || s_run.non5_since_ms == 0)
            s_run.non5_since_ms = now;
        if (s_state != ST_DEGRADED &&
            now - s_run.non5_since_ms > LV_N5_DEGRADE_MS) {
            s_state = ST_DEGRADED;
            if (!s_run.warned_degrade) {
                s_run.warned_degrade = true;
                DG_LOGW(TAG, "关键点数 %u ≠ 5,动作活体降级放行(后端点集未映射)",
                        n);
            }
            if (s_run.chip_shown)
                pub_prompt(0, 0, 0, DG_LIVE_PHASE_HIDE);
        }
        return;
    }

    float yaw, nod;
    if (!frame_metrics(pts, &yaw, &nod))
        return;                          /* 退化帧:不进判定也不清 hold */

    switch (s_state) {
    case ST_IDLE:
        memset(&s_run, 0, sizeof(s_run));
        s_state = ST_ARMING;
        arm_frame(now, yaw, nod);
        return;
    case ST_ARMING:
        arm_frame(now, yaw, nod);
        return;
    case ST_CHALLENGE: {
        if (now - s_run.step_start_ms > cfg_timeout_ms()) {
            DG_LOGI(TAG, "第 %d/%d 步超时,重出题", s_run.step + 1,
                    s_run.seq_len);
            s_state = ST_ARMING;         /* 重新采基线再出题(人可能挪了) */
            s_run.ring_n = 0;
            return;
        }
        if (step_detect(yaw - s_base_yaw, nod - s_base_nod)) {
            s_run.step++;
            if (s_run.step >= s_run.seq_len) {
                s_state = ST_PASSED;
                pub_prompt(0, 0, 0, DG_LIVE_PHASE_PASSED);
                DG_LOGI(TAG, "动作活体通过");
            } else {
                s_run.stage = 0;
                s_run.hold = 0;
                s_run.dir = 0;
                s_run.step_start_ms = now;
                pub_prompt((int8_t)s_run.seq[s_run.step], (int8_t)(s_run.step + 1),
                           (int8_t)s_run.seq_len, DG_LIVE_PHASE_STEP);
            }
        }
        return;
    }
    case ST_PASSED:
    case ST_DEGRADED:
    default:
        return;                          /* 粘滞:在场内保持,无需逐帧处理 */
    }
}

/* ---- 事件订阅(总线线程) ---- */

static int on_face_lost_evt(const event_t *e, void *ud)
{
    (void)e;
    (void)ud;
    pthread_mutex_lock(&s_mtx);
    reset_locked("人脸离场");
    pthread_mutex_unlock(&s_mtx);
    return 0;
}

static int on_set_mode_evt(const event_t *e, void *ud)
{
    (void)ud;
    const ev_vision_mode_t *m = (const ev_vision_mode_t *)e->data;
    const bool active = (m->mode == DG_VMODE_DETECT_1N ||
                         m->mode == DG_VMODE_VERIFY_11);
    pthread_mutex_lock(&s_mtx);
    s_active = active;
    if (!active)
        reset_locked("模式失活");
    pthread_mutex_unlock(&s_mtx);
    return 0;
}

/* ---- 公开接口 ---- */

int liveness_service_start(void)
{
    srand((unsigned)(time(NULL) ^ (uintptr_t)&s_state));
    event_bus_subscribe(EV_VISION_FACE_LOST, on_face_lost_evt, NULL);
    event_bus_subscribe(EV_VISION_SET_MODE, on_set_mode_evt, NULL);
    DG_LOGI(TAG, "活体服务就绪(动作指令式;恒启用)");
    return DG_OK;
}

int liveness_service_on_face(const dg_face_pt_t *pts, uint32_t n, int32_t quality)
{
    if (n > DG_LANDMARK_MAX)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    s_last.has = true;
    s_last.ms = now_mono_ms();
    s_last.n = pts ? n : 0;
    s_last.quality = quality;
    if (s_active && pts && n > 0)
        handle_frame(now_mono_ms(), pts, n);
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

bool liveness_service_pass(void)
{
    bool pass;
    pthread_mutex_lock(&s_mtx);
    if (s_state == ST_PASSED &&
        s_last.has && now_mono_ms() - s_last.ms > LV_PASS_STALE_MS) {
        /* 双保险:FACE_LOST 订阅是主复位路径;若事件丢失(总线上限/时序),
         * 陈旧通过在这里兜住,防下一个人(或照片)继承上一场的通过 */
        reset_locked("通过已陈旧");
    }
    pass = (s_state == ST_PASSED) || (s_state == ST_DEGRADED);
    pthread_mutex_unlock(&s_mtx);
    return pass;
}

int64_t liveness_service_last_face_age_ms(void)
{
    int64_t ms;
    bool has;
    pthread_mutex_lock(&s_mtx);
    has = s_last.has;
    ms = s_last.ms;
    pthread_mutex_unlock(&s_mtx);
    return has ? now_mono_ms() - ms : -1;
}
