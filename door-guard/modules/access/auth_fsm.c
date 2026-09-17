/*
 * auth_fsm.c — 验证状态机实现(规格:spec-auth-business.md;逐条边界见 tests/test_auth_fsm.c)
 *
 * 关键不变量:
 * - match_enabled 仅在 ST_NORMAL 且无弹窗时为真(spec §2/§4)
 * - 检测画框恒开:弹窗期间检测继续、匹配停止(spec §1 标志位)
 * - 每个验证动作(成功/失败/任何方式)都经 FSM_ACT_WRITE_LOG 落一条
 * - timer_seq 机制:重设定时器 seq+1,旧事件按 seq 丢弃
 * - 密码连错锁 N 次锁 S 秒,计数内存态(掉电可丢,spec §5)
 * - FSM 不碰 LVGL/DB:提示输出语义枚举,文案由 UI 映射(_() 在 UI 侧)
 */
#include "auth_fsm.h"
#include "dg_log.h"

#include <stdio.h>

#include <string.h>

static const char *TAG = "[FSM]";

static void emit(auth_fsm_t *fsm, fsm_action_t act, const fsm_action_data_t *d)
{
    if (fsm->on_action)
        fsm->on_action(act, d, fsm->ud);
}

static void emit_none(auth_fsm_t *fsm, fsm_action_t act)
{
    emit(fsm, act, NULL);
}

static void set_timer(auth_fsm_t *fsm, fsm_timer_t id, uint32_t ms)
{
    fsm->timer_seq++;
    fsm->timer_active[id] = fsm->timer_seq;
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = id;
    d.timer.ms = ms;
    d.timer.seq = fsm->timer_seq;
    emit(fsm, FSM_ACT_SET_TIMER, &d);
}

static void cancel_timers(auth_fsm_t *fsm)
{
    fsm->timer_seq++;
    for (int i = 0; i < FSM_TMR_COUNT; i++)
        fsm->timer_active[i] = 0;
    emit_none(fsm, FSM_ACT_CANCEL_TIMERS);
}

/* 结果弹窗载荷:ID+姓名经 misc.hint 传递(格式 "user_id|user_name",UI 拆分) */
static void popup_result(auth_fsm_t *fsm, bool ok, const char *id, const char *name)
{
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.fail.reason = ok ? DG_REASON_OK : DG_REASON_STRANGER;
    d.fail.popup_fail = ok;
    snprintf(d.misc.hint, sizeof(d.misc.hint), "%s|%s", id ? id : "",
             name ? name : "");
    emit(fsm, ok ? FSM_ACT_POPUP_SUCCESS : FSM_ACT_POPUP_FAIL, &d);
}

static void popup_fail_reason(auth_fsm_t *fsm, int32_t reason)
{
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.fail.reason = reason;
    d.fail.popup_fail = true;
    emit(fsm, FSM_ACT_POPUP_FAIL, &d);
}

static void write_log(auth_fsm_t *fsm, const char *uid, const char *name,
                      int32_t method, int32_t result, int32_t reason, int64_t now_ms)
{
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    if (uid)
        snprintf(d.log.user_id, sizeof(d.log.user_id), "%s", uid);
    if (name)
        snprintf(d.log.user_name, sizeof(d.log.user_name), "%s", name);
    d.log.method = method;
    d.log.result = result;
    d.log.reason = reason;
    d.log.ts = now_ms / 1000;           /* 0 = 由写入方以落库时刻补齐 */
    emit(fsm, FSM_ACT_WRITE_LOG, &d);
}

static void goto_page(auth_fsm_t *fsm, const char *page)
{
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.page = page;
    emit(fsm, FSM_ACT_GOTO_PAGE, &d);
}

static void set_match_enabled(auth_fsm_t *fsm, bool on)
{
    fsm->match_enabled = on;
}

/* 成功收尾:结果态 + 开门 + 日志(spec §2.3/§4.5) */
static void succeed(auth_fsm_t *fsm, int32_t method, int64_t now_ms)
{
    cancel_timers(fsm);
    fsm->state = ST_RESULT;
    fsm->popup_active = true;
    set_timer(fsm, FSM_TMR_RESULT_3S, 3000);
    popup_result(fsm, true, fsm->cur_uid, fsm->cur_name);
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.door_open_ms = (uint32_t)fsm->door_open_ms;
    emit(fsm, FSM_ACT_OPEN_DOOR, &d);
    write_log(fsm, fsm->cur_uid, fsm->cur_name, method, DG_RESULT_PASS,
              DG_REASON_OK, now_ms);
    set_match_enabled(fsm, false);
}

static void fail_and_back(auth_fsm_t *fsm, int32_t method, int32_t reason,
                          int64_t now_ms)
{
    cancel_timers(fsm);
    fsm->state = ST_RESULT;
    fsm->popup_active = true;
    set_timer(fsm, FSM_TMR_RESULT_3S, 3000);
    popup_result(fsm, false, fsm->cur_uid[0] ? fsm->cur_uid : NULL,
                 fsm->cur_name[0] ? fsm->cur_name : NULL);
    write_log(fsm, fsm->cur_uid[0] ? fsm->cur_uid : NULL,
              fsm->cur_name[0] ? fsm->cur_name : NULL, method,
              DG_RESULT_REJECT, reason, now_ms);
    set_match_enabled(fsm, false);
}

static void back_to_normal(auth_fsm_t *fsm)
{
    fsm->state = ST_NORMAL;
    fsm->popup_active = false;
    set_match_enabled(fsm, true);
    goto_page(fsm, "home");
}

static void enter_standby(auth_fsm_t *fsm)
{
    fsm->state = ST_STANDBY;
    set_match_enabled(fsm, false);
    cancel_timers(fsm);
    goto_page(fsm, "standby");
    DG_LOGI(TAG, "进入待机");
}

static void wake_up(auth_fsm_t *fsm)
{
    fsm->idle_s = 0;
    if (fsm->state == ST_STANDBY) {
        fsm->state = ST_NORMAL;
        set_match_enabled(fsm, true);
        goto_page(fsm, "home");
        DG_LOGI(TAG, "待机唤醒回普通模式");
    }
}

/* ---- 1:N 命中(普通态/管理员态共用入口) ---- */

static void on_match_1n(auth_fsm_t *fsm, const ev_match_t *m, int64_t now_ms)
{
    if (fsm->state == ST_ADMIN_AUTH) {
        if (!m->matched)
            return;                     /* 未命中不打断管理员等待 */
        snprintf(fsm->cur_uid, sizeof(fsm->cur_uid), "%s", m->user_id);
        snprintf(fsm->cur_name, sizeof(fsm->cur_name), "%s", m->user_name);
        if (m->role == DG_ROLE_ADMIN) {
            cancel_timers(fsm);
            fsm->state = ST_MENU;
            goto_page(fsm, "menu");
            DG_LOGI(TAG, "管理员通过进菜单");
        } else {
            /* 非管理员:红弹窗,停留本模式继续尝试(spec §3) */
            popup_fail_reason(fsm, DG_REASON_STRANGER);
            write_log(fsm, m->user_id, m->user_name, DG_METHOD_FACE_1N,
                      DG_RESULT_REJECT, DG_REASON_STRANGER, now_ms);
            set_timer(fsm, FSM_TMR_ADMIN_5S, 5000);
        }
        return;
    }

    /* ST_NORMAL:match_enabled 且命中才判定;黑名单任何路径都失败(spec §2.4) */
    if (fsm->state != ST_NORMAL || !fsm->match_enabled || !m->matched)
        return;

    snprintf(fsm->cur_uid, sizeof(fsm->cur_uid), "%s", m->user_id);
    snprintf(fsm->cur_name, sizeof(fsm->cur_name), "%s", m->user_name);

    /* 命中重绘只改颜色:框位置沿用 FACE_DETECTED 的最近一次(UI 端保持) */
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.facebox.box.w = 0;                /* w=0 = 沿用现有位置 */
    d.facebox.state = (m->role == DG_ROLE_BLACKLIST) ? DG_BOX_FAILED : DG_BOX_MATCHED;
    emit(fsm, FSM_ACT_FACEBOX, &d);

    if (m->role == DG_ROLE_BLACKLIST)
        fail_and_back(fsm, DG_METHOD_FACE_1N, DG_REASON_BLACKLIST, now_ms);
    else
        succeed(fsm, DG_METHOD_FACE_1N, now_ms);
}

/* ST_VERIFY 子步推进 */
static void verify_start(auth_fsm_t *fsm)
{
    fsm->state = ST_VERIFY;
    fsm->step = V_INPUT_UID;
    fsm->cur_uid[0] = '\0';
    fsm->cur_name[0] = '\0';
    set_match_enabled(fsm, false);      /* 点验证即放弃 1:N(spec §4 前提语义) */
    emit_none(fsm, FSM_ACT_ASK_UID);
    set_timer(fsm, FSM_TMR_STEP_5S, 5000);
}

static void show_methods(auth_fsm_t *fsm)
{
    fsm->step = V_PICK_METHOD;
    fsm_action_data_t d;
    memset(&d, 0, sizeof(d));
    d.misc.auth_flags = fsm->cur_auth_flags;
    emit(fsm, FSM_ACT_SHOW_METHODS, &d);
    set_timer(fsm, FSM_TMR_STEP_5S, 5000);
}

void auth_fsm_handle(auth_fsm_t *fsm, fsm_event_t ev, const fsm_event_data_t *data)
{
    fsm_event_data_t empty;
    memset(&empty, 0, sizeof(empty));
    if (!data)
        data = &empty;

    switch (ev) {
    case FSM_EV_TOUCH:
        wake_up(fsm);
        return;

    case FSM_EV_TICK:
        fsm->idle_s++;
        /* 仅普通态进待机(管理员/验证/结果中途不进,防误伤流程) */
        if (fsm->state == ST_NORMAL && fsm->idle_s >= fsm->standby_timeout_s)
            enter_standby(fsm);
        return;

    case FSM_EV_FACE_DETECTED:
        wake_up(fsm);
        /* 检测画框与匹配独立(spec §1):除待机外框照常跟随 */
        if (fsm->state != ST_STANDBY) {
            fsm_action_data_t d;
            memset(&d, 0, sizeof(d));
            d.facebox.box = data->box;
            d.facebox.state = DG_BOX_DETECTED;
            emit(fsm, FSM_ACT_FACEBOX, &d);
        }
        /* 1.5s 判定窗只在普通态(每此出现重置) */
        if (fsm->state == ST_NORMAL)
            set_timer(fsm, FSM_TMR_MATCH_WINDOW, 1500);
        return;

    case FSM_EV_FACE_LOST:
        if (fsm->state == ST_NORMAL)
            emit_none(fsm, FSM_ACT_FACEBOX_HIDE);
        return;

    case FSM_EV_MATCH_1N:
        on_match_1n(fsm, &data->match, 0);
        return;

    case FSM_EV_VERIFY_11:
        /* 1:1 结果并入统一结果处理 */
        if (fsm->state == ST_VERIFY && fsm->step == V_FACE_1V1) {
            fsm_verify_result_t r;
            memset(&r, 0, sizeof(r));
            r.method = DG_METHOD_FACE_11;
            r.ok = data->match.matched && data->match.role != DG_ROLE_BLACKLIST;
            r.reason = r.ok ? DG_REASON_OK : DG_REASON_MISMATCH;
            snprintf(r.user_id, sizeof(r.user_id), "%s", data->match.user_id);
            snprintf(r.user_name, sizeof(r.user_name), "%s", data->match.user_name);
            fsm_event_data_t rd;
            memset(&rd, 0, sizeof(rd));
            rd.result = r;
            auth_fsm_handle(fsm, FSM_EV_VERIFY_RESULT, &rd);
        }
        return;

    case FSM_EV_TIMER: {
        const fsm_timer_evt_t *t = &data->timer;
        if (t->timer_id < 0 || t->timer_id >= FSM_TMR_COUNT)
            return;
        /* 旧定时器:seq 与内部登记不符,直接丢弃(spec §5 timer_seq 机制) */
        if (t->seq != fsm->timer_active[t->timer_id]) {
            DG_LOGD(TAG, "旧定时器丢弃 id=%d seq=%u", t->timer_id, t->seq);
            return;
        }
        fsm->timer_active[t->timer_id] = 0;

        if (t->timer_id == FSM_TMR_MATCH_WINDOW && fsm->state == ST_NORMAL) {
            /* 1.5s 未命中:红框 + 失败弹窗 reason=1 陌生人(spec §2.4) */
            fsm_action_data_t d;
            memset(&d, 0, sizeof(d));
            d.facebox.state = DG_BOX_FAILED;
            emit(fsm, FSM_ACT_FACEBOX, &d);
            fail_and_back(fsm, DG_METHOD_FACE_1N, DG_REASON_STRANGER, 0);
        } else if (t->timer_id == FSM_TMR_STEP_5S && fsm->state == ST_VERIFY) {
            /* 子步 5s 超时回普通(spec §4.4),日志 reason=7 */
            fail_and_back(fsm, DG_METHOD_PWD, DG_REASON_TIMEOUT, 0);
        } else if (t->timer_id == FSM_TMR_ADMIN_5S && fsm->state == ST_ADMIN_AUTH) {
            back_to_normal(fsm);            /* 5s 无脸无操作回普通(spec §3) */
        } else if (t->timer_id == FSM_TMR_RESULT_3S && fsm->state == ST_RESULT) {
            /* 结果展示约 3s 自动回普通,恢复检测与匹配(spec §2.5) */
            back_to_normal(fsm);
        }
        return;
    }

    case FSM_EV_MENU_BTN:
        if (fsm->state == ST_NORMAL) {
            fsm->state = ST_ADMIN_AUTH;
            set_match_enabled(fsm, false);
            fsm_action_data_t d;
            memset(&d, 0, sizeof(d));
            d.misc.method = -1;             /* 语义:提示"管理员认证"(UI 映射) */
            emit(fsm, FSM_ACT_HINT_TEXT, &d);
            set_timer(fsm, FSM_TMR_ADMIN_5S, 5000);
        }
        return;

    case FSM_EV_VERIFY_BTN:
        if (fsm->state == ST_NORMAL || fsm->state == ST_ADMIN_AUTH) {
            cancel_timers(fsm);
            verify_start(fsm);
        }
        return;

    case FSM_EV_BACK:
        if (fsm->state == ST_MENU)
            back_to_normal(fsm);
        return;

    case FSM_EV_UID_SUBMIT:
        /* 记录待解析 ID:FSM 不碰 DB,等 UID_RESOLVED 回执 */
        if (fsm->state != ST_VERIFY || fsm->step != V_INPUT_UID)
            return;
        snprintf(fsm->cur_uid, sizeof(fsm->cur_uid), "%s", data->uid);
        set_timer(fsm, FSM_TMR_STEP_5S, 5000);
        return;

    case FSM_EV_UID_RESOLVED: {
        if (fsm->state != ST_VERIFY || fsm->step != V_INPUT_UID)
            return;
        const fsm_uid_resolved_t *r = &data->uid_res;
        snprintf(fsm->cur_name, sizeof(fsm->cur_name), "%s", r->user_name);
        fsm->cur_role = r->role;
        fsm->cur_auth_flags = r->auth_flags;
        snprintf(fsm->cur_uid, sizeof(fsm->cur_uid), "%s", r->user_id);

        if (!r->found) {
            fail_and_back(fsm, DG_METHOD_PWD, DG_REASON_NO_USER, 0);
            return;
        }
        if (r->role == DG_ROLE_BLACKLIST) {
            fail_and_back(fsm, DG_METHOD_PWD, DG_REASON_BLACKLIST, 0);
            return;
        }
        if (r->auth_flags == 0) {
            fail_and_back(fsm, DG_METHOD_PWD, DG_REASON_AUTH_DISABLED, 0);
            return;
        }
        show_methods(fsm);                  /* 按开启方式显示按钮(spec §4.2) */
        return;
    }

    case FSM_EV_METHOD_PICK: {
        if (fsm->state != ST_VERIFY || fsm->step != V_PICK_METHOD)
            return;
        int32_t m = data->method;
        fsm->step = (m == DG_METHOD_FACE_11)  ? V_FACE_1V1
                    : (m == DG_METHOD_FINGER) ? V_FINGER
                    : (m == DG_METHOD_PWD)    ? V_PWD
                                              : V_IC;
        set_timer(fsm, FSM_TMR_STEP_5S, 5000);
        fsm_action_data_t d;
        memset(&d, 0, sizeof(d));
        d.misc.method = m;                  /* UI 映射提示("请按指纹"等) */
        emit(fsm, FSM_ACT_HINT_TEXT, &d);
        return;
    }

    case FSM_EV_VERIFY_RESULT: {
        if (fsm->state != ST_VERIFY)
            return;
        const fsm_verify_result_t *r = &data->result;
        int32_t method = r->method;

        if (r->ok) {
            succeed(fsm, method, r->now_ms);
            return;
        }

        /* 密码连错锁定(spec §5):同 ID 连错 N 次锁 S 秒 */
        if (method == DG_METHOD_PWD && !r->locked) {
            if (!strcmp(fsm->lock_uid, fsm->cur_uid))
                fsm->lock_fail_cnt++;
            else
                fsm->lock_fail_cnt = 1;
            snprintf(fsm->lock_uid, sizeof(fsm->lock_uid), "%s", fsm->cur_uid);
            if (fsm->lock_fail_cnt >= fsm->pwd_fail_lock_n) {
                fsm->lock_until_ms = r->now_ms + (int64_t)fsm->pwd_fail_lock_s * 1000;
                fsm->lock_fail_cnt = 0;
                /* 锁定中:失败弹窗(文案由 UI 依 locked 状态区分"锁定中") */
            }
        }
        fail_and_back(fsm, method, r->reason, r->now_ms);
        return;
    }

    default:
        return;
    }
}

/* 密码方式入口前的锁定预检:锁定中直接提示,不计失败次数(spec §5) */
bool auth_fsm_pwd_locked(const auth_fsm_t *fsm, const char *user_id, int64_t now_ms)
{
    return fsm->lock_until_ms > now_ms && !strcmp(fsm->lock_uid, user_id);
}

void auth_fsm_init(auth_fsm_t *fsm, int32_t door_open_ms, int32_t standby_timeout_s,
                   int32_t pwd_fail_lock_n, int32_t pwd_fail_lock_s,
                   fsm_action_fn on_action, void *ud)
{
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = ST_NORMAL;
    fsm->match_enabled = true;          /* 开机默认普通模式 1:N(spec §5) */
    fsm->door_open_ms = door_open_ms;
    fsm->standby_timeout_s = standby_timeout_s;
    fsm->pwd_fail_lock_n = pwd_fail_lock_n;
    fsm->pwd_fail_lock_s = pwd_fail_lock_s;
    fsm->on_action = on_action;
    fsm->ud = ud;
}

const char *auth_fsm_state_name(dg_fsm_state_t st)
{
    switch (st) {
    case ST_NORMAL:     return "NORMAL";
    case ST_ADMIN_AUTH: return "ADMIN_AUTH";
    case ST_MENU:       return "MENU";
    case ST_VERIFY:     return "VERIFY";
    case ST_RESULT:     return "RESULT";
    case ST_STANDBY:    return "STANDBY";
    default:            return "?";
    }
}
