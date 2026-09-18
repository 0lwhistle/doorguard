/*
 * page_users.c — 用户管理(spec-ui §3.3):列表 + 添加/权限/删除/人脸录入
 *
 * 冲突提示对照 spec-database 错误码;特征录入发 EV_ENROLL_REQUEST
 * (服务端 Phase 7 完成),结果经 EV_ENROLL_RESULT 回渲染。
 */
#include "dg_log.h"
#include "err.h"
#include "events.h"
#include "event_bus.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "storage.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_list.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define USERS_PAGE_SIZE 6

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_title = NULL;
static char s_row_uids[USERS_PAGE_SIZE][DG_UID_LEN];  /* 行→uid(页容量定长池) */
static char s_add_id[DG_UID_LEN];        /* 添加流程暂存 */
static char s_add_name[DG_NAME_LEN];
static char s_sel_uid[DG_UID_LEN];       /* 行选中目标 */

/* 错误码 → 文案(spec-database §2;UI 唯一映射点) */
static const char *err_text(int rc)
{
    switch (rc) {
    case DG_ERR_NO_PASSWORD:  return _("请输入密码");
    case DG_ERR_DUP_UID:      return _("该卡已绑定其他用户");   /* 见下:ID 重复 */
    case DG_ERR_DUP_IC:       return _("该卡已绑定其他用户");
    case DG_ERR_DUP_FACE:     return _("该人脸已绑定其他用户");
    case DG_ERR_DUP_FINGER:   return _("该人脸已绑定其他用户");
    case DG_ERR_USER_LIMIT:   return _("用户数已达上限");
    default:                  return _("验证失败");
    }
}

static void refresh_list(void);

/* ---- 添加:ID → 姓名 → 密码 三步入库 ---- */

static void add_pwd_done(void *ud, const char *pwd)
{
    (void)ud;
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", s_add_id);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", s_add_name);
    rec.role = DG_ROLE_NORMAL;
    rec.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;

    if (pwd[0] == '\0') {
        dg_popup_fail(_("请输入密码"), 1500, NULL, NULL);
        return;
    }
    int rc = db_user_set_password(&rec, pwd);
    if (rc == DG_OK)
        rc = db_user_add(&rec);
    if (rc == DG_OK)
        dg_popup_success(_("验证成功"), 1000, NULL, NULL);
    else
        dg_popup_fail(err_text(rc), 2000, NULL, NULL);
    refresh_list();
}

static void add_name_done(void *ud, const char *name)
{
    (void)ud;
    snprintf(s_add_name, sizeof(s_add_name), "%s", name);
    dg_popup_input(_("请输入密码"), true, add_pwd_done, NULL, NULL);
}

static void add_id_done(void *ud, const char *id)
{
    (void)ud;
    snprintf(s_add_id, sizeof(s_add_id), "%s", id);
    dg_popup_input(_("姓名"), false, add_name_done, NULL, NULL);
}

static void on_add(lv_event_t *e)
{
    (void)e;
    dg_popup_input(_("请输入用户ID"), false, add_id_done, NULL, NULL);
}

/* ---- 行选中后的二级菜单:改权限 / 删除 ---- */

static void role_pick(void *ud, int idx)
{
    (void)ud;
    user_rec_t rec;
    if (db_user_get(s_sel_uid, &rec) != DG_OK)
        return;
    rec.role = idx;                     /* 0 普通 1 管理员 2 黑名单 */
    int rc = db_user_update(&rec);
    if (rc == DG_OK)
        dg_popup_success(_("验证成功"), 800, NULL, NULL);
    else
        dg_popup_fail(err_text(rc), 2000, NULL, NULL);
    refresh_list();
}

static void del_pick(void *ud, int idx)
{
    (void)ud;
    if (idx != 0)
        return;
    int rc = db_user_del(s_sel_uid);
    if (rc == DG_OK)
        dg_popup_success(_("验证成功"), 800, NULL, NULL);
    else
        dg_popup_fail(_("用户不存在"), 1500, NULL, NULL);
    refresh_list();
}

static void row_menu(void *ud, int idx)
{
    (void)ud;
    if (idx == 0) {
        const char *const opts[] = { _("普通"), _("管理员"), _("黑名单") };
        dg_popup_choice(_("权限"), opts, 3, role_pick, NULL, NULL);
    } else {
        const char *const opts[] = { _("确认") };
        dg_popup_choice(_("删除"), opts, 1, del_pick, NULL, NULL);
    }
}

static void on_row_click(lv_event_t *e)
{
    snprintf(s_sel_uid, sizeof(s_sel_uid), "%s",
             (const char *)lv_event_get_user_data(e));
    const char *const opts[] = { _("权限"), _("删除") };
    dg_popup_choice(_("用户管理"), opts, 2, row_menu, NULL, NULL);
}

/* ---- 录入人脸:发请求(Phase 7 服务实现后闭环) ---- */

static void on_enroll_face(lv_event_t *e)
{
    (void)e;
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", s_sel_uid);
    ev.kind = DG_ENROLL_FACE;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
    dg_popup_success(_("请正对摄像头"), 1500, NULL, NULL);
}

/* ---- 列表:ID 序遍历分页(≤2000;storage 迭代列表接口 Phase 7 优化) ---- */

static const char *role_name(int32_t role)
{
    switch (role) {
    case DG_ROLE_ADMIN:     return _("管理员");
    case DG_ROLE_BLACKLIST: return _("黑名单");
    default:                return _("普通");
    }
}

static void refresh_list(void)
{
    if (!s_list)
        return;
    dg_list_clear(s_list);

    uint32_t total = 0;
    if (db_user_count(&total) != DG_OK)
        return;
    uint32_t shown = 0;
    char uid[DG_UID_LEN];
    for (uint32_t i = 1; i <= DG_USER_MAX && shown < USERS_PAGE_SIZE; i++) {
        /* 稀疏 ID:从 1 递增探测(用户 ID 由操作员自定义,演示库按序号) */
        snprintf(uid, sizeof(uid), "%u", i);
        user_rec_t rec;
        if (db_user_get(uid, &rec) != DG_OK)
            continue;
        char rowtxt[DG_UID_LEN + DG_NAME_LEN + 16];
        snprintf(rowtxt, sizeof(rowtxt), "%s %s [%s]", rec.user_id, rec.user_name,
                 role_name(rec.role));
        snprintf(s_row_uids[shown], sizeof(s_row_uids[shown]), "%s", rec.user_id);
        lv_obj_t *row = dg_list_add_row(s_list, LV_SYMBOL_EDIT, rowtxt, on_row_click);
        lv_obj_set_user_data(row, s_row_uids[shown]);
        shown++;
    }
    if (s_title) {
        char t[32];
        snprintf(t, sizeof(t), "%s (%u)", _("用户管理"), total);
        lv_label_set_text(s_title, t);
    }
}

static void on_back_page(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

void page_users_create(lv_obj_t *parent)
{
    DG_LOGI("[USERS]", "page create");

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_font(s_title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(s_title, DG_COL_TEXT(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 20);

    s_list = dg_list_create(parent);
    lv_obj_set_size(s_list, DG_SCREEN_W - 2 * DG_PAD, 700);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *add = dg_btn_create(parent, LV_SYMBOL_PLUS, _("添加"));
    lv_obj_set_size(add, 180, DG_BTN_H);
    lv_obj_align(add, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(add, on_add, LV_EVENT_CLICKED, NULL);

    lv_obj_t *face = dg_btn_create_light(parent, LV_SYMBOL_IMAGE, _("人脸"));
    lv_obj_set_size(face, 180, DG_BTN_H);
    lv_obj_align(face, LV_ALIGN_BOTTOM_MID, 0, -DG_PAD);
    lv_obj_add_event_cb(face, on_enroll_face, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 180, DG_BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(back, on_back_page, LV_EVENT_CLICKED, NULL);

    refresh_list();
}

void page_users_destroy(void)
{
    s_list = NULL;
    s_title = NULL;
    DG_LOGI("[USERS]", "page destroy");
}

void page_users_register(void)
{
    static const navigator_page_t ops = {
        .name = "users",
        .create = page_users_create,
        .destroy = page_users_destroy,
    };
    navigator_register(&ops);
}
