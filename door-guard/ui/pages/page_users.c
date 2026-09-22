/*
 * page_users.c — 用户管理列表(spec-ui §3.3)
 *
 * 2026-09-21 重做:本页只做“列表 + 入口”——行点击进 user_edit 编辑页
 * (增改同一模板,见 page_user_edit.c),“添加”输入 ID 后也进同一编辑页。
 * 旧的行内二级菜单(权限/删除)与三步入库流已并入编辑页。
 */
#include "dg_log.h"
#include "err.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "storage.h"
#include "theme.h"
#include "presenters/presenter_user_edit.h"
#include "widgets/dg_avatar.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_list.h"
#include "widgets/dg_popup.h"
#include "valid_ui.h"

#include <stdio.h>
#include <string.h>

#define USERS_PAGE_SIZE 6

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_title = NULL;
static char s_row_uids[USERS_PAGE_SIZE][DG_UID_LEN];  /* 行→uid(页容量定长池) */

static void refresh_list(void);
static void on_row_click(lv_event_t *e);

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
    /* 列表来自真实枚举(字典序,取本页容量),不再按 1..2000 探测数字 ID——
     * 字母/前导零/超界 ID 的合法用户此前永远不显示,计数却正常 */
    static char ids[USERS_PAGE_SIZE][DG_UID_LEN];
    uint32_t n = 0;
    if (db_user_list_ids(ids, USERS_PAGE_SIZE, &n) != DG_OK)
        return;
    for (uint32_t i = 0; i < n; i++) {
        user_rec_t rec;
        if (db_user_get(ids[i], &rec) != DG_OK)
            continue;
        char rowtxt[DG_UID_LEN + DG_NAME_LEN + 16];
        snprintf(rowtxt, sizeof(rowtxt), "%s %s [%s]", rec.user_id, rec.user_name,
                 role_name(rec.role));
        snprintf(s_row_uids[i], sizeof(s_row_uids[i]), "%s", rec.user_id);
        /* 行首头像缩略图(40×40,libjpeg 1/4 缩放解码 + 控件内缓存);
         * 无头像传 NULL,行为与旧列表一致 */
        lv_obj_t *row = dg_list_add_row(s_list, dg_avatar_get(rec.user_id, DG_AVATAR_THUMB),
                                        rowtxt, on_row_click);
        lv_obj_set_user_data(row, s_row_uids[i]);
        /* 编辑入口必须「看得见」:行点击=编辑是无形交互,用户找不到怎么改
         * (2026-09-22 反馈「看不到编辑选项」)。行右侧常驻提示,整行可点 */
        lv_obj_t *hint = lv_label_create(row);
        lv_label_set_text(hint, _("编辑 >"));
        lv_obj_set_style_text_font(hint, DG_FONT_CN, 0);
        lv_obj_set_style_text_color(hint, DG_COL_TEXT(), 0);
        lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
        lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_title) {
        char t[32];
        snprintf(t, sizeof(t), "%s (%u)", _("用户管理"), total);
        lv_label_set_text(s_title, t);
    }
    DG_LOGI("[USERS]", "list rows=%u/%u first=%s", n, total, n ? ids[0] : "-");
}

static void on_row_click(lv_event_t *e)
{
    /* 注意:lv_event_get_user_data 返回的是“回调注册时的 user_data”(此处为
     * NULL),不是 lv_obj_set_user_data 设置的行数据——旧版用户管理因此拿到
     * “(null)”,点行进的编辑页全部显示“无”、保存必败(2026-09-21 用户反馈)。 */
    page_user_edit_open((const char *)lv_obj_get_user_data(lv_event_get_target(e)));
    navigator_push("user_edit");
}

/* 添加:先输入 ID,进同一编辑模板(ID 不存在 → 编辑页自动进入添加模式) */
static void add_id_done(void *ud, const char *id)
{
    (void)ud;
    page_user_edit_open(id);
    navigator_push("user_edit");
}

static void on_add(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = _("请输入用户ID"),
        .start_alpha = false,
        .max_len = DG_UID_LEN - 1,
        .validate = dg_ui_valid_uid,
        .on_confirm = add_id_done,
    };
    dg_popup_input(&cfg);
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

    lv_obj_t *add = dg_btn_create(parent, NULL, _("添加"));
    lv_obj_set_size(add, 180, DG_BTN_H);
    lv_obj_align(add, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(add, on_add, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = dg_btn_create_light(parent, NULL, _("返回"));
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
