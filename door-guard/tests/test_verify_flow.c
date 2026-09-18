/*
 * test_verify_flow.c — 验证按钮全流程回归(UI 可见的事件序列)
 *
 * 为什么单独一个用例:主页"验证"链路横跨 UI→总线→access FSM→storage→UI,
 * 断在"FSM 动作 → UI 事件"这一跳上时(历史上正是如此)编译和 FSM 单测都发现不了,
 * 表现为"点验证后静默 5 秒弹验证失败"。本用例按用户实际操作顺序驱动,
 * 断言每一步 UI 该收到的请求事件,等价于把 PC 模拟器上的手点流程自动化:
 *
 *   点验证 → 弹 ID 框 → 提交 ID → 弹方式选择 → 选密码 → 弹密码框
 *        → 提交密码 → 结果弹窗(成功+开门/失败+原因文案)
 * 外加菜单入口两条:无管理员免认证进菜单、有管理员要认证。
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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[128];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 记录 UI 收到的请求事件(总线线程写,主线程读) */
typedef struct {
    int ask_uid;
    int input_pwd;
    char input_pwd_uid[DG_UID_LEN];
    int pick_method;
    uint32_t pick_flags;
    int result;
    bool result_ok;
    int32_t result_reason;
    bool result_not_admin;
    char result_name[DG_NAME_LEN];
    int hint_admin_auth;
    int hint_no_admin;
    int hint_clear;
    int goto_menu;
    int goto_home;
    int door_open;
} rec_t;

static rec_t s_rec;

static void lock(void) { pthread_mutex_lock(&s_mtx); }
static void unlock(void) { pthread_mutex_unlock(&s_mtx); }

static int on_ask_uid(const event_t *e, void *ud)
{
    (void)e; (void)ud; lock(); s_rec.ask_uid++; unlock(); return 0;
}
static int on_input_pwd(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ui_input_req_t *r = (const ev_ui_input_req_t *)e->data;
    lock();
    s_rec.input_pwd++;
    snprintf(s_rec.input_pwd_uid, sizeof(s_rec.input_pwd_uid), "%s", r->uid);
    unlock();
    return 0;
}
static int on_pick_method(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ui_methods_t *m = (const ev_ui_methods_t *)e->data;
    lock();
    s_rec.pick_method++;
    s_rec.pick_flags = m->auth_flags;
    unlock();
    return 0;
}
static int on_result(const event_t *e, void *ud)
{
    (void)ud;
    const ev_ui_result_t *r = (const ev_ui_result_t *)e->data;
    lock();
    s_rec.result++;
    s_rec.result_ok = r->ok;
    s_rec.result_reason = r->reason;
    s_rec.result_not_admin = r->not_admin;
    snprintf(s_rec.result_name, sizeof(s_rec.result_name), "%s", r->user_name);
    unlock();
    return 0;
}
static int on_hint(const event_t *e, void *ud)
{
    (void)ud;
    const ev_hint_t *h = (const ev_hint_t *)e->data;
    lock();
    if (h->method == DG_HINT_ADMIN_AUTH)
        s_rec.hint_admin_auth++;
    else if (h->method == DG_HINT_NO_ADMIN)
        s_rec.hint_no_admin++;
    unlock();
    return 0;
}
static int on_hint_clear(const event_t *e, void *ud)
{
    (void)e; (void)ud; lock(); s_rec.hint_clear++; unlock(); return 0;
}
static int on_goto_page(const event_t *e, void *ud)
{
    (void)ud;
    const ev_goto_page_t *p = (const ev_goto_page_t *)e->data;
    lock();
    if (!strcmp(p->page, "menu"))
        s_rec.goto_menu++;
    else if (!strcmp(p->page, "home"))
        s_rec.goto_home++;
    unlock();
    return 0;
}
static int on_door(const event_t *e, void *ud)
{
    (void)e; (void)ud; lock(); s_rec.door_open++; unlock(); return 0;
}

static int s_get(rec_t *out, int (*field)(const rec_t *))
{
    lock();
    int v = field ? field(&s_rec) : 0;
    if (out)
        *out = s_rec;
    unlock();
    return v;
}

/* 等待某个计数达到 target(事件异步处理) */
static void wait_cnt(int (*field)(const rec_t *), int target, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i += 5) {
        if (s_get(NULL, field) >= target)
            return;
        usleep(5000);
    }
}

static int f_result(const rec_t *r) { return r->result; }
static int f_ask(const rec_t *r) { return r->ask_uid; }
static int f_pwd(const rec_t *r) { return r->input_pwd; }
static int f_pick(const rec_t *r) { return r->pick_method; }
static int f_door(const rec_t *r) { return r->door_open; }
static int f_menu(const rec_t *r) { return r->goto_menu; }

static void add_user(const char *uid, const char *name, int32_t role,
                     uint32_t flags, const char *pwd)
{
    user_rec_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.user_id, sizeof(u.user_id), "%s", uid);
    snprintf(u.user_name, sizeof(u.user_name), "%s", name);
    u.role = role;
    u.auth_flags = flags;
    DG_CHECK(db_user_set_password(&u, pwd) == DG_OK);
    DG_CHECK(db_user_add(&u) == DG_OK);
}

static void btn(int32_t b)
{
    ev_ui_btn_t e = { .btn = b };
    EVENT_BUS_PUBLISH(EV_UI_BTN, &e);
}

static void input_uid(const char *uid)
{
    ev_text_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_UID;
    snprintf(in.text, sizeof(in.text), "%s", uid);
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
}

static void input_pwd(const char *uid, const char *pwd)
{
    ev_text_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = DG_INPUT_PWD;
    snprintf(in.uid, sizeof(in.uid), "%s", uid);
    snprintf(in.text, sizeof(in.text), "%s", pwd);
    EVENT_BUS_PUBLISH(EV_UI_TEXT_INPUT, &in);
}

static void pick(int32_t method)
{
    ev_method_pick_t e = { .method = method };
    EVENT_BUS_PUBLISH(EV_UI_METHOD_PICK, &e);
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(tasker_init() == TASK_OK);
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_vflow_%d", (int)getpid());
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        return 1;
    char db[192], key[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    cfg_load(NULL);

    event_bus_subscribe(EV_UI_ASK_UID, on_ask_uid, NULL);
    event_bus_subscribe(EV_UI_INPUT_PWD, on_input_pwd, NULL);
    event_bus_subscribe(EV_UI_PICK_METHOD, on_pick_method, NULL);
    event_bus_subscribe(EV_UI_RESULT, on_result, NULL);
    event_bus_subscribe(EV_UI_HINT, on_hint, NULL);
    event_bus_subscribe(EV_UI_HINT_CLEAR, on_hint_clear, NULL);
    event_bus_subscribe(EV_UI_GOTO_PAGE, on_goto_page, NULL);
    event_bus_subscribe(EV_AUTH_DOOR_OPEN, on_door, NULL);

    DG_CHECK(vision_service_start() == DG_OK);
    extern const vision_backend_ops_t vision_backend_sim;
    DG_CHECK(vision_backend_register(&vision_backend_sim) == DG_OK);
    DG_CHECK(vision_backend_start(false) == DG_OK);
    DG_CHECK(access_service_start() == DG_OK);

    add_user("10001", "张三", DG_ROLE_NORMAL, DG_AUTH_FACE | DG_AUTH_PWD, "1234");

    /* ---- 1. 空库(无管理员)点菜单:免认证进菜单 + 提示先建管理员 ---- */
    memset(&s_rec, 0, sizeof(s_rec));
    btn(DG_BTN_MENU);
    wait_cnt(f_menu, 1, 2000);
    rec_t r;
    s_get(&r, NULL);
    DG_CHECK(r.goto_menu == 1);
    DG_CHECK(r.hint_no_admin == 1);
    DG_CHECK(r.hint_admin_auth == 0);      /* 不要求管理员认证(新机鸡生蛋) */
    btn(DG_BTN_BACK);                      /* 菜单返回 → 回普通 */
    wait_cnt(NULL, 0, 100);

    /* ---- 2. 点验证 → 弹 ID 输入框 ---- */
    memset(&s_rec, 0, sizeof(s_rec));
    btn(DG_BTN_VERIFY);
    wait_cnt(f_ask, 1, 2000);
    DG_CHECK(s_get(NULL, f_ask) == 1);

    /* ---- 3. 提交 ID → 弹方式选择(带该用户开启的方式位) ---- */
    input_uid("10001");
    wait_cnt(f_pick, 1, 2000);
    s_get(&r, NULL);
    DG_CHECK(r.pick_method == 1);
    DG_CHECK(r.pick_flags == (DG_AUTH_FACE | DG_AUTH_PWD));

    /* ---- 4. 选密码 → 弹密码框(uid 原样带回) ---- */
    pick(DG_METHOD_PWD);
    wait_cnt(f_pwd, 1, 2000);
    s_get(&r, NULL);
    DG_CHECK(r.input_pwd == 1);
    DG_CHECK(!strcmp(r.input_pwd_uid, "10001"));

    /* ---- 5. 密码正确 → 结果弹窗(成功)+ 开门 + 用户名 ---- */
    input_pwd("10001", "1234");
    wait_cnt(f_result, 1, 2000);
    s_get(&r, NULL);
    DG_CHECK(r.result == 1 && r.result_ok);
    DG_CHECK(!strcmp(r.result_name, "张三"));
    DG_CHECK(s_get(NULL, f_door) >= 1);
    sleep(4);                              /* 结果态 3s 自动回普通 */

    /* ---- 6. 密码错误 → 失败弹窗 reason=密码错误(3) ---- */
    memset(&s_rec, 0, sizeof(s_rec));
    btn(DG_BTN_VERIFY);
    wait_cnt(f_ask, 1, 2000);
    input_uid("10001");
    wait_cnt(f_pick, 1, 2000);
    pick(DG_METHOD_PWD);
    wait_cnt(f_pwd, 1, 2000);
    input_pwd("10001", "9999");
    wait_cnt(f_result, 1, 2000);
    s_get(&r, NULL);
    DG_CHECK(r.result == 1 && !r.result_ok);
    DG_CHECK(r.result_reason == DG_REASON_WRONG_PWD);
    DG_CHECK(s_get(NULL, f_door) == 0);    /* 失败不开门 */
    sleep(4);

    /* ---- 7. 有管理员后:点菜单要管理员认证;管理员验证通过进菜单 ---- */
    add_user("00001", "管理员A", DG_ROLE_ADMIN, DG_AUTH_PWD, "8888");
    memset(&s_rec, 0, sizeof(s_rec));
    btn(DG_BTN_MENU);
    for (int i = 0; i < 400; i += 5) {     /* 等 HINT(管理员认证) */
        s_get(&r, NULL);
        if (r.hint_admin_auth)
            break;
        usleep(5000);
    }
    DG_CHECK(r.hint_admin_auth == 1);
    DG_CHECK(r.goto_menu == 0);            /* 未认证先进不去 */

    btn(DG_BTN_VERIFY);                    /* 管理员模式点验证 */
    wait_cnt(f_ask, 1, 2000);
    input_uid("00001");
    wait_cnt(f_pick, 1, 2000);
    pick(DG_METHOD_PWD);
    wait_cnt(f_pwd, 1, 2000);
    input_pwd("00001", "8888");
    wait_cnt(f_menu, 1, 2000);
    s_get(&r, NULL);
    DG_CHECK(r.goto_menu == 1);            /* 管理员 → 进菜单 */
    DG_CHECK(s_get(NULL, f_door) == 0);    /* 进菜单不开门 */

    /* ---- 8. 非管理员在管理员模式验证通过:不进菜单 + 「非管理员」提示 ---- */
    memset(&s_rec, 0, sizeof(s_rec));
    btn(DG_BTN_BACK);                      /* 出菜单 → 普通 */
    sleep(1);
    memset(&s_rec, 0, sizeof(s_rec));
    btn(DG_BTN_MENU);                      /* 再进管理员模式 */
    for (int i = 0; i < 400; i += 5) {
        s_get(&r, NULL);
        if (r.hint_admin_auth)
            break;
        usleep(5000);
    }
    btn(DG_BTN_VERIFY);
    wait_cnt(f_ask, 1, 2000);
    input_uid("10001");                    /* 普通用户 */
    wait_cnt(f_pick, 1, 2000);
    pick(DG_METHOD_PWD);
    wait_cnt(f_pwd, 1, 2000);
    input_pwd("10001", "1234");            /* 密码正确,但不是管理员 */
    wait_cnt(f_result, 1, 2000);
    s_get(&r, NULL);
    DG_CHECK(r.result == 1 && !r.result_ok);
    DG_CHECK(r.result_not_admin);
    DG_CHECK(r.goto_menu == 0);
    DG_CHECK(s_get(NULL, f_door) == 0);

    /* ---- 清理 ---- */
    access_service_stop();
    vision_service_stop();
    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }
    event_bus_deinit();

    DG_TEST_EXIT();
}
