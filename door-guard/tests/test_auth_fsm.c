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
    auth_fsm_init(&s_fsm, 3000 /*door*/, 30 /*standby*/, 15 /*menu*/, 5 /*lock_n*/,
                  60 /*lock_s*/, rec_on_action, NULL);
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

/* 分步驱动(需要断言中间动作时用,如 ASK_UID/ASK_PWD) */
static void uid_submit(const char *uid)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.uid, sizeof(d.uid), "%s", uid);
    auth_fsm_handle(&s_fsm, FSM_EV_UID_SUBMIT, &d);
}

static void uid_resolved(const char *uid, bool found, int32_t role, uint32_t flags)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.uid_res.user_id, sizeof(d.uid_res.user_id), "%s", uid);
    snprintf(d.uid_res.user_name, sizeof(d.uid_res.user_name), "用户%s", uid);
    d.uid_res.found = found;
    d.uid_res.role = role;
    d.uid_res.auth_flags = flags;
    auth_fsm_handle(&s_fsm, FSM_EV_UID_RESOLVED, &d);
}

static void method_pick(int32_t method)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.method = method;
    auth_fsm_handle(&s_fsm, FSM_EV_METHOD_PICK, &d);
}

static void verify_result(int32_t method, bool ok, int32_t reason)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.result.method = method;
    d.result.ok = ok;
    d.result.reason = reason;
    snprintf(d.result.user_id, sizeof(d.result.user_id), "%s", s_fsm.cur_uid);
    snprintf(d.result.user_name, sizeof(d.result.user_name), "%s", s_fsm.cur_name);
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_RESULT, &d);
}

/* 触发当前登记的定时器(取 FSM 内部 seq,模拟"真到期") */
static void fire_timer(fsm_timer_t id)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.timer.timer_id = id;
    d.timer.seq = s_fsm.timer_active[id];
    auth_fsm_handle(&s_fsm, FSM_EV_TIMER, &d);
}

static void step_timeout(void)
{
    fire_timer(FSM_TMR_STEP_5S);
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
    auth_fsm_init(&s_fsm, 3000, 5 /*短值*/, 15 /*menu*/, 5, 60, rec_on_action, NULL);

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

/* ================= 10b 菜单无操作超时回主页;待机倒计时只数主页面 ================= */

static void t10b_menu_timeout(void)
{
    printf("[F10b] 菜单 15s 无操作回主页;菜单里不数待机;回主页待机重新倒数\n");
    fsm_reset();

    /* —— 进菜单:菜单键 → 管理员认证(人脸命中管理员)通过 —— */
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
    ev_match_t m = mk_match("10001", "管理员", DG_ROLE_ADMIN, true);
    handle_match(m);
    DG_CHECK(s_fsm.state == ST_MENU);
    DG_CHECK(last_act(FSM_ACT_GOTO_PAGE) &&
             !strcmp(last_act(FSM_ACT_GOTO_PAGE)->d.page, "menu"));

    /* —— 菜单会话拉到 45s(3 轮「14 拍 + 摸一下」):期间既不超时也不待机。
     * 待机阈值 30s,若待机倒计时错误地在菜单里累加,回主页当拍就进待机 —— */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 14; i++)
            auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
        DG_CHECK(s_fsm.state == ST_MENU);           /* 14s:未到菜单超时 */
        auth_fsm_handle(&s_fsm, FSM_EV_TOUCH, NULL);/* 有操作:两个计数都清零 */
    }

    /* —— 15s 无操作 → 自动回主页面,1:N 恢复 —— */
    for (int i = 0; i < 14; i++)
        auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_MENU);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);     /* 第 15 拍:超时 */
    DG_CHECK(s_fsm.state == ST_NORMAL && s_fsm.match_enabled);
    DG_CHECK(last_act(FSM_ACT_GOTO_PAGE) &&
             !strcmp(last_act(FSM_ACT_GOTO_PAGE)->d.page, "home"));

    /* —— 回主页后待机倒计时重新开始:菜单里耗掉的 45s 不带入 —— */
    for (int i = 0; i < 29; i++)
        auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL);             /* 29s:未到 30s */
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_STANDBY);            /* 第 30s:进待机 */
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

/* ================= 12 菜单入口与验证流程 UI 契约 ================= */

static void admin_count(int32_t n)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.admin_count = n;
    auth_fsm_handle(&s_fsm, FSM_EV_ADMIN_COUNT, &d);
}

static void t12_menu_entry_and_verify_flow(void)
{
    printf("[F12] 无管理员免认证进菜单;管理员入口验证过 role;取消/未开启方式\n");

    /* --- 无管理员:免认证进菜单 + 提示先建管理员(新机鸡生蛋) --- */
    fsm_reset();
    admin_count(0);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_MENU);
    DG_CHECK(last_act(FSM_ACT_GOTO_PAGE) &&
             !strcmp(last_act(FSM_ACT_GOTO_PAGE)->d.page, "menu"));
    DG_CHECK(last_act(FSM_ACT_HINT_TEXT) &&
             last_act(FSM_ACT_HINT_TEXT)->d.misc.method == DG_HINT_NO_ADMIN);
    DG_CHECK(count_act(FSM_ACT_SET_TIMER) == 0);   /* 免认证:不起 5s 认证定时器 */

    /* 退出菜单回普通 */
    auth_fsm_handle(&s_fsm, FSM_EV_BACK, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL);

    /* --- 有管理员:仍要管理员认证;人数未知(-1)同样要求认证 --- */
    fsm_reset();
    admin_count(2);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
    DG_CHECK(last_act(FSM_ACT_HINT_TEXT)->d.misc.method == DG_HINT_ADMIN_AUTH);
    DG_CHECK(last_act(FSM_ACT_SET_TIMER)->d.timer.timer_id == FSM_TMR_ADMIN_5S);

    fsm_reset();                                   /* 未回填人数:保守要认证 */
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);

    /* --- 管理员入口点"验证":通过且是管理员 → 进菜单(spec §3) --- */
    fsm_reset();
    admin_count(1);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_VERIFY);
    DG_CHECK(count_act(FSM_ACT_ASK_UID) == 1);     /* 弹 ID 输入框 */
    uid_submit("00001");
    uid_resolved("00001", true, DG_ROLE_ADMIN, DG_AUTH_PWD);
    DG_CHECK(count_act(FSM_ACT_SHOW_METHODS) == 1);
    DG_CHECK(last_act(FSM_ACT_SHOW_METHODS)->d.misc.auth_flags == DG_AUTH_PWD);
    method_pick(DG_METHOD_PWD);
    DG_CHECK(count_act(FSM_ACT_ASK_PWD) == 1);     /* 弹密码框(带 uid) */
    DG_CHECK(!strcmp(last_act(FSM_ACT_ASK_PWD)->d.misc.uid, "00001"));
    verify_result(DG_METHOD_PWD, true, DG_REASON_OK);
    DG_CHECK(s_fsm.state == ST_MENU);
    DG_CHECK(last_act(FSM_ACT_GOTO_PAGE) &&
             !strcmp(last_act(FSM_ACT_GOTO_PAGE)->d.page, "menu"));
    /* 管理员进门是"进菜单",不是开门(开门只发生在验证成功的通行场景) */
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.result == DG_RESULT_PASS);

    /* --- 管理员入口验证通过但不是管理员:红弹窗(文案"非管理员")+ 停留 --- */
    fsm_reset();
    admin_count(1);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    uid_submit("10002");
    uid_resolved("10002", true, DG_ROLE_NORMAL, DG_AUTH_PWD);
    method_pick(DG_METHOD_PWD);
    verify_result(DG_METHOD_PWD, true, DG_REASON_OK);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);        /* 不回普通,继续尝试 */
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);   /* 非管理员不开门 */
    const act_rec_t *pf = last_act(FSM_ACT_POPUP_FAIL);
    DG_CHECK(pf && pf->d.fail.not_admin);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.result == DG_RESULT_REJECT);

    /* --- 取消验证流程:回普通 + 清提示 + 不写日志 --- */
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    DG_CHECK(s_fsm.state == ST_VERIFY);
    auth_fsm_handle(&s_fsm, FSM_EV_BACK, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL && s_fsm.match_enabled);
    DG_CHECK(last_act(FSM_ACT_HINT_CLEAR) != NULL);   /* 提示条必须被清掉 */
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == 0);      /* 取消不是验证动作 */

    /* --- 未开启的方式被拒(reason=6;防陈旧弹窗/上位机注入) --- */
    fsm_reset();
    uid_flow("10004", true, DG_ROLE_NORMAL, DG_AUTH_PWD);   /* 只开了密码 */
    method_pick(DG_METHOD_FACE_11);                          /* 却选人脸 */
    DG_CHECK(s_fsm.state == ST_RESULT);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_METHOD_DISABLED);

    /* --- 子步超时的日志方式 = 当前子步(不再是固定 PWD) --- */
    fsm_reset();
    uid_flow("10005", true, DG_ROLE_NORMAL, DG_AUTH_FINGER);
    method_pick(DG_METHOD_FINGER);
    step_timeout();
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.method == DG_METHOD_FINGER);
    DG_CHECK(last_act(FSM_ACT_WRITE_LOG)->d.log.reason == DG_REASON_TIMEOUT);
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
    t10b_menu_timeout();
    t11_misc_invariants();
    t12_menu_entry_and_verify_flow();

    DG_TEST_EXIT();
}
