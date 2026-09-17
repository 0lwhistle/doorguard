/*
 * test_proto.c — proto 层事件契约测试(Phase 2)
 *
 * 需求:静态断言(role/auth_flags 位宽)已在 types.h/events.h 编译期生效;
 * 本测试覆盖"每种事件 publish→subscribe 收到且负载逐字段相等":
 * 全部 EV_* 事件各发布一条全字段填充的负载,订阅方按类型回捕后 memcmp
 * 逐字节比对(负载一律 memset 零初始化,补齐填充字节),EV_AUTH_RESULT
 * 另做逐字段显式比对(门禁最关键契约)。
 */
#include "events.h"
#include "err.h"
#include "event_bus.h"
#include "dg_test.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

/* ---- 事件目录:每个 EV_* 一条全填充负载 ---- */

typedef struct {
    event_type_t type;
    const void *payload;
    uint16_t len;
} evt_case_t;

static ev_auth_result_t p_auth;
static ev_face_box_t p_box;
static ev_match_t p_match1n, p_match11;
static ev_capture_state_t p_capture;
static ev_enroll_request_t p_req;
static ev_enroll_progress_t p_prog;
static ev_enroll_result_t p_result;
static ev_net_state_t p_net;
static ev_ntp_result_t p_ntp;
static ev_ota_progress_t p_ota;
static ev_finger_status_t p_finger;
static ev_ic_card_t p_ic;
static ev_door_state_t p_door;
static ev_standby_t p_standby;

static evt_case_t s_cases[18];
static int s_case_count = 0;

/* 目录登记:负载必须零初始化后逐字段填充(memcmp 才能代表逐字段相等) */
static void catalog_build(void)
{
    memset(&p_auth, 0, sizeof(p_auth));
    p_auth.has_user = true;
    strcpy(p_auth.user_id, "10086");
    strcpy(p_auth.user_name, "张三");
    p_auth.method = DG_METHOD_FACE_1N;
    p_auth.result = DG_RESULT_PASS;
    p_auth.reason = DG_REASON_OK;
    p_auth.ts = 1789000000;

    memset(&p_box, 0, sizeof(p_box));
    p_box.x = 10; p_box.y = 20; p_box.w = 300; p_box.h = 400;
    p_box.state = DG_BOX_MATCHED;

    memset(&p_match1n, 0, sizeof(p_match1n));
    p_match1n.matched = true;
    strcpy(p_match1n.user_id, "10086");
    strcpy(p_match1n.user_name, "张三");
    p_match1n.score_permille = 987;

    memset(&p_match11, 0, sizeof(p_match11));
    p_match11.matched = false;
    strcpy(p_match11.user_id, "10001");
    strcpy(p_match11.user_name, "李四");
    p_match11.score_permille = 512;

    memset(&p_capture, 0, sizeof(p_capture));
    p_capture.ready = false;

    memset(&p_req, 0, sizeof(p_req));
    strcpy(p_req.user_id, "10086");
    p_req.kind = DG_ENROLL_FACE;
    p_req.seq = 42;

    memset(&p_prog, 0, sizeof(p_prog));
    strcpy(p_prog.user_id, "10086");
    p_prog.kind = DG_ENROLL_FACE;
    p_prog.seq = 42;
    p_prog.percent = 73;

    memset(&p_result, 0, sizeof(p_result));
    strcpy(p_result.user_id, "10086");
    p_result.kind = DG_ENROLL_DELETE;
    p_result.seq = 43;
    p_result.err = DG_ERR_DUP_FACE;

    memset(&p_net, 0, sizeof(p_net));
    p_net.online = true;

    memset(&p_ntp, 0, sizeof(p_ntp));
    p_ntp.ok = true;
    p_ntp.err = DG_OK;
    p_ntp.synced_ts = 1789000100;

    memset(&p_ota, 0, sizeof(p_ota));
    p_ota.permille = 999;
    p_ota.done = true;
    p_ota.err = DG_OK;

    memset(&p_finger, 0, sizeof(p_finger));
    p_finger.status = DG_FINGER_PRESSED;

    memset(&p_ic, 0, sizeof(p_ic));
    strcpy(p_ic.card_no, "0134567890");

    memset(&p_door, 0, sizeof(p_door));
    p_door.open = true;

    memset(&p_standby, 0, sizeof(p_standby));
    p_standby.enter = true;

    const evt_case_t cases[] = {
        { EV_AUTH_RESULT,      &p_auth,    sizeof(p_auth) },
        { EV_AUTH_DOOR_OPEN,   NULL,       0 },
        { EV_AUTH_DOOR_CLOSE,  NULL,       0 },
        { EV_VISION_FACE_BOX,  &p_box,     sizeof(p_box) },
        { EV_VISION_FACE_LOST, NULL,       0 },
        { EV_VISION_MATCH_1N,  &p_match1n, sizeof(p_match1n) },
        { EV_VISION_VERIFY_11, &p_match11, sizeof(p_match11) },
        { EV_CAPTURE_STATE,    &p_capture, sizeof(p_capture) },
        { EV_ENROLL_REQUEST,   &p_req,     sizeof(p_req) },
        { EV_ENROLL_PROGRESS,  &p_prog,    sizeof(p_prog) },
        { EV_ENROLL_RESULT,    &p_result,  sizeof(p_result) },
        { EV_NET_STATE,        &p_net,     sizeof(p_net) },
        { EV_NET_NTP_RESULT,   &p_ntp,     sizeof(p_ntp) },
        { EV_NET_OTA_PROGRESS, &p_ota,     sizeof(p_ota) },
        { EV_FINGER_STATUS,    &p_finger,  sizeof(p_finger) },
        { EV_IC_CARD,          &p_ic,      sizeof(p_ic) },
        { EV_DOOR_STATE,       &p_door,    sizeof(p_door) },
        { EV_UI_STANDBY,       &p_standby, sizeof(p_standby) },
    };
    s_case_count = (int)(sizeof(cases) / sizeof(cases[0]));
    memcpy(s_cases, cases, sizeof(cases));
}

/* ---- 订阅侧:按类型回捕(每类型恰好一条) ---- */

static uint8_t s_captured[18][EVENT_BUS_MAX_EVENT_SIZE];
static uint16_t s_captured_len[18];
static atomic_int s_received = 0;
static atomic_int s_dup_err = 0;

static int on_any(const event_t *e, void *ud)
{
    (void)ud;
    for (int i = 0; i < s_case_count; i++) {
        if (s_cases[i].type == e->header.type) {
            /* handler 在分发线程:不用 DG_CHECK(其计数器被主线程并发访问) */
            if (s_captured_len[i] != 0)         /* 每类型只应到达一次 */
                atomic_fetch_add(&s_dup_err, 1);
            s_captured_len[i] = e->header.data_len;
            if (e->header.data_len > 0)
                memcpy(s_captured[i], e->data, e->header.data_len);
            atomic_fetch_add(&s_received, 1);
            return 0;
        }
    }
    return -1;                                  /* 收到目录外事件 */
}

int main(void)
{
    catalog_build();
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);

    /* 每种事件单独挂同一 handler:验证订阅按类型精确过滤 */
    for (int i = 0; i < s_case_count; i++)
        DG_CHECK(event_bus_subscribe(s_cases[i].type, on_any, NULL) != NULL);

    for (int i = 0; i < s_case_count; i++) {
        event_bus_err_t r = event_bus_publish(s_cases[i].type,
                                              s_cases[i].payload, s_cases[i].len);
        DG_CHECK(r == EVENT_BUS_OK);
    }

    for (int i = 0; i < 3000 && atomic_load(&s_received) < s_case_count; i++)
        usleep(2000);
    DG_CHECK(atomic_load(&s_received) == s_case_count);
    DG_CHECK(atomic_load(&s_dup_err) == 0);

    /* 负载逐字段相等:零初始化负载下 memcmp 等价于逐字段相等 */
    for (int i = 0; i < s_case_count; i++) {
        DG_CHECK(s_captured_len[i] == s_cases[i].len);
        if (s_cases[i].len > 0) {
            if (memcmp(s_captured[i], s_cases[i].payload, s_cases[i].len) != 0) {
                printf("  payload mismatch: %s\n", dg_event_name(s_cases[i].type));
                dg_fail++;
            }
        }
    }

    /* EV_AUTH_RESULT 契约显式逐字段(最关键事件,防 memcmp 掩盖布局巧合) */
    ev_auth_result_t got;
    memcpy(&got, s_captured[0], sizeof(got));
    DG_CHECK(got.has_user == p_auth.has_user);
    DG_CHECK(strcmp(got.user_id, p_auth.user_id) == 0);
    DG_CHECK(strcmp(got.user_name, p_auth.user_name) == 0);
    DG_CHECK(got.method == DG_METHOD_FACE_1N);
    DG_CHECK(got.result == DG_RESULT_PASS);
    DG_CHECK(got.reason == DG_REASON_OK);
    DG_CHECK(got.ts == p_auth.ts);

    /* 事件名表全覆盖 */
    for (int i = 0; i < s_case_count; i++)
        DG_CHECK(strcmp(dg_event_name(s_cases[i].type), "UNKNOWN") != 0);

    /* 错误码名称映射抽查 */
    DG_CHECK(strcmp(dg_err_name(DG_ERR_DUP_FACE), "DUP_FACE") == 0);
    DG_CHECK(strcmp(dg_err_name(DG_ERR_USER_LIMIT), "USER_LIMIT") == 0);
    DG_CHECK(strcmp(dg_err_name((dg_err_t)999), "UNKNOWN") == 0);

    DG_CHECK(event_bus_deinit() == EVENT_BUS_OK);
    DG_TEST_EXIT();
}
