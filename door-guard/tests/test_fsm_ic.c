/*
 * test_fsm_ic.c — FSM 刷卡分支测试(ICCARD_PROTOCOL §7.2 表逐行)
 *
 * 纯事件序列驱动(同 test_auth_fsm 模式):服务层已把卡解析成
 * fsm_ic_t(found/user/role/auth_flags)后喂入;防重窗在服务层,不在此测。
 * 覆盖:普通命中/黑名单/未开IC/陌生卡四路、管理员模式(进菜单不开门/
 * 非管理员停留)、待机唤醒、弹窗期间忽略、v_ic 经 VERIFY_RESULT、
 * 读卡器未就绪 reason=9 门禁。
 */
#include "auth_fsm.h"
#include "dg_test.h"

#include <string.h>

#define REC_MAX 256
typedef struct {
    fsm_action_t act;
    fsm_action_data_t d;
} act_rec_t;

static act_rec_t s_recs[REC_MAX];
static int s_rec_cnt;
static auth_fsm_t s_fsm;

static void rec_on_action(fsm_action_t act, const fsm_action_data_t *d, void *ud)
{
    (void)ud;
    if (s_rec_cnt < REC_MAX) {
        s_recs[s_rec_cnt].act = act;
        s_recs[s_rec_cnt].d = d ? *d : (fsm_action_data_t){ 0 };
        s_rec_cnt++;
    }
}

static void fsm_reset(void)
{
    s_rec_cnt = 0;
    auth_fsm_init(&s_fsm, 3000, 30, 15, 5, 60, rec_on_action, NULL);
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

static void feed_ic(bool found, const char *uid, const char *name, int32_t role,
                    uint32_t flags)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.ic.found = found;
    snprintf(d.ic.card_no, sizeof(d.ic.card_no), "04A3B2C1");
    snprintf(d.ic.user_id, sizeof(d.ic.user_id), "%s", uid);
    snprintf(d.ic.user_name, sizeof(d.ic.user_name), "%s", name);
    d.ic.role = role;
    d.ic.auth_flags = flags;
    auth_fsm_handle(&s_fsm, FSM_EV_IC_CARD, &d);
}

static void feed_tick(void)
{
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
}

static void feed_verify_result(int32_t method, bool ok, int32_t reason)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.result.method = method;
    d.result.ok = ok;
    d.result.reason = reason;
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_RESULT, &d);
}

/* 进入 ST_VERIFY 的 v_ic 子步(真实事件序列:验证按钮→ID→方式) */
static void enter_v_ic(uint32_t flags)
{
    auth_fsm_handle(&s_fsm, FSM_EV_VERIFY_BTN, NULL);
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.uid, sizeof(d.uid), "20001");
    auth_fsm_handle(&s_fsm, FSM_EV_UID_SUBMIT, &d);

    memset(&d, 0, sizeof(d));
    d.uid_res.found = true;
    d.uid_res.role = DG_ROLE_NORMAL;
    d.uid_res.auth_flags = flags;
    snprintf(d.uid_res.user_id, sizeof(d.uid_res.user_id), "20001");
    snprintf(d.uid_res.user_name, sizeof(d.uid_res.user_name), "李四");
    auth_fsm_handle(&s_fsm, FSM_EV_UID_RESOLVED, &d);

    memset(&d, 0, sizeof(d));
    d.method = DG_METHOD_IC;
    auth_fsm_handle(&s_fsm, FSM_EV_METHOD_PICK, &d);
}

static void t_normal_branch(void)
{
    printf("[P1] 普通模式命中开门(method=4)\n");
    fsm_reset();
    feed_ic(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_ALL);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.method == DG_METHOD_IC);
    DG_CHECK(log->d.log.result == DG_RESULT_PASS);

    printf("[P2] 黑名单拒绝(reason=2)\n");
    fsm_reset();
    feed_ic(true, "30001", "王五", DG_ROLE_BLACKLIST, DG_AUTH_ALL);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_BLACKLIST);
    DG_CHECK(log->d.log.result == DG_RESULT_REJECT);

    printf("[P3] 未开 IC 拒绝(reason=6)\n");
    fsm_reset();
    feed_ic(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_FACE | DG_AUTH_PWD);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_METHOD_DISABLED);
    DG_CHECK(log->d.log.method == DG_METHOD_IC);

    printf("[P4] 陌生卡拒绝(reason=1,无用户)\n");
    fsm_reset();
    feed_ic(false, "", "", 0, 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_STRANGER);
    DG_CHECK(log->d.log.user_id[0] == '\0');
    DG_CHECK(count_act(FSM_ACT_POPUP_FAIL) == 1);
}

static void t_admin_branch(void)
{
    printf("[P5] 管理员刷卡进菜单(不开门)\n");
    fsm_reset();
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.admin_count = 1;
    auth_fsm_handle(&s_fsm, FSM_EV_ADMIN_COUNT, &d);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    feed_ic(true, "10001", "管理员", DG_ROLE_ADMIN, DG_AUTH_ALL);
    const act_rec_t *pg = last_act(FSM_ACT_GOTO_PAGE);
    DG_CHECK(pg && strcmp(pg->d.page, "menu") == 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    DG_CHECK(s_fsm.state == ST_MENU);

    printf("[P6] 管理员模式刷非管理员卡:弹窗+日志,停留本模式\n");
    fsm_reset();
    memset(&d, 0, sizeof(d));
    d.admin_count = 1;
    auth_fsm_handle(&s_fsm, FSM_EV_ADMIN_COUNT, &d);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    feed_ic(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_ALL);
    DG_CHECK(count_act(FSM_ACT_POPUP_FAIL) == 1);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_STRANGER);
    DG_CHECK(log->d.log.method == DG_METHOD_IC);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);

    printf("[P7] 管理员模式陌生卡:弹窗+日志(user_id 空)\n");
    fsm_reset();
    memset(&d, 0, sizeof(d));
    d.admin_count = 1;
    auth_fsm_handle(&s_fsm, FSM_EV_ADMIN_COUNT, &d);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
    feed_ic(false, "", "", 0, 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.user_id[0] == '\0');
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
}

static void t_standby_wake(void)
{
    printf("[P8] 待机中刷卡先唤醒再走开门分支\n");
    auth_fsm_init(&s_fsm, 3000, 1 /*1s 待机*/, 15, 5, 60, rec_on_action, NULL);
    s_rec_cnt = 0;
    feed_tick();
    feed_tick();
    DG_CHECK(s_fsm.state == ST_STANDBY);
    feed_ic(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_ALL);
    const act_rec_t *pg = last_act(FSM_ACT_GOTO_PAGE);
    DG_CHECK(pg && strcmp(pg->d.page, "home") == 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
}

static void t_verify_flow(void)
{
    printf("[P9] 弹窗期间(v_pick_method/v_ic)刷卡忽略不落日志\n");
    fsm_reset();
    enter_v_ic(DG_AUTH_IC);
    DG_CHECK(s_fsm.state == ST_VERIFY && s_fsm.step == V_IC);
    int logs_before = count_act(FSM_ACT_WRITE_LOG);
    feed_ic(true, "30001", "王五", DG_ROLE_NORMAL, DG_AUTH_ALL);  /* 他人卡 */
    feed_ic(false, "", "", 0, 0);                                 /* 陌生卡 */
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == logs_before);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);

    printf("[P10] v_ic 1:1 经 VERIFY_RESULT 开门(method=4)\n");
    feed_verify_result(DG_METHOD_IC, true, DG_REASON_OK);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.method == DG_METHOD_IC);
    DG_CHECK(log->d.log.result == DG_RESULT_PASS);

    printf("[P11] v_ic 不匹配拒绝(reason=4)\n");
    fsm_reset();
    enter_v_ic(DG_AUTH_IC);
    feed_verify_result(DG_METHOD_IC, false, DG_REASON_MISMATCH);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_MISMATCH);

    printf("[P12] 读卡器未就绪:选 IC 方式立即 reason=9\n");
    fsm_reset();
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.ic_ready = false;
    auth_fsm_handle(&s_fsm, FSM_EV_IC_STATE, &d);
    enter_v_ic(DG_AUTH_IC);
    /* PICK 已被门禁拦下:失败弹窗 + 日志 reason=9,不进 v_ic 子步 */
    DG_CHECK(s_fsm.step != V_IC);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_DEVICE_ERR);
    DG_CHECK(log->d.log.method == DG_METHOD_IC);
}

int main(void)
{
    t_normal_branch();
    t_admin_branch();
    t_standby_wake();
    t_verify_flow();
    DG_TEST_EXIT();
}
