/*
 * test_e2e.c — 服务层端到端测试(Phase 7,事件总线全链路,宿主跑)
 *
 * 链路:视觉(mock 由测试扮演)→ access_service(FSM)→ 门控/日志/结果事件;
 * 录入:EV_ENROLL_REQUEST → vision 槽位 → 查重入库 → EV_ENROLL_RESULT。
 * 覆盖任务要求:命中已录用户→开门+日志 result=0;陌生人→失败;录入→查重→可命中。
 */
#include "access_service.h"
#include "auth_fsm.h"
#include "cfg.h"
#include "dg_test.h"
#include "enroll_service.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "tasker.h"
#include "vision_service.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char s_dir[64];

/* ---- 事件收集 ---- */
static atomic_int s_door_open = 0;
static atomic_int s_result_cnt = 0;
static ev_auth_result_t s_results[16];
static atomic_int s_result_idx = 0;
static atomic_int s_enroll_err = 999;
static atomic_uint s_enroll_seq = 0;

static int on_door(const event_t *e, void *ud)
{
    (void)ud;
    if (((const ev_door_state_t *)e->data)->open)
        atomic_fetch_add(&s_door_open, 1);
    return 0;
}

static int on_result(const event_t *e, void *ud)
{
    (void)ud;
    int idx = atomic_load(&s_result_idx);
    if (idx < 16) {
        s_results[idx] = *(const ev_auth_result_t *)e->data;
        atomic_store(&s_result_idx, idx + 1);
    }
    atomic_fetch_add(&s_result_cnt, 1);
    return 0;
}

static int on_enroll_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_enroll_result_t *r = (const ev_enroll_result_t *)e->data;
    atomic_store(&s_enroll_err, r->err);
    atomic_store(&s_enroll_seq, r->seq);
    return 0;
}

static void wait_until_uint(int timeout_ms, atomic_uint *counter, unsigned target)
{
    for (int i = 0; i < timeout_ms; i += 5) {
        if (atomic_load(counter) >= target)
            return;
        usleep(5000);
    }
}

static void wait_until(int timeout_ms, _Atomic int *counter, int target)
{
    for (int i = 0; i < timeout_ms; i += 5) {
        if (atomic_load(counter) >= target)
            return;
        usleep(5000);
    }
}

/* 充当视觉后端:响应录入抓取请求,提交确定性伪特征 */
static atomic_int s_submit_err = 0;   /* handler 在总线线程:不用 DG_CHECK */

static int on_capture_req(const event_t *e, void *ud)
{
    (void)ud;
    const ev_capture_req_t *r = (const ev_capture_req_t *)e->data;
    uint8_t feat[32];
    memset(feat, 0xAB, sizeof(feat));           /* 固定特征(测试用) */
    int rc = vision_service_submit_feature(r->user_id, r->seq, feat, sizeof(feat));
    if (rc != DG_OK)
        atomic_store(&s_submit_err, rc);
    return 0;
}

static atomic_int s_req_cnt = 0;

static int on_capture_req_record(const event_t *e, void *ud)
{
    (void)ud;
    /* 只以原子计数同步:请求体内容正确性由 EV_ENROLL_RESULT 回执证明 */
    atomic_fetch_add(&s_req_cnt, 1);
    on_capture_req(e, ud);
    return 0;
}

int main(void)
{
    /* ---- 环境:event_bus + tasker + storage + cfg + 服务 ---- */
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(tasker_init() == TASK_OK);
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_e2e_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        return 1;
    char db[96], key[96];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    cfg_load(NULL, NULL);                       /* 纯内置默认(无文件模式) */

    event_bus_subscribe(EV_AUTH_DOOR_OPEN, on_door, NULL);
    event_bus_subscribe(EV_AUTH_RESULT, on_result, NULL);
    event_bus_subscribe(EV_ENROLL_RESULT, on_enroll_result, NULL);
    /* 录入抓取请求记录 + 以"视觉后端"身份提交特征 */
    event_bus_subscribe(EV_VISION_CAPTURE_REQ, on_capture_req_record, NULL);

    DG_CHECK(access_service_start() == DG_OK);
    DG_CHECK(enroll_service_start() == DG_OK);
    DG_CHECK(vision_service_start() == DG_OK);

    /* ---- 预置用户 10001(密码 1234,无特征) ---- */
    user_rec_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.user_id, sizeof(u.user_id), "10001");
    snprintf(u.user_name, sizeof(u.user_name), "张三");
    u.role = DG_ROLE_NORMAL;
    u.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    DG_CHECK(db_user_set_password(&u, "1234") == DG_OK);
    DG_CHECK(db_user_add(&u) == DG_OK);

    /* ---- E2E-1 录入:请求 → 抓取 → 查重入库 → 回执 ok → 库内可查 ---- */
    ev_enroll_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.user_id, sizeof(req.user_id), "10001");
    req.kind = DG_ENROLL_FACE;
    req.seq = 42;
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &req);

    wait_until(3000, &s_req_cnt, 1);            /* 请求到达"视觉" */
    DG_CHECK(atomic_load(&s_req_cnt) == 1);
    DG_CHECK(atomic_load(&s_submit_err) == 0);

    wait_until_uint(3000, &s_enroll_seq, 42);   /* 结果回执 */
    DG_CHECK(atomic_load(&s_enroll_err) == DG_OK);

    user_rec_t got;
    DG_CHECK(db_user_get("10001", &got) == DG_OK);
    DG_CHECK(got.face_vec_len > 0);             /* 特征已入库(密文) */

    /* ---- E2E-2 命中:1:N 匹配事件 → 结果 result=0 + 开门 + 日志 ---- */
    ev_match_t m;
    memset(&m, 0, sizeof(m));
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "10001");
    snprintf(m.user_name, sizeof(m.user_name), "张三");
    m.role = DG_ROLE_NORMAL;
    m.score_permille = 950;
    {
        fsm_event_data_t unused;                /* 占位说明:match 经总线投递 */
        (void)unused;
    }
    ev_match_t pub = m;
    EVENT_BUS_PUBLISH(EV_VISION_MATCH_1N, &pub);

    wait_until(3000, &s_result_cnt, 1);
    DG_CHECK(atomic_load(&s_result_cnt) == 1);
    DG_CHECK(s_results[0].result == DG_RESULT_PASS);
    DG_CHECK(s_results[0].reason == DG_REASON_OK);
    DG_CHECK(s_results[0].method == DG_METHOD_FACE_1N);
    DG_CHECK(strcmp(s_results[0].user_id, "10001") == 0);
    wait_until(1000, &s_door_open, 1);          /* 开门事件 */
    DG_CHECK(atomic_load(&s_door_open) == 1);

    /* 等 RESULT 3s 自动回普通(服务层真实时序) */
    sleep(4);

    /* 日志落库(唯一出口)与事件同源 */
    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1; q.page_size = 10;
    access_log_t rows[16];
    log_page_t page = { .logs = rows, .max = 16 };
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == 1);
    DG_CHECK(rows[0].result == DG_RESULT_PASS);

    /* ---- E2E-3 陌生人:脸出现 + 1.5s 无命中 → 失败 reason=1 ----
     * 先补一个 FACE_LOST(上一场的人走了):在场判定窗一次在场只判一次,
     * 不离场则新场景不会重新判定(2026-09-22 在场闸) */
    EVENT_BUS_PUBLISH_EMPTY(EV_VISION_FACE_LOST);
    sleep(1);                                   /* LOST 分发完再进场 */
    ev_face_box_t box = { .x = 1, .y = 2, .w = 3, .h = 4,
                          .state = DG_BOX_DETECTED };
    EVENT_BUS_PUBLISH(EV_VISION_FACE_BOX, &box);
    wait_until(5000, &s_result_cnt, 2);         /* 1.5s 窗口超时 */
    DG_CHECK(atomic_load(&s_result_cnt) == 2);
    DG_CHECK(s_results[1].result == DG_RESULT_REJECT);
    DG_CHECK(s_results[1].reason == DG_REASON_STRANGER);
    DG_CHECK(s_results[1].user_id[0] == '\0');  /* 陌生人无 ID */
    sleep(4);                                   /* 等 RESULT 回普通 */

    /* ---- E2E-4 密码验证(成功/失败)走服务 ---- */
    ev_ui_btn_t btn = { .btn = DG_BTN_VERIFY };
    EVENT_BUS_PUBLISH(EV_UI_BTN, &btn);
    ev_text_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_UID;
    snprintf(in.text, sizeof(in.text), "10001");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);

    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_PWD;
    snprintf(in.uid, sizeof(in.uid), "10001");
    snprintf(in.text, sizeof(in.text), "wrong");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
    wait_until(3000, &s_result_cnt, 3);
    DG_CHECK(s_results[2].result == DG_RESULT_REJECT);
    DG_CHECK(s_results[2].reason == DG_REASON_WRONG_PWD);

    /* 失败后回普通再走正确密码 */
    {
        /* 等待结果 3s 自动回普通 */
        sleep(4);
    }
    EVENT_BUS_PUBLISH(EV_UI_BTN, &btn);
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_UID;
    snprintf(in.text, sizeof(in.text), "10001");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_PWD;
    snprintf(in.uid, sizeof(in.uid), "10001");
    snprintf(in.text, sizeof(in.text), "1234");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
    wait_until(3000, &s_result_cnt, 4);
    DG_CHECK(s_results[3].result == DG_RESULT_PASS);
    DG_CHECK(s_results[3].method == DG_METHOD_PWD);

    /* 诊断输出 */
    for (int i = 0; i < atomic_load(&s_result_idx); i++) {
        printf("  [dbg] result[%d]: method=%d result=%d reason=%d uid='%s'\n",
               i, s_results[i].method, s_results[i].result, s_results[i].reason,
               s_results[i].user_id);
    }

    /* ---- 清理 ---- */
    enroll_service_stop();
    access_service_stop();
    vision_service_stop();
    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }
    event_bus_deinit();

    DG_TEST_EXIT();
}
