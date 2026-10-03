/*
 * test_fsm_finger.c — FSM 指纹分支测试(FINGERPRINT_AS608 §5.1/§5.2 + ICCARD §7.2 表)
 *
 * 与 IC 的差异点:判定闸在 provider(一次按压一次判定),FSM 不用 window_done;
 * 未命中也是显式动作(弹窗+日志)。覆盖:四路 reason、管理员三态、待机唤醒、
 * v_finger 1:1 成功/失配、模组未就绪 reason=9、就绪态标志喂入。
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

static void feed_finger_match(bool matched, const char *uid, const char *name,
                              int32_t role, uint32_t flags)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.match.matched = matched;
    snprintf(d.match.user_id, sizeof(d.match.user_id), "%s", uid);
    snprintf(d.match.user_name, sizeof(d.match.user_name), "%s", name);
    d.match.role = role;
    d.match.auth_flags = flags;
    d.match.score_permille = 900;
    auth_fsm_handle(&s_fsm, FSM_EV_FINGER_MATCH_1N, &d);
}

static void feed_finger_verify(bool matched, const char *uid)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.match.matched = matched;
    snprintf(d.match.user_id, sizeof(d.match.user_id), "%s", uid);
    snprintf(d.match.user_name, sizeof(d.match.user_name), "李四");
    d.match.role = DG_ROLE_NORMAL;
    auth_fsm_handle(&s_fsm, FSM_EV_FINGER_VERIFY_11, &d);
}

/* 进入 v_finger 子步(真实事件序列) */
static void enter_v_finger(uint32_t flags)
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
    auth_fsm_handle(&s_fsm, FSM_EV_UID_RESOLVED, &d);

    memset(&d, 0, sizeof(d));
    d.method = DG_METHOD_FINGER;
    auth_fsm_handle(&s_fsm, FSM_EV_METHOD_PICK, &d);
}

static void enter_admin_mode(void)
{
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.admin_count = 1;
    auth_fsm_handle(&s_fsm, FSM_EV_ADMIN_COUNT, &d);
    auth_fsm_handle(&s_fsm, FSM_EV_MENU_BTN, NULL);
}

static void t_normal_branch(void)
{
    printf("[P1] 命中开门(method=2)\n");
    fsm_reset();
    feed_finger_match(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_ALL);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.method == DG_METHOD_FINGER);
    DG_CHECK(log->d.log.result == DG_RESULT_PASS);

    printf("[P2] 黑名单拒绝(reason=2)\n");
    fsm_reset();
    feed_finger_match(true, "30001", "王五", DG_ROLE_BLACKLIST, DG_AUTH_ALL);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_BLACKLIST);

    printf("[P3] 未开指纹拒绝(reason=6)\n");
    fsm_reset();
    feed_finger_match(true, "20001", "李四", DG_ROLE_NORMAL,
                      DG_AUTH_FACE | DG_AUTH_PWD);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_METHOD_DISABLED);
    DG_CHECK(log->d.log.method == DG_METHOD_FINGER);

    printf("[P4] 未命中:陌生人弹窗+日志(按压是显式动作)\n");
    fsm_reset();
    feed_finger_match(false, "", "", 0, 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    DG_CHECK(count_act(FSM_ACT_POPUP_FAIL) == 1);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_STRANGER);
    DG_CHECK(log->d.log.user_id[0] == '\0');
}

static void t_admin_branch(void)
{
    printf("[P5] 管理员指纹进菜单(不开门)\n");
    fsm_reset();
    enter_admin_mode();
    feed_finger_match(true, "10001", "管理员", DG_ROLE_ADMIN, DG_AUTH_ALL);
    const act_rec_t *pg = last_act(FSM_ACT_GOTO_PAGE);
    DG_CHECK(pg && strcmp(pg->d.page, "menu") == 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    DG_CHECK(s_fsm.state == ST_MENU);

    printf("[P6] 非管理员按压:弹窗+日志,停留本模式\n");
    fsm_reset();
    enter_admin_mode();
    feed_finger_match(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_ALL);
    DG_CHECK(count_act(FSM_ACT_POPUP_FAIL) == 1);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_STRANGER);

    printf("[P7] 未命中不打断管理员等待(同 face)\n");
    fsm_reset();
    enter_admin_mode();
    int logs_before = count_act(FSM_ACT_WRITE_LOG);
    int popups_before = count_act(FSM_ACT_POPUP_FAIL);
    feed_finger_match(false, "", "", 0, 0);
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == logs_before);
    DG_CHECK(count_act(FSM_ACT_POPUP_FAIL) == popups_before);
    DG_CHECK(s_fsm.state == ST_ADMIN_AUTH);
}

static void t_standby(void)
{
    printf("[P8] 待机中按压指纹:唤醒+验证\n");
    auth_fsm_init(&s_fsm, 3000, 1, 15, 5, 60, rec_on_action, NULL);
    s_rec_cnt = 0;
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_STANDBY);
    feed_finger_match(true, "20001", "李四", DG_ROLE_NORMAL, DG_AUTH_ALL);
    const act_rec_t *pg = last_act(FSM_ACT_GOTO_PAGE);
    DG_CHECK(pg && strcmp(pg->d.page, "home") == 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
}

static void t_press_wake(void)
{
    printf("[P13] 待机中按压沿即时亮屏(不等检索结果)
");
    auth_fsm_init(&s_fsm, 3000, 1, 15, 5, 60, rec_on_action, NULL);
    s_rec_cnt = 0;
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    auth_fsm_handle(&s_fsm, FSM_EV_TICK, NULL);
    DG_CHECK(s_fsm.state == ST_STANDBY);
    auth_fsm_handle(&s_fsm, FSM_EV_FINGER_PRESS, NULL);
    DG_CHECK(s_fsm.state == ST_NORMAL);          /* 已回普通模式 */
    const act_rec_t *pg = last_act(FSM_ACT_GOTO_PAGE);
    DG_CHECK(pg && strcmp(pg->d.page, "home") == 0);
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0); /* 仅亮屏,不开门 */
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == 0); /* 不落日志(结果事件才算动作) */

    printf("[P14] 非待机状态按压沿忽略
");
    fsm_reset();
    auth_fsm_handle(&s_fsm, FSM_EV_FINGER_PRESS, NULL);
    DG_CHECK(count_act(FSM_ACT_GOTO_PAGE) == 0);
    DG_CHECK(s_fsm.state == ST_NORMAL);
}

static void t_verify_flow(void)
{
    printf("[P9] v_finger 1:1 成功(method=2)\n");
    fsm_reset();
    enter_v_finger(DG_AUTH_FINGER | DG_AUTH_PWD);
    DG_CHECK(s_fsm.state == ST_VERIFY && s_fsm.step == V_FINGER);
    feed_finger_verify(true, "20001");
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 1);
    const act_rec_t *log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.method == DG_METHOD_FINGER);

    printf("[P10] v_finger 全部不匹配拒绝(reason=4)\n");
    fsm_reset();
    enter_v_finger(DG_AUTH_FINGER | DG_AUTH_PWD);
    feed_finger_verify(false, "20001");
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_MISMATCH);

    printf("[P11] v_finger 之外的状态忽略 1:1 结果\n");
    fsm_reset();
    feed_finger_verify(true, "20001");
    DG_CHECK(count_act(FSM_ACT_OPEN_DOOR) == 0);
    DG_CHECK(count_act(FSM_ACT_WRITE_LOG) == 0);

    printf("[P12] 模组未就绪:选指纹方式立即 reason=9\n");
    fsm_reset();
    fsm_event_data_t d;
    memset(&d, 0, sizeof(d));
    d.finger_ready = false;
    auth_fsm_handle(&s_fsm, FSM_EV_FINGER_STATE, &d);
    enter_v_finger(DG_AUTH_FINGER | DG_AUTH_PWD);
    DG_CHECK(s_fsm.step != V_FINGER);
    log = last_act(FSM_ACT_WRITE_LOG);
    DG_CHECK(log && log->d.log.reason == DG_REASON_DEVICE_ERR);
    DG_CHECK(log->d.log.method == DG_METHOD_FINGER);
}

int main(void)
{
    t_normal_branch();
    t_admin_branch();
    t_standby();
    t_press_wake();
    t_verify_flow();
    DG_TEST_EXIT();
}
