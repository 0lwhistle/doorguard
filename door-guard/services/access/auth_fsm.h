/*
 * auth_fsm.h — 主页验证状态机(spec-auth-business 唯一权威;纯 C,可脱离 UI 驱动)
 *
 * 设计:事件注入(注入式状态机)——UI/服务层把摄像头、识别、触摸、定时器
 * 全部翻译成 fsm_event_t 喂进来;FSM 内部不碰 LVGL、不碰 DB,只做决策并
 * 通过 on_action 回调输出动作(渲染/开门/写日志/定时器控制)由外部执行。
 * 这样 §5 全部边界都可以用事件序列在 ctest 里逐条驱动。
 *
 * 状态:ST_NORMAL(1:N 默认)→ ST_ADMIN_AUTH(管理员)→ ST_MENU →
 *      ST_VERIFY(v_input_uid/v_pick_method/v_face_1v1/v_finger/v_pwd/v_ic)→
 *      ST_RESULT(3s 自动回)→ ST_NORMAL;ST_STANDBY 独立(唤醒回 NORMAL)。
 *
 * 定时器:FSM 只发 SET_TIMER/CANCEL_TIMER 动作,外部真实定时;事件回填
 * {timer_id, seq};FSM 维护 timer_seq(每次重置 +1),旧定时器事件按 seq
 * 丢弃 —— spec-auth §5"timer_seq 保证旧定时器失效"。
 */
#ifndef DG_AUTH_FSM_H
#define DG_AUTH_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "proto/events.h"
#include "proto/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 状态 ---- */

typedef enum {
    ST_NORMAL = 0,
    ST_ADMIN_AUTH,
    ST_MENU,
    ST_VERIFY,
    ST_RESULT,
    ST_STANDBY,
} dg_fsm_state_t;

/* 验证子步(ST_VERIFY 内) */
typedef enum {
    V_NONE = 0,
    V_INPUT_UID,
    V_PICK_METHOD,
    V_FACE_1V1,
    V_FINGER,
    V_PWD,
    V_IC,
} dg_fsm_step_t;

/* ---- 事件(外部注入) ---- */

typedef enum {
    FSM_EV_MENU_BTN = 0,        /**< 主页点"菜单" */
    FSM_EV_VERIFY_BTN,          /**< 主页点"验证" */
    FSM_EV_BACK,                /**< 返回/取消(菜单返回、验证流程取消) */
    FSM_EV_FACE_DETECTED,       /**< 人脸出现(data: face box) */
    FSM_EV_FACE_LOST,
    FSM_EV_MATCH_1N,            /**< 1:N 结果(data: ev_match_t) */
    FSM_EV_VERIFY_11,           /**< 1:1 结果(data: ev_match_t) */
    FSM_EV_UID_SUBMIT,          /**< 输入弹窗确认(data: uid 字符串) */
    FSM_EV_UID_RESOLVED,        /**< ID 查询回执(data: uid_resolved_t) */
    FSM_EV_METHOD_PICK,         /**< 选择方式(data: method) */
    FSM_EV_VERIFY_RESULT,       /**< 方式验证结果(data: verify_result_t) */
    FSM_EV_ADMIN_COUNT,         /**< 管理员人数回执(data: admin_count;服务层查库后喂) */
    FSM_EV_TIMER,               /**< 定时器到期(data: timer_evt_t) */
    FSM_EV_TOUCH,               /**< 任意触摸(data: now_ms) */
    FSM_EV_TICK,                /**< 1s 心跳(data: now_ms,用于待机/锁定计时) */
} fsm_event_t;

typedef struct {
    int32_t x, y, w, h;
} fsm_face_box_t;

typedef struct {
    bool found;                    /**< ID 不存在 = false(reason=5) */
    bool locked;                   /**< 密码连错锁定中(reason=锁定) */
    char user_id[DG_UID_LEN];
    char user_name[DG_NAME_LEN];
    int32_t role;                  /**< dg_role_t */
    uint32_t auth_flags;           /**< DG_AUTH_* */
} fsm_uid_resolved_t;

typedef struct {
    int32_t method;                /**< dg_auth_method_t */
    bool ok;
    bool locked;                   /**< 密码方式:连错锁定直接拒绝 */
    char user_id[DG_UID_LEN];
    char user_name[DG_NAME_LEN];
    int32_t reason;                /**< 失败原因(dg_auth_reason_t;ok 时 0) */
    int64_t now_ms;                /**< 密码连错锁定计时用 */
} fsm_verify_result_t;

typedef struct {
    int32_t timer_id;              /**< 见 fsm_timer_t */
    uint32_t seq;                  /**< 事件携带的 seq:与 FSM 内部不符即丢弃 */
} fsm_timer_evt_t;

typedef union {
    fsm_face_box_t box;
    ev_match_t match;
    char uid[DG_UID_LEN];
    fsm_uid_resolved_t uid_res;
    int32_t method;
    int32_t admin_count;           /**< FSM_EV_ADMIN_COUNT:管理员人数(<0 = 未知) */
    fsm_verify_result_t result;
    fsm_timer_evt_t timer;
    int64_t now_ms;
} fsm_event_data_t;

/* ---- 定时器用途(FSM 声明,外部实现) ---- */

typedef enum {
    FSM_TMR_MATCH_WINDOW = 0,   /**< 人脸出现 1.5s 未命中 */
    FSM_TMR_STEP_5S,            /**< 验证子步 5s 超时 */
    FSM_TMR_RESULT_3S,          /**< 结果展示 3s */
    FSM_TMR_ADMIN_5S,           /**< 管理员模式 5s 无人脸 */
    FSM_TMR_COUNT,
} fsm_timer_t;

/* ---- 动作(FSM 输出,外部执行) ---- */

typedef enum {
    FSM_ACT_NONE = 0,
    FSM_ACT_GOTO_PAGE,          /**< data: page 名("menu"/"home"/"standby") */
    FSM_ACT_FACEBOX,            /**< data: box + 状态颜色(w=0 = 只改颜色) */
    FSM_ACT_FACEBOX_HIDE,
    FSM_ACT_POPUP_SUCCESS,      /**< data: user_name(misc.hint "id|name") */
    FSM_ACT_POPUP_FAIL,         /**< data: reason(dg_auth_reason_t) + not_admin */
    FSM_ACT_ASK_UID,            /**< 请弹 ID 输入框(验证流程第一步) */
    FSM_ACT_ASK_PWD,            /**< data: misc.uid;请弹密码输入框 */
    FSM_ACT_SHOW_METHODS,       /**< data: auth_flags(按开启方式显示按钮) */
    FSM_ACT_SET_TIMER,          /**< data: timer_req_t{timer_id, ms} */
    FSM_ACT_CANCEL_TIMERS,      /**< 取消全部未到期定时器 */
    FSM_ACT_OPEN_DOOR,          /**< data: door_open_ms */
    FSM_ACT_WRITE_LOG,          /**< data: log_rec_t(每次验证动作唯一出口) */
    FSM_ACT_HINT_TEXT,          /**< data: hint(-1 管理员认证/-2 无管理员/方式) */
    FSM_ACT_HINT_CLEAR,
} fsm_action_t;

typedef struct {
    fsm_timer_t timer_id;
    uint32_t ms;
    uint32_t seq;                  /**< 本次设置的 seq(回调事件须带回) */
} fsm_timer_req_t;

typedef struct {
    fsm_face_box_t box;
    int32_t state;                 /**< dg_box_state_t(黄/绿/红) */
} fsm_facebox_act_t;

typedef struct {
    int32_t reason;                /**< dg_auth_reason_t(UI 映射文案) */
    bool popup_fail;               /**< true=失败弹窗;false=仅提示 */
    bool not_admin;                /**< 管理员入口专用:文案显示「非管理员」 */
} fsm_fail_act_t;

typedef struct {
    char user_id[DG_UID_LEN];      /**< 陌生人空串 */
    char user_name[DG_NAME_LEN];
    int32_t method;
    int32_t result;                /**< dg_auth_result_t */
    int32_t reason;                /**< dg_auth_reason_t */
    int64_t ts;                    /**< unix 秒 */
} fsm_log_act_t;

typedef struct {
    int32_t method;                /**< 指定方式时非 0;0=按 auth_flags 列出 */
    uint32_t auth_flags;
    char uid[DG_UID_LEN];          /**< FSM_ACT_ASK_PWD:密码验证的目标用户 */
    char hint[64];
} fsm_misc_act_t;

typedef union {
    const char *page;
    fsm_facebox_act_t facebox;
    fsm_fail_act_t fail;
    fsm_timer_req_t timer;
    fsm_log_act_t log;
    fsm_misc_act_t misc;
    uint32_t door_open_ms;
} fsm_action_data_t;

/** 动作回调:FSM 决策输出(渲染/门控/日志/定时器均由外部执行) */
typedef void (*fsm_action_fn)(fsm_action_t act, const fsm_action_data_t *data, void *ud);

/* ---- 上下文 ---- */

typedef struct {
    dg_fsm_state_t state;
    dg_fsm_step_t step;
    bool match_enabled;            /**< 1:N 检索开关:ST_NORMAL 且无弹窗时开 */
    bool popup_active;             /**< 弹窗在显示(spec 标志位) */

    uint32_t timer_seq;            /**< 每次设置新定时器 +1(旧定时器失效) */
    uint32_t timer_active[FSM_TMR_COUNT];

    char cur_uid[DG_UID_LEN];      /**< ST_VERIFY 流程中的用户 */
    char cur_name[DG_NAME_LEN];
    int32_t cur_role;
    uint32_t cur_auth_flags;
    int32_t step_method;           /**< 当前子步的验证方式(超时日志用) */
    bool verify_from_admin;        /**< 本流程由管理员入口发起(通过后校验 role) */

    /** 管理员人数(FSM_EV_ADMIN_COUNT 回填;<0 = 未知)。=0 时菜单免认证进入:
     *  新机/管理员被删光的情形下,要求管理员认证会让菜单永远进不去(鸡生蛋) */
    int32_t admin_count;

    /* 密码连错锁定(内存即可,掉电可丢;spec-auth §5) */
    char lock_uid[DG_UID_LEN];
    int32_t lock_fail_cnt;
    int64_t lock_until_ms;

    /* 待机(空闲秒计数;触摸/人脸清零) */
    int32_t idle_s;

    /* 初始化参数 */
    int32_t door_open_ms;
    int32_t standby_timeout_s;
    int32_t pwd_fail_lock_n;
    int32_t pwd_fail_lock_s;

    fsm_action_fn on_action;
    void *ud;
} auth_fsm_t;

/** 初始化;cfg 参数来自 dg_cfg_t(业务参数不进 FSM 魔数) */
void auth_fsm_init(auth_fsm_t *fsm, int32_t door_open_ms, int32_t standby_timeout_s,
                   int32_t pwd_fail_lock_n, int32_t pwd_fail_lock_s,
                   fsm_action_fn on_action, void *ud);

/** 事件入口(线程约定:与 UI 同线程调用) */
void auth_fsm_handle(auth_fsm_t *fsm, fsm_event_t ev, const fsm_event_data_t *data);

/** 密码方式入口前预检:该用户连错锁定中返回 true(锁定中直接提示,不计次数) */
bool auth_fsm_pwd_locked(const auth_fsm_t *fsm, const char *user_id, int64_t now_ms);

const char *auth_fsm_state_name(dg_fsm_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* DG_AUTH_FSM_H */
