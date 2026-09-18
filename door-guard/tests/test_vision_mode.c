/*
 * test_vision_mode.c — 视觉工作模式联动(B7 §0.1:FSM 状态 → mode)
 *
 * 覆盖:
 *   1. vision_service_set_mode 参数校验 / 幂等 / 目标 uid 携带
 *   2. EV_VISION_SET_MODE 事件路径(access 与 vision 只经总线联动)
 *   3. access_service 派生:普通态→DETECT_1N、管理员态→DETECT_1N、
 *      验证输入→DETECT_ONLY、1:1 人脸子步→VERIFY_11(cur_uid)、
 *      验证结果后回 DETECT_1N
 *   4. 未知模式被拒(不下发非法 mode)
 *
 * 为什么单独一个用例:模式是"误触发开门"的第二道门禁,必须有独立回归,
 * 不能只压在 e2e 的顺带断言里。
 */
#include "dg_test.h"
#include "access_service.h"
#include "cfg.h"
#include "event_bus.h"
#include "events.h"
#include "storage.h"
#include "tasker.h"
#include "vision_backend.h"
#include "vision_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[128];

/* 等待模式生效(事件由总线线程处理,轮询带超时) */static dg_vision_mode_t wait_mode(dg_vision_mode_t want, int timeout_ms)
{
    dg_vision_mode_t m = DG_VMODE_MAX;
    for (int i = 0; i < timeout_ms; i += 5) {
        m = vision_service_get_mode();
        if (m == want)
            return m;
        usleep(5000);
    }
    return m;
}

static void pub_btn(int32_t btn)
{
    ev_ui_btn_t b;
    memset(&b, 0, sizeof(b));
    b.btn = btn;
    EVENT_BUS_PUBLISH(EV_UI_BTN, &b);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(tasker_init() == TASK_OK);
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_vmode_%d", (int)getpid());
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        return 1;
    char db[192], key[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    cfg_load(NULL);

    DG_CHECK(access_service_start() == DG_OK);
    DG_CHECK(vision_service_start() == DG_OK);
    /* 后端:测试构建里编入的是 sim(装配层在 main.c 注册,测试自己注册) */
    extern const vision_backend_ops_t vision_backend_sim;
    DG_CHECK(vision_backend_register(&vision_backend_sim) == DG_OK);
    DG_CHECK(vision_backend_start(false) == DG_OK);   /* 仅订阅录入抓取 */

    /* ---- 1. 直接 API:默认 1:N(与 FSM 开机态一致) ---- */
    DG_CHECK(vision_service_get_mode() == DG_VMODE_DETECT_1N);
    DG_CHECK(vision_service_set_mode(DG_VMODE_IDLE, NULL) == DG_OK);
    DG_CHECK(vision_service_get_mode() == DG_VMODE_IDLE);
    DG_CHECK(vision_service_set_mode((dg_vision_mode_t)DG_VMODE_MAX, NULL)
             == DG_ERR_PARAM);                        /* 越界拒绝 */
    DG_CHECK(vision_service_set_mode((dg_vision_mode_t)-1, NULL) == DG_ERR_PARAM);

    char uid[DG_UID_LEN];
    vision_service_get_verify_uid(uid, sizeof(uid));
    DG_CHECK(uid[0] == '\0');                          /* 非 VERIFY_11:无目标 */
    DG_CHECK(vision_service_set_mode(DG_VMODE_VERIFY_11, "10001") == DG_OK);
    vision_service_get_verify_uid(uid, sizeof(uid));
    DG_CHECK(strcmp(uid, "10001") == 0);
    DG_CHECK(vision_service_set_mode(DG_VMODE_DETECT_ONLY, "10001") == DG_OK);
    vision_service_get_verify_uid(uid, sizeof(uid));
    DG_CHECK(uid[0] == '\0');                          /* 非 1:1 模式不再带目标 */

    /* ---- 2. 事件路径(模块间唯一通道) ---- */
    ev_vision_mode_t mev;
    memset(&mev, 0, sizeof(mev));
    mev.mode = DG_VMODE_DETECT_1N;
    EVENT_BUS_PUBLISH(EV_VISION_SET_MODE, &mev);
    DG_CHECK(wait_mode(DG_VMODE_DETECT_1N, 1000) == DG_VMODE_DETECT_1N);

    mev.mode = DG_VMODE_MAX;                           /* 非法模式:保持原模式 */
    EVENT_BUS_PUBLISH(EV_VISION_SET_MODE, &mev);
    usleep(100 * 1000);
    DG_CHECK(vision_service_get_mode() == DG_VMODE_DETECT_1N);

    /* ---- 3. access 派生(FSM 状态 → mode) ---- */
    /* 用户 10001(普通,人脸+密码)+ 管理员 00001:有管理员才会走"点菜单要认证"
     * 那条分支(无管理员时菜单免认证直接进,见 spec-auth §3 新机引导) */
    user_rec_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.user_id, sizeof(u.user_id), "10001");
    snprintf(u.user_name, sizeof(u.user_name), "张三");
    u.role = DG_ROLE_NORMAL;
    u.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    DG_CHECK(db_user_set_password(&u, "1234") == DG_OK);
    DG_CHECK(db_user_add(&u) == DG_OK);

    user_rec_t adm;
    memset(&adm, 0, sizeof(adm));
    snprintf(adm.user_id, sizeof(adm.user_id), "00001");
    snprintf(adm.user_name, sizeof(adm.user_name), "管理员A");
    adm.role = DG_ROLE_ADMIN;
    adm.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;   /* 下面要走 1:1 人脸子步 */
    DG_CHECK(db_user_set_password(&adm, "8888") == DG_OK);
    DG_CHECK(db_user_add(&adm) == DG_OK);

    /* 普通态:1:N(先让 access 心跳把模式刷回基线) */
    DG_CHECK(wait_mode(DG_VMODE_DETECT_1N, 2000) == DG_VMODE_DETECT_1N);

    /* 点菜单:库里已有管理员 → 进管理员认证态(也允许 1:N,role 过滤在 FSM) */
    pub_btn(DG_BTN_MENU);
    DG_CHECK(wait_mode(DG_VMODE_DETECT_1N, 2000) == DG_VMODE_DETECT_1N);

    /* 点验证 → 输入 ID 子步:只检测不检索(防误触发) */
    pub_btn(DG_BTN_VERIFY);
    DG_CHECK(wait_mode(DG_VMODE_DETECT_ONLY, 2000) == DG_VMODE_DETECT_ONLY);

    /* 提交管理员 ID → 方式选择页:仍是 DETECT_ONLY */
    ev_text_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_UID;
    snprintf(in.text, sizeof(in.text), "00001");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
    usleep(200 * 1000);
    DG_CHECK(vision_service_get_mode() == DG_VMODE_DETECT_ONLY);

    /* 选 1:1 人脸 → 携带目标 uid 的 VERIFY_11(后端据此装载比对标的) */
    ev_method_pick_t mp;
    memset(&mp, 0, sizeof(mp));
    mp.method = DG_METHOD_FACE_11;
    EVENT_BUS_PUBLISH(EV_UI_METHOD_PICK, &mp);
    DG_CHECK(wait_mode(DG_VMODE_VERIFY_11, 2000) == DG_VMODE_VERIFY_11);
    vision_service_get_verify_uid(uid, sizeof(uid));
    DG_CHECK(strcmp(uid, "00001") == 0);

    /* 管理员 1:1 通过 → 进菜单(spec-auth §3):菜单页只检测不检索 */
    ev_match_t m;
    memset(&m, 0, sizeof(m));
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "00001");
    snprintf(m.user_name, sizeof(m.user_name), "管理员A");
    m.role = DG_ROLE_ADMIN;
    m.score_permille = 900;
    EVENT_BUS_PUBLISH(EV_VISION_VERIFY_11, &m);
    DG_CHECK(wait_mode(DG_VMODE_DETECT_ONLY, 2000) == DG_VMODE_DETECT_ONLY);

    /* ---- 4. 普通模式点验证:1:1 通过 → 结果态 → 3s 回普通恢复 1:N ---- */
    pub_btn(DG_BTN_BACK);                          /* 菜单返回 → 回普通 */
    DG_CHECK(wait_mode(DG_VMODE_DETECT_1N, 2000) == DG_VMODE_DETECT_1N);

    pub_btn(DG_BTN_VERIFY);
    DG_CHECK(wait_mode(DG_VMODE_DETECT_ONLY, 2000) == DG_VMODE_DETECT_ONLY);
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_UID;
    snprintf(in.text, sizeof(in.text), "10001");
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
    usleep(200 * 1000);
    EVENT_BUS_PUBLISH(EV_UI_METHOD_PICK, &mp);     /* 仍选 1:1 人脸 */
    DG_CHECK(wait_mode(DG_VMODE_VERIFY_11, 2000) == DG_VMODE_VERIFY_11);
    vision_service_get_verify_uid(uid, sizeof(uid));
    DG_CHECK(strcmp(uid, "10001") == 0);

    memset(&m, 0, sizeof(m));
    m.matched = true;
    snprintf(m.user_id, sizeof(m.user_id), "10001");
    snprintf(m.user_name, sizeof(m.user_name), "张三");
    m.role = DG_ROLE_NORMAL;
    m.score_permille = 900;
    EVENT_BUS_PUBLISH(EV_VISION_VERIFY_11, &m);
    DG_CHECK(wait_mode(DG_VMODE_DETECT_ONLY, 2000) == DG_VMODE_DETECT_ONLY);
    /* 结果态 3s 自动回普通:恢复 1:N */
    DG_CHECK(wait_mode(DG_VMODE_DETECT_1N, 6000) == DG_VMODE_DETECT_1N);

    /* ---- 清理 ---- */
    access_service_stop();
    vision_service_stop();
    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }
    event_bus_deinit();

    DG_TEST_EXIT();
}
