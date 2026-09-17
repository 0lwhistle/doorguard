/*
 * test_auth_fsm.c — 验证状态机边界测试(Phase 6,spec-auth-business §5 逐条)
 *
 * 纯事件序列驱动(不依赖 LVGL/DB):断言 FSM 输出动作序列
 * (开门/写日志/弹窗/定时器/页面)。任务清单 10 组用例拆成 19 条。
 */
#include "auth_fsm.h"
#include "dg_test.h"

#include <string.h>

/* ---- 动作记录器 ---- */

#define REC_MAX 64
typedef struct {
    fsm_action_t act;
    fsm_action_data_t d;
} act_rec_t;

static act_rec_t s_recs[REC_MAX];
static int s_rec_cnt = 0;

static auth_fsm_t s_fsm;

static void rec_reset(void)
{
    s_rec_cnt = 0;
}

static void rec_on_action(fsm_action_t act, const fsm_action_data_t *d, void *ud)
{
    (void)ud;
    if (s_rec_cnt < REC_MAX) {
        s_recs[s_rec_cnt].act = act;
        if (d)
            s_recs[s_rec_cnt].d = *d;
        else
            memset(&s_recs[s_rec_cnt].d, 0, sizeof(fsm_action_data_t));
        s_rec_cnt++;
    }
}

static void fsm_reset(void)
{
    rec_reset();
    auth_fsm_init(&s_fsm, 3000 /*door*/, 30 /*standby*/, 5 /*lock_n*/, 60 /*lock_s*/,
                  rec_on_action, NULL);
}

static int count_act(fsm_action_t act)
{
    int n = 0;
    for (int i = 0; i < s_rec_cnt; i++)
        if (s_recs[i].act == act)
            n++;
    return n;
}

static const act_rec_t *last_act(fsm_action_t act)
{
    for (int i = s_rec_cnt - 1; i >= 0; i--)
        if (s_recs[i].act == act)
            return &s_recs[i];
    return NULL;
}

static ev_match_t mk_match(const char *uid, const char *name, int32_t role, bool matched)
{
    ev_match_t m;
    memset(&m, 0, sizeof(m));
    m.matched = matched;
    snprintf(m.user_id, sizeof(m.user_id), "%s", uid);
    snprintf(m.user_name, sizeof(m.user_name), "%s", name);
    m.role = role;
    m.score_permille = 950;
    return m;
}

static void handle_match(ev_match_t m)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.match = m;
    auth_fsm_handle(&s_fsm, FSM_EV_MATCH_1N, &d);
}

/* 通用:普通态出现人脸 */
static void face_detected(void)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.box.x = 100; d.box.y = 200; d.box.w = 300; d.box.h = 400;
    auth_fsm_handle(&s_fsm, FSM_EV_FACE_DETECTED, &d);
}

/* 通用:走完 UID 流程到方式选择 */
static void uid_flow(const char *uid, bool found, int32_t role, uint32_t flags)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    snprintf(d.uid, sizeof(d.uid), "%s", uid);
    auth_fsm_handle(&s_fsm, FSM_EV_UID_SUBMIT, &d);
    memset(&d, 0, sizeof(d));
    snprintf(d.uid_res.user_id, sizeof(d.uid_res.user_id), "%s", uid);
    snprintf(d.uid_res.user_name, sizeof(d.uid_res.user_name), "用户%s", uid);
    d.uid_res.found = found;
    d.uid_res.role = role;
    d.uid_res.auth_flags = flags;
    auth_fsm_handle(&s_fsm, FSM_EV_UID_RESOLVED, &d);
}

/* ================= 1 普通模式命中 ================= */

static void t01_normal_hit(void)
{
    printf("[F01] 普通模式命中→绿弹窗+开门+日志 result=0\n");
    fsm_reset();
    face_detected();
    DG_CHECK(s_fsm.state == ST_NORMAL);
    DG_CHECK(s_fsm.match_enabled);
    ev_match_t m = mk_match("10001", "张三", DG_ROLE_NORMAL, true);
    handle_match(m);

    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
    DG_CHECK(last_act(FSM_ACT_OPEN_DOOR)->d.door_open_ms == 3000);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log != NULL);
    DG_CHECK(log->d.log.result == DG_RESULT_PASS);
    DG_CHECK(log->d.log.reason == DG_REASON_OK);
    DG_CHECK(log->d.log.method == DG_METHOD_FACE_1N);
    DG_CHECK(strcmp(log->d.log.user_id, "10001") == 0);
    DG_CHECK(!s_fsm.match_enabled);     /* 结果期间匹配挂起 */
    /* 绿框动作存在 */
    const act_rec_t *fb = last_act(FSM_ACT_FACEBOX);
    DG_CHECK(fb && fb->d.facebox.state == DG_BOX_MATCHED);
}

/* ================= 2 1.5s 未命中 ================= */

static void t02_match_timeout(void)
{
    printf("[F02] 1.5s 未命中→红框+红弹窗 reason=1\n");
    fsm_reset();
    face_detected();
    /* 定时器请求发出:1.5s 窗口 */
    const act_rec_t *tr = last_act(FSM_ACT_SET_TIMER);
    DG_CHECK(tr && tr->d.timer.timer_id == FSM_TMR_MATCH_WINDOW);
    DG_CHECK(tr->d.timer.ms == 1500);
    uint32_t seq = tr->d.timer.seq;

    /* 窗口内无人脸命中 → 定时器到期 */
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = FSM_TMR_MATCH_WINDOW;
    d.timer.seq = seq;
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);

    DG_CHECK(s_fsm.state == ST_RESULT);
    const act_rec_t *fb = last_act(FSM_ACT_FACEBOX);
    DG_CHECK(fb && fb->d.facebox.state == DG_BOX_FAILED);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.result == DG_RESULT_REJECT);
    DG_CHECK(log->d.log.reason == DG_REASON_STRANGER);
    DG_CHECK(log->d.log.user_id[0] == '\0');    /* 陌生人 user_id NULL */
}

/* ================= 3 黑名单命中 ================= */

static void t03_blacklist_hit(void)
{
    printf("[F03] 黑名单 1:N 命中→红弹窗 reason=2\n");
    fsm_reset();
    face_detected();
    ev_match_t m = mk_match("99001", "老黑", DG_ROLE_BLACKLIST, true);
    handle_match(m);
    DG_CHECK(s_fsm.state == ST_RESULT);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_BLACKLIST);
    DG_CHECK(log->d.log.result == DG_RESULT_REJECT);
}

/* ================= 4 点验证后 match_enabled=0 ================= */

static void t04_verify_suspends_match(void)
{
    printf("[F04] 点验证后 match_enabled=0:1:N 命中被忽略\n");
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_VERIFY && !s_fsm.match_enabled);

    int logs_before = count_act(FSM_ACT_WRITE_LOG);
    ev_match_t m = mk_match("10001", "张三", DG_ROLE_NORMAL, true);
    handle_match(m);
    DG_CHECK(s_fsm.state == ST_VERIFY);             /* 状态不迁移 */
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == logs_before);  /* 不出结果 */
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
}

/* ================= 5 ID 不存在 / 黑名单 / auth_flags=0 ================= */

static void t05_uid_reject_paths(void)
{
    printf("[F05] ID 不存在/黑名单/auth_flags=0 → 红弹窗,不出现方式选择\n");

    fsm_reset();
    uid_flow("99999", false, DG_ROLE_NORMAL, DG_AUTH_ALL);
    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(count_act(FSM_ACT_SHOW_METHODS) == 0);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_NO_USER);

    fsm_reset();
    uid_flow("99001", true, DG_ROLE_BLACKLIST, DG_AUTH_ALL);
    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(count_act(FSM_ACT_SHOW_METHODS) == 0);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_BLACKLIST);

    fsm_reset();
    uid_flow("10003", true, DG_ROLE_NORMAL, 0);
    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(count_act(FSM_ACT_SHOW_METHODS) == 0);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_AUTH_DISABLED);
}

/* ================= 6 子步 5s 超时 + timer_seq 失效旧定时器 ================= */

static void t06_step_timeout_and_stale_timer(void)
{
    printf("[F06] 子步 5s 超时回普通;旧定时器事件被丢弃\n");
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    const act_rec_t *tr = last_act(FSM_ACT_SET_TIMER);
    DG_CHECK(tr && tr->d.timer.timer_id == FSM_TMR_STEP_5S);
    uint32_t stale_seq = tr->d.timer.seq;

    /* 输入 ID 触发新定时器(seq+1),旧 seq 的到期事件必须被丢弃 */
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.uid, sizeof(d.uid), "10001");
    auth_fsm_handle(&s_fsm, FSM_EV_UID_SUBMIT, &d);
    uint32_t fresh_seq = s_fsm.timer_active[FSM_TMR_STEP_5S];
    DG_CHECK(fresh_seq != stale_seq);

    d.timer.timer_id = FSM_TMR_STEP_5S;
    d.timer.seq = stale_seq;
    int recs = s_rec_cnt;
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
    DG_CHECK(s_fsm.state == ST_VERIFY);             /* 旧定时器不杀新状态 */
    DG_CHECK(s_rec_cnt == recs);                    /* 无动作输出 */

    /* 新 seq 到期:回普通 + 日志 reason=7 */
    d.timer.seq = fresh_seq;
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_TIMEOUT);
    /* 结果 3s 后回普通 */
    uint32_t rseq = s_fsm.timer_active[FSM_TMR_RESULT_3S];
    d.timer.timer_id = FSM_TMR_RESULT_3S;
    d.timer.seq = rseq;
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
    DG_CHECK(s_fsm.state == ST_NORMAL);
    DG_CHECK(s_fsm.match_enabled);
}

/* ================= 7 管理员模式 ================= */

static void t07_admin_flow(void)
{
    printf("[F07] 管理员:过→菜单;非管理员停留;5s 无脸回普通\n");
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
    DG_CHECK(!s_fsm.match_enabled);

    /* 非管理员命中:红弹窗 + 留在管理员模式 + 落日志 */
    ev_match_t bad = mk_match("10002", "李四", DG_ROLE_NORMAL, true);
    handle_match(bad);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == 1);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_STRANGER);

    /* 管理员命中:进菜单 */
    ev_match_t admin = mk_match("00001", "管理员A", DG_ROLE_ADMIN, true);
    handle_match(admin);
    DG_CHECK(s_fsm.state == ST_MENU);
    DG_CHECK(last_act(FSM_ACT_GOTO_PAGE) &&
             !strcmp(last_act(FSM_ACT_GOTO_PAGE)->d.page, "menu"));

    /* 返回普通 */
    auth_fsm_handle(&s_fsm, FSM_EV_BACK, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL && s_fsm.match_enabled);

    /* 5s 无脸无操作:自动回普通 */
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    const act_rec_t *tr = last_act(FSM_ACT_SET_TIMER);
    DG_CHECK(tr && tr->d.timer.timer_id == FSM_TMR_ADMIN_5S);
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = FSM_TMR_ADMIN_5S;
    d.timer.seq = tr->d.timer.seq;
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
    DG_CHECK(s_fsm.state == ST_NORMAL);
    DG_CHECK(s_fsm.match_enabled);
}

/* ================= 8 密码连错锁定 ================= */

static void t08_pwd_lock(void)
{
    printf("[F08] 密码连错 5 次锁定 60s;第 6 次直接提示锁定\n");
    fsm_reset();
    for (int i = 1; i <= 5; i++) {
        uid_flow("10001", true, DG_ROLE_NORMAL, DG_AUTH_PWD);
        DG_CHECK(s_fsm.state == ST_VERIFY);
        DG_CHECK(s_fsm.step == V_PICK_METHOD);

        /* 选密码方式 */
        fsm_event_data_t d;
        memset(&d, 0, sizeof(d));
        d.method = DG_METHOD_PWD;
        auth_fsm_handle(&s_fsm, FSM_EV_METHOD_PICK, &d);
        DG_CHECK(s_fsm.step == V_PWD);

        /* 密码错误结果 */
        memset(&d, 0, sizeof(d));
        d.result.method = DG_METHOD_PWD;
        d.result.ok = false;
        d.result.reason = DG_REASON_WRONG_PWD;
        d.result.now_ms = (int64_t)i * 10000;
        auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_RESULT, &d);
        DG_CHECK(s_fsm.state == ST_RESULT);

        /* 3s 结果展示后回普通,再走下一轮 */
        memset(&d, 0, sizeof(d));
        d.timer.timer_id = FSM_TMR_RESULT_3S;
        d.timer.seq = s_fsm.timer_active[FSM_TMR_RESULT_3S];
        auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
        DG_CHECK(s_fsm.state == ST_NORMAL);
    }
    DG_CHECK(s_fsm.lock_fail_cnt == 0);             /* 第 5 次触发锁定并清零 */
    DG_CHECK(s_fsm.lock_until_ms == 5 * 10000 + 60 * 1000);

    /* 第 6 次:输入 ID 后服务层预检 locked → 直接红弹窗,不计次数 */
    uid_flow("10001", true, DG_ROLE_NORMAL, DG_AUTH_PWD);
    DG_CHECK(s_fsm.state == ST_VERIFY);
    /* pwd_locked 预检(auth_fsm_pwd_locked):UI 在弹出密码框前调用 */
    DG_CHECK(auth_fsm_pwd_locked(&s_fsm, "10001", 5 * 10000 + 1000));
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.result.method = DG_METHOD_PWD;
    d.result.ok = false;
    d.result.locked = true;                          /* 服务层预检锁定直接拒 */
    d.result.reason = DG_REASON_WRONG_PWD;
    d.result.now_ms = 5 * 10000 + 2000;
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_RESULT, &d);
    DG_CHECK(s_fsm.state == ST_RESULT);

    /* 锁定 60s 过后:预检放行 */
    DG_CHECK(!auth_fsm_pwd_locked(&s_fsm, "10001", 5 * 10000 + 61 * 1000));
}

/* ================= 9 结果 3s 自动回;期间新验证请求被忽略 ================= */

static void t09_result_ignores_requests(void)
{
    printf("[F09] 成功 3s 自动回普通;期间新验证请求被忽略不崩\n");
    fsm_reset();
    face_detected();
    ev_match_t m = mk_match("10001", "张三", DG_ROLE_NORMAL, true);
    handle_match(m);
    DG_CHECK(s_fsm.state == ST_RESULT);

    /* 结果期间:验证按钮/菜单按钮/1:N 命中 全部忽略 */
    int recs = s_rec_cnt;
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    ev_match_t m2 = mk_match("10002", "李四", DG_ROLE_NORMAL, true);
    handle_match(m2);
    DG_CHECK(s_rec_cnt == recs);                    /* 无任何动作 */
    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(strcmp(s_fsm.cur_uid, "10001") == 0);  /* 上下文未被覆盖 */

    /* 3s 到:回普通,匹配恢复 */
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = FSM_TMR_RESULT_3S;
    d.timer.seq = s_fsm.timer_active[FSM_TMR_RESULT_3S];
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
    DG_CHECK(s_fsm.state == ST_NORMAL && s_fsm.match_enabled);
}

/* ================= 10 待机:进入与唤醒 ================= */

static void t10_standby(void)
{
    printf("[F10] 30s 无脸无操作进待机(短值验证);触摸/人脸唤醒\n");
    fsm_reset();
    auth_fsm_init(&s_fsm, 3000, 5 /*短值*/, 5, 60, rec_on_action, NULL);

    for (int i = 0; i < 4; i++)
        auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL);             /* 未到时不进 */

    auth_fsm_handle(&s_fsm, FSM_EV_TOUCH, NULL);    /* 触摸重置计数 */
    for (int i = 0; i < 4; i++)
        auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL);

    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);     /* 第 5 次(无操作)→ 待机 */
    DG_CHECK(s_fsm.state == ST_STANDBY);
    DG_CHECK(last_act(FSM_ACT_GOTO_PAGE) &&
             !strcmp(last_act(FSM_ACT_GOTO_PAGE)->d.page, "standby"));

    /* 触摸唤醒回普通 */
    auth_fsm_handle(&s_fsm, FSM_EV_TOUCH, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL && s_fsm.match_enabled);

    /* 人脸唤醒 */
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_STANDBY);
    face_detected();                                /* 检测到人脸唤醒 */
    DG_CHECK(s_fsm.state == ST_NORMAL);
}

/* ================= 11 补充:成功期间重复 1:N 忽略 / 弹窗期匹配挂起恢复 ================= */

static void t11_misc_invariants(void)
{
    printf("[F11] 结果期间开门只发一次;回普通后匹配恢复;检测画框在验证态仍输出\n");
    fsm_reset();
    face_detected();
    ev_match_t m = mk_match("10001", "张三", DG_ROLE_NORMAL, true);
    handle_match(m);
    int doors = count_act(FSM_ACT_OPEN_DOOR);
    /* 结果期间再来 1:N:重复开门被忽略(spec §5 开门时长内忽略请求) */
    handle_match(m);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == doors);

    /* 验证态:人脸检测画框照常(spec §1 detect_enabled 独立) */
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    face_detected();
    const act_rec_t *fb = last_act(FSM_ACT_FACEBOX);
    DG_CHECK(fb && fb->d.facebox.state == DG_BOX_DETECTED);
    /* 但 1.5s 窗口在验证态不启动(窗口只在普通态) */
    DG_CHECK(last_act(FSM_ACT_SET_TIMER)->d.timer.timer_id == FSM_TMR_STEP_5S);
}

int main(void)
{
    t01_normal_hit();
    t02_match_timeout();
    t03_blacklist_hit();
    t04_verify_suspends_match();
    t05_uid_reject_paths();
    t06_step_timeout_and_stale_timer();
    t07_admin_flow();
    t08_pwd_lock();
    t09_result_ignores_requests();
    t10_standby();
    t11_misc_invariants();

    DG_TEST_EXIT();
}
