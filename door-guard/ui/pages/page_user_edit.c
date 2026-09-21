/*
 * page_user_edit.c — 用户编辑页(添加/编辑同一模板,2026-09-21 用户反馈重做)
 *
 * 一页看清并编辑用户的全部信息:ID / 姓名 / 权限 / 密码 / 人脸 / 指纹 / IC 卡。
 * 没有的项显示“无”;指纹与 IC 卡硬件未接入,点击明确提示(不静默)。
 * 所有文案经 _() 走多语言;动作弹窗全部带取消。
 *
 * 两种模式:
 *   EDIT:行点击进入(带 uid),每项改动即时落库;
 *   ADD :列表页“添加”输入 ID 后进入,密码/姓名先攒在页内,
 *         [保存] 才建用户(硬规则:新用户必须设密码,否则禁止添加);
 *         人脸/指纹/IC 在保存前不可录入(用户还不存在,特征无处挂)。
 */
#include "dg_log.h"
#include "err.h"
#include "event_bus.h"
#include "events.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "presenters/presenter_capture.h"
#include "storage.h"
#include "theme.h"
#include "valid_ui.h"
#include "widgets/dg_avatar.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static char  s_uid[DG_UID_LEN];       /* EDIT:当前用户;ADD:待创建 ID */
static bool  s_add_mode;              /* true = 新用户(保存才落库) */
static char  s_pending_name[DG_NAME_LEN];
static bool  s_pending_has_pwd;       /* ADD:是否已输入密码 */
static char  s_pending_pwd[DG_PWD_MAX_LEN];

static lv_obj_t *s_title;
static lv_obj_t *s_val_name, *s_val_role, *s_val_pwd, *s_val_face,
               *s_val_finger, *s_val_ic;
static lv_obj_t *s_img_face;                    /* 人脸行头像预览 */
static lv_obj_t *s_btn_pwd, *s_btn_face, *s_btn_save, *s_btn_del;

/* 错误码 → 文案(UI 唯一映射点;修正旧版把 DUP_UID 映射成“该卡已绑定”的错误) */
static const char *err_text(int rc)
{
    switch (rc) {
    case DG_ERR_NO_PASSWORD:  return _("请先设置密码");
    case DG_ERR_DUP_UID:      return _("该用户ID已存在");
    case DG_ERR_DUP_IC:       return _("该卡已绑定其他用户");
    case DG_ERR_DUP_FACE:     return _("该人脸已绑定其他用户");
    case DG_ERR_DUP_FINGER:   return _("该指纹已绑定其他用户");
    case DG_ERR_USER_LIMIT:   return _("用户数已达上限");
    case DG_ERR_BAD_NAME:     return _("姓名不合法");
    case DG_ERR_BAD_PWD:      return _("密码不合法");
    case DG_ERR_BAD_UID:      return _("用户ID不合法");
    default:                  return _("操作失败");
    }
}

static void refresh(void);

/* 拍摄录入页入口:特征与头像由拍摄页一并发起(本页不再直发录入请求) */
static void goto_capture(void)
{
    page_capture_open(s_uid);
    navigator_push("capture");
}

/* ---- 小构件:一行 = 标题 + 值 + 动作按钮 ----
 * 层次:行卡片 = 浅蓝底 + 半透明白描边;标题降透明度(次要),值保持全黑
 * (主要);「无」占位值降透明度,与「已录入」拉开视觉层级 */

static lv_obj_t *row_create_h(lv_obj_t *parent, const char *title,
                              lv_obj_t **val_out, lv_obj_t **btn_out,
                              lv_coord_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, DG_SCREEN_W - 2 * DG_PAD, h);
    lv_obj_set_style_bg_color(row, DG_COL_BG_LIGHT(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, DG_RADIUS, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, DG_COL_BG(), 0);
    lv_obj_set_style_border_opa(row, DG_OPA_CARD_LINE, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_opa(lbl, DG_OPA_TEXT_DIM, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_TEXT(), 0);
    lv_obj_align(val, LV_ALIGN_LEFT_MID, 190, 0);
    *val_out = val;

    if (btn_out) {
        lv_obj_t *btn = dg_btn_create_light(row, NULL, _("修改"));
        lv_obj_set_size(btn, 150, 72);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -12, 0);
        *btn_out = btn;
    }
    return row;
}

static lv_obj_t *row_create(lv_obj_t *parent, const char *title,
                            lv_obj_t **val_out, lv_obj_t **btn_out)
{
    return row_create_h(parent, title, val_out, btn_out, 96);
}

static void val_set(lv_obj_t *lbl, const char *text)
{
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, DG_COL_TEXT(), 0);
    /* 「无」是占位不是数据:降透明度,与「已录入/已设置」拉开层级 */
    lv_obj_set_style_text_opa(lbl,
                              strcmp(text, _("无")) == 0 ? DG_OPA_TEXT_DIM
                                                         : LV_OPA_COVER, 0);
}

/* 行右侧动作按钮(统一尺寸/对齐) */
static lv_obj_t *row_action_btn_label(lv_obj_t *row, const char *label,
                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = dg_btn_create_light(row, NULL, label);
    lv_obj_set_size(btn, 150, 72);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *row_action_btn(lv_obj_t *row, lv_event_cb_t cb)
{
    return row_action_btn_label(row, _("修改"), cb);
}

static const char *role_name(int32_t role)
{
    switch (role) {
    case DG_ROLE_ADMIN:     return _("管理员");
    case DG_ROLE_BLACKLIST: return _("黑名单");
    default:                return _("普通");
    }
}

static void refresh(void)
{
    if (s_add_mode) {
        lv_label_set_text(s_title, _("添加用户"));
        val_set(s_val_name, s_pending_name[0] ? s_pending_name : _("无"));
        val_set(s_val_role, _("普通"));
        val_set(s_val_pwd, s_pending_has_pwd ? _("已设置") : _("无"));
        val_set(s_val_face, _("无"));
        val_set(s_val_finger, _("无"));
        val_set(s_val_ic, _("无"));
        if (s_btn_pwd)
            dg_btn_set_label(s_btn_pwd, _("设置"));
        if (s_btn_face)
            lv_obj_add_flag(s_btn_face, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_save)
            lv_obj_clear_flag(s_btn_save, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_del)
            lv_obj_add_flag(s_btn_del, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_title, _("用户编辑"));
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (db_user_get(s_uid, &rec) != DG_OK) {
        val_set(s_val_name, _("用户不存在"));
        return;
    }
    val_set(s_val_name, rec.user_name[0] ? rec.user_name : _("无"));
    val_set(s_val_role, role_name(rec.role));
    val_set(s_val_pwd, _("已设置"));
    val_set(s_val_face, rec.face_vec_len > 0 ? _("已录入") : _("无"));
    val_set(s_val_finger, rec.finger_vec_len > 0 ? _("已录入") : _("无"));
    val_set(s_val_ic, _("无"));
    /* 头像预览:有人脸才有头像(同一生命周期);人脸行加高到 128,预览 96×96 */
    if (s_img_face) {
        const lv_img_dsc_t *av = dg_avatar_get(s_uid, DG_AVATAR_FULL);
        if (av && rec.face_vec_len > 0) {
            lv_img_set_src(s_img_face, av);
            lv_img_set_zoom(s_img_face, (uint16_t)(256 * 96 / 160));
            lv_obj_update_layout(s_img_face);
            lv_obj_clear_flag(s_img_face, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_img_face, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_btn_pwd)
        dg_btn_set_label(s_btn_pwd, _("修改"));
    if (s_btn_face)
        lv_obj_clear_flag(s_btn_face, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_save)
        lv_obj_add_flag(s_btn_save, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_del)
        lv_obj_clear_flag(s_btn_del, LV_OBJ_FLAG_HIDDEN);
}

/* ---- 动作:姓名 / 权限 / 密码 / 人脸 / 指纹 / IC / 保存 / 删除 ---- */

static void apply_name(void *ud, const char *text)
{
    (void)ud;
    if (s_add_mode) {
        snprintf(s_pending_name, sizeof(s_pending_name), "%s", text);
    } else {
        user_rec_t rec;
        if (db_user_get(s_uid, &rec) != DG_OK)
            return;
        snprintf(rec.user_name, sizeof(rec.user_name), "%s", text);
        int rc = db_user_update(&rec);
        if (rc != DG_OK) {
            dg_popup_fail(err_text(rc), 2000, NULL, NULL);
            return;
        }
    }
    refresh();
}

static void on_name(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = _("姓名"),
        .start_alpha = true,
        .max_len = DG_NAME_LEN - 1,
        .validate = dg_ui_valid_name,
        .on_confirm = apply_name,
    };
    dg_popup_input(&cfg);               /* 弹窗自带取消(spec 修复后) */
}

static void apply_pwd(void *ud, const char *pwd)
{
    (void)ud;
    if (s_add_mode) {
        snprintf(s_pending_pwd, sizeof(s_pending_pwd), "%s", pwd);
        s_pending_has_pwd = pwd[0] != '\0';
        refresh();
        return;
    }
    user_rec_t rec;
    if (db_user_get(s_uid, &rec) != DG_OK)
        return;
    int rc = db_user_set_password(&rec, pwd);
    if (rc == DG_OK)
        rc = db_user_update(&rec);      /* set_password 只算哈希,落库要 update */
    if (rc == DG_OK)
        dg_popup_success(_("已保存"), 800, NULL, NULL);
    else
        dg_popup_fail(err_text(rc), 2000, NULL, NULL);
}

static void on_pwd(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = s_add_mode ? _("设置密码") : _("修改密码"),
        .mask_text = true,
        .start_alpha = false,
        .max_len = DG_PWD_MAX_LEN - 1,
        .validate = dg_ui_valid_pwd,
        .on_confirm = apply_pwd,
    };
    dg_popup_input(&cfg);
}

static void apply_role(void *ud, int idx)
{
    (void)ud;
    user_rec_t rec;
    if (db_user_get(s_uid, &rec) != DG_OK)
        return;
    rec.role = idx;
    int rc = db_user_update(&rec);
    if (rc != DG_OK)
        dg_popup_fail(err_text(rc), 2000, NULL, NULL);
    refresh();
}

static void on_role(lv_event_t *e)
{
    (void)e;
    if (s_add_mode) {
        dg_popup_fail(_("请先保存用户"), 1200, NULL, NULL);
        return;
    }
    const char *const opts[] = { _("普通"), _("管理员"), _("黑名单") };
    dg_popup_choice(_("权限"), opts, 3, apply_role, NULL, NULL);
}

static void apply_face_pick(void *ud, int idx);   /* on_face 先用后定义 */

static void on_face(lv_event_t *e)
{
    (void)e;
    user_rec_t rec;
    if (db_user_get(s_uid, &rec) != DG_OK)
        return;
    if (rec.face_vec_len > 0) {
        /* 已录入:给“重录 / 清除”两个选项(弹窗带取消);重录走拍摄页 */
        const char *const opts[] = { _("重录"), _("清除") };
        dg_popup_choice(_("人脸"), opts, 2, apply_face_pick, NULL, NULL);
        return;
    }
    goto_capture();
}

static void apply_face_pick(void *ud, int idx)
{
    (void)ud;
    if (idx == 0) {
        goto_capture();
        return;
    }
    /* 清除人脸(头像随行消失:db_user_clear_face 连带清 avatar) */
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", s_uid);
    ev.kind = DG_ENROLL_FACE_CLEAR;
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
}

static void on_finger(lv_event_t *e)
{
    (void)e;
    dg_popup_fail(_("硬件未接入"), 1500, NULL, NULL);
}

static void on_ic(lv_event_t *e)
{
    (void)e;
    dg_popup_fail(_("硬件未接入"), 1500, NULL, NULL);
}

static void on_save(lv_event_t *e)
{
    (void)e;
    if (!s_pending_has_pwd) {
        dg_popup_fail(_("请先设置密码"), 1500, NULL, NULL);   /* 硬规则:新用户必设密码 */
        return;
    }
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", s_uid);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", s_pending_name);
    rec.role = DG_ROLE_NORMAL;
    rec.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    int rc = db_user_set_password(&rec, s_pending_pwd);
    if (rc == DG_OK)
        rc = db_user_add(&rec);
    if (rc != DG_OK) {
        dg_popup_fail(err_text(rc), 2000, NULL, NULL);
        return;
    }
    /* 建好即转编辑模式:人脸/指纹/IC 从这里开始可录 */
    s_add_mode = false;
    memset(s_pending_pwd, 0, sizeof(s_pending_pwd));
    dg_popup_success(_("已保存"), 800, NULL, NULL);
    refresh();
}

static void apply_del(void *ud, int idx)
{
    (void)ud;
    if (idx != 0)
        return;
    dg_avatar_invalidate(s_uid);        /* 用户即删:头像缓存同步作废 */
    ev_enroll_request_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.user_id, sizeof(ev.user_id), "%s", s_uid);
    ev.kind = DG_ENROLL_DELETE;         /* 经 enroll 服务:DB+视觉特征库一起删 */
    ev.seq = (uint32_t)time(NULL);
    EVENT_BUS_PUBLISH(EV_ENROLL_REQUEST, &ev);
    navigator_back();
}

static void on_del(lv_event_t *e)
{
    (void)e;
    const char *const opts[] = { _("删除") };
    dg_popup_choice(_("确认删除该用户"), opts, 1, apply_del, NULL, NULL);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

/* ---- 录入结果回执(桥转发,UI 线程;人脸录入的回执由拍摄页处理,这里只管清除/删除) ---- */

static void on_evt(const ui_evt_t *evt)
{
    if (evt->kind != UI_EVT_ENROLL_RESULT)
        return;
    if (strcmp(evt->enroll.user_id, s_uid) != 0)
        return;
    if (evt->enroll.kind == DG_ENROLL_FACE_CLEAR && evt->enroll.err == DG_OK) {
        dg_avatar_invalidate(s_uid);     /* 头像随人脸清除,缓存同步作废 */
        dg_popup_success(_("已清除"), 800, NULL, NULL);
    }
    refresh();
}

void page_user_edit_evt(const ui_evt_t *evt)
{
    if (evt->kind != UI_EVT_ENROLL_RESULT)
        return;
    on_evt(evt);
}

/* ---- 页面装配 ---- */

static lv_obj_t *page_create_(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, DG_COL_TEXT(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 20);
    return lbl;
}

void page_user_edit_create(lv_obj_t *parent)
{
    s_title = page_create_(parent, s_add_mode ? _("添加用户") : _("用户编辑"));

    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, DG_SCREEN_W, DG_SCREEN_H - 320);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *v;
    lv_obj_t *row;

    row = row_create(col, _("用户ID"), &v, NULL);
    (void)row;
    val_set(v, s_uid);

    row = row_create(col, _("姓名"), &s_val_name, NULL);
    row_action_btn(row, on_name);

    row = row_create(col, _("权限"), &s_val_role, NULL);
    row_action_btn(row, on_role);

    row_create(col, _("密码"), &s_val_pwd, &s_btn_pwd);
    lv_obj_add_event_cb(s_btn_pwd, on_pwd, LV_EVENT_CLICKED, NULL);

    /* 人脸行:值与按钮之外再挂一个头像预览位(无头像时隐藏,值区显示「无」)。
     * 行加高到 128:160 宽的 img 部件缩放绘制到 96px,原 96 行高下头像
     * 会滑进右侧「修改」按钮底下(布局重叠),加高 + 右移让开按钮 */
    lv_obj_t *face_row = row_create_h(col, _("人脸"), &s_val_face, &s_btn_face, 128);
    lv_obj_add_event_cb(s_btn_face, on_face, LV_EVENT_CLICKED, NULL);
    s_img_face = lv_img_create(face_row);
    /* 不加边框:zoom 只缩小绘制,部件包围盒仍是 160×160,边框会画到行外 */
    lv_obj_align(s_img_face, LV_ALIGN_RIGHT_MID, -218, 0);
    lv_obj_add_flag(s_img_face, LV_OBJ_FLAG_HIDDEN);

    row = row_create(col, _("指纹"), &s_val_finger, NULL);
    row_action_btn_label(row, _("录入"), on_finger);

    row = row_create(col, _("IC卡"), &s_val_ic, NULL);
    row_action_btn_label(row, _("绑定"), on_ic);

    s_btn_save = dg_btn_create(parent, NULL, _("保存"));
    lv_obj_set_size(s_btn_save, 180, DG_BTN_H);
    lv_obj_align(s_btn_save, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(s_btn_save, on_save, LV_EVENT_CLICKED, NULL);

    s_btn_del = dg_btn_create(parent, NULL, _("删除"));
    lv_obj_set_size(s_btn_del, 180, DG_BTN_H);
    lv_obj_align(s_btn_del, LV_ALIGN_BOTTOM_LEFT, DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(s_btn_del, on_del, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = dg_btn_create_light(parent, NULL, _("返回"));
    lv_obj_set_size(back, 180, DG_BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -DG_PAD, -DG_PAD);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    refresh();
}

void page_user_edit_destroy(void)
{
    s_title = s_val_name = s_val_role = s_val_pwd = NULL;
    s_val_face = s_val_finger = s_val_ic = NULL;
    s_img_face = NULL;
    s_btn_pwd = s_btn_face = s_btn_save = s_btn_del = NULL;
    memset(s_pending_pwd, 0, sizeof(s_pending_pwd));
}

void page_user_edit_open(const char *uid)
{
    s_pending_name[0] = '\0';
    s_pending_has_pwd = false;
    memset(s_pending_pwd, 0, sizeof(s_pending_pwd));

    /* 按“用户是否存在”自判模式:存在 = 编辑;不存在 = 添加(ID 为待创建)。
     * 列表页因此不需要知道模式语义——传入 ID 进来就是同一个入口。 */
    user_rec_t rec;
    if (uid && uid[0] && db_user_get(uid, &rec) == DG_OK) {
        s_add_mode = false;
        snprintf(s_uid, sizeof(s_uid), "%s", uid);
    } else {
        s_add_mode = true;
        snprintf(s_uid, sizeof(s_uid), "%s", uid ? uid : "");
    }
}
