/*
 * page_user_edit.c — 用户编辑页(添加/编辑同一模板,2026-09-21 用户反馈重做)
 *
 * 2026-09-27 全字段草稿化(用户反馈「编辑不要立即落盘,保存才保存」):
 * 姓名/权限/密码与此前一样攒页内草稿;**特征(人脸)草稿在 enroll 服务**
 * ——拍摄回执 OK = 草稿就绪(未落库),本页「保存」经 commit_draft 落库,
 * 返回「直接退出」经 discard_draft 丢弃;「清除人脸」也改为草稿标志,
 * 保存时同步清。任何未保存修改(含 ADD 待建用户的姓名/密码)在返回时
 * 弹「保存退出/直接退出」确认(dg_edit_nav)。
 *
 * 两种模式:
 *   EDIT:行点击进入(带 uid),字段改动攒草稿,「保存」提交;
 *   ADD :列表页“添加”输入 ID 后进入,密码/姓名先攒在页内,
 *         [保存] 才建用户(硬规则:新用户必须设密码,否则禁止添加);
 *         人脸/指纹/IC 在保存前不可录入(用户还不存在,特征无处挂)。
 */
#include "dg_log.h"
#include "enroll_service.h"
#include "err.h"
#include "events.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "presenters/presenter_capture.h"
#include "theme.h"
#include "types.h"
#include "valid_ui.h"
#include "widgets/dg_avatar.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_edit_nav.h"
#include "widgets/dg_popup.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static char  s_uid[DG_UID_LEN];       /* EDIT:当前用户;ADD:待创建 ID */
static bool  s_add_mode;              /* true = 新用户(保存才落库) */
static char  s_pending_name[DG_NAME_LEN];
static bool  s_pending_has_pwd;       /* ADD:是否已输入密码 */
static char  s_pending_pwd[DG_PWD_MAX_LEN];

/* EDIT 草稿(保存才落库) */
static char  s_draft_name[DG_NAME_LEN];
static int32_t s_draft_role;
static char  s_draft_pwd[DG_PWD_MAX_LEN];   /* 空 = 不修改密码 */
static bool  s_dirty;                        /* 姓名/权限/密码有无改动 */
static bool  s_face_clear_pending;           /* 保存时清除人脸(草稿语义) */

static lv_obj_t *s_title;
static lv_obj_t *s_val_name, *s_val_role, *s_val_pwd, *s_val_face,
               *s_val_finger, *s_val_ic;
static lv_obj_t *s_img_face;                    /* 人脸行头像预览 */
static lv_obj_t *s_btn_pwd, *s_btn_face;
static lv_obj_t *s_btn_finger, *s_btn_ic;
static lv_obj_t *s_row_del;                     /* 「删除用户」行(EDIT 才显示) */

/* 指纹/IC 录入流状态(2026-10-01):页面退出时撤销,防流程悬挂 */
static int32_t  s_fp_pages[DG_FINGER_PAGES_MAX];
static uint32_t s_fp_cnt;
static bool     s_fp_enrolling;                 /* 指纹两次按压进行中 */
static bool     s_ic_enrolling;                 /* 等待刷卡绑定中 */

/* 卡号展示掩码(********+末4;spec-database §3,UI 本地呈现不改数据) */
static void mask_card(const char *no, char *out, size_t cap)
{
    size_t n = no ? strnlen(no, DG_IC_LEN) : 0;
    if (n < 8) {
        snprintf(out, cap, "%s", _("********"));
        return;
    }
    snprintf(out, cap, "********%s", no + n - 4);
}

static void cancel_bio_flows(void)
{
    if (s_fp_enrolling) {
        bridge_enroll_request(s_uid, DG_ENROLL_FINGER_CANCEL);
        s_fp_enrolling = false;
    }
    if (s_ic_enrolling) {
        bridge_enroll_request(s_uid, DG_ENROLL_IC_CANCEL);
        s_ic_enrolling = false;
    }
}

/* 错误码 → 文案(UI 唯一映射点;修正旧版把 DUP_UID 映射成“该卡已绑定”的错误) */
static const char *err_text(int rc)
{
    switch (rc) {
    case DG_ERR_NO_PASSWORD:  return _("请先设置密码");
    case DG_ERR_DUP_UID:      return _("该用户ID已存在");
    case DG_ERR_DUP_IC:       return _("该卡已绑定其他用户");
    case DG_ERR_DUP_FACE:     return _("该人脸已绑定其他用户");
    case DG_ERR_DUP_FINGER:   return _("该指纹已绑定其他用户");
    case DG_ERR_FINGER_FULL:  return _("指纹库已满");
    case DG_ERR_FINGER_LIMIT: return _("该用户指纹已达上限");
    case DG_ERR_USER_LIMIT:   return _("用户数已达上限");
    case DG_ERR_BAD_NAME:     return _("姓名不合法");
    case DG_ERR_BAD_PWD:      return _("密码不合法");
    case DG_ERR_BAD_UID:      return _("用户ID不合法");
    default:                  return _("操作失败,请重试");
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
    lv_obj_set_style_text_font(lbl, DG_FONT_SUB, 0);
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
            dg_btn_set_label(s_btn_pwd, s_pending_has_pwd ? _("修改") : _("设置"));
        if (s_btn_face)
            lv_obj_add_flag(s_btn_face, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_finger)
            dg_btn_set_label(s_btn_finger, _("录入"));
        if (s_btn_ic)
            dg_btn_set_label(s_btn_ic, _("录入"));
        if (s_row_del)
            lv_obj_add_flag(s_row_del, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_title, _("用户编辑"));
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (enroll_service_user_get(s_uid, &rec) != DG_OK) {
        val_set(s_val_name, _("用户不存在"));
        return;
    }
    val_set(s_val_name, s_draft_name[0] ? s_draft_name : _("无"));
    val_set(s_val_role, role_name(s_draft_role));
    val_set(s_val_pwd, s_draft_pwd[0] ? _("已修改") : _("已设置"));

    /* 人脸行三态草稿显示:服务内草稿(已拍未保存)> 清除标志(待清除)>
     * DB 现值。按钮一律「修改」;弹窗内提供 重录/清除 */
    const bool face_draft = enroll_service_draft_active(s_uid);
    if (face_draft)
        val_set(s_val_face, _("已拍摄，未保存"));
    else if (s_face_clear_pending)
        val_set(s_val_face, _("待清除，未保存"));
    else
        val_set(s_val_face, rec.face_vec_len > 0 ? _("已录入") : _("无"));
    dg_btn_set_label(s_btn_face, _("修改"));

    /* 指纹行:已录 n/3(独立 fingerprints 表是唯一事实源;users.finger_vec
     * 废弃列不再读取)。录入中显示进度文案 */
    if (enroll_service_finger_pages(s_uid, s_fp_pages, DG_FINGER_PAGES_MAX,
                                    &s_fp_cnt) != DG_OK)
        s_fp_cnt = 0;
    if (s_fp_enrolling)
        val_set(s_val_finger, _("请按压指纹…"));
    else if (s_fp_cnt > 0) {
        char fv[32];
        snprintf(fv, sizeof(fv), _("已录 %d/3 枚"), (int)s_fp_cnt);
        val_set(s_val_finger, fv);
    } else
        val_set(s_val_finger, _("无"));
    dg_btn_set_label(s_btn_finger, s_fp_cnt > 0 ? _("修改") : _("录入"));

    /* IC 行:卡号展示一律掩码(spec-database §3) */
    if (s_ic_enrolling)
        val_set(s_val_ic, _("请刷卡…"));
    else if (rec.ic_card[0]) {
        char masked[16];
        mask_card(rec.ic_card, masked, sizeof(masked));
        val_set(s_val_ic, masked);
        dg_btn_set_label(s_btn_ic, _("修改"));
    } else {
        val_set(s_val_ic, _("无"));
        dg_btn_set_label(s_btn_ic, _("录入"));
    }

    /* 头像预览:草稿期用内存图(dg_avatar_decode),已落库才走 DB 缓存。
     * 人脸行加高到 128,预览 96×96 */
    if (s_img_face) {
        const lv_image_dsc_t *av = NULL;
        if (face_draft) {
            const uint8_t *jpeg = NULL;
            size_t jlen = 0;
            if (enroll_service_draft_avatar(s_uid, &jpeg, &jlen))
                av = dg_avatar_decode(jpeg, jlen, DG_AVATAR_FULL);
        } else if (rec.face_vec_len > 0) {
            av = dg_avatar_get(s_uid, DG_AVATAR_FULL);
        }
        if (av) {
            lv_image_set_src(s_img_face, av);
            lv_obj_clear_flag(s_img_face, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_img_face, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_btn_pwd)
        dg_btn_set_label(s_btn_pwd, _("修改"));
    if (s_row_del)
        lv_obj_clear_flag(s_row_del, LV_OBJ_FLAG_HIDDEN);
}

/* ---- 动作:姓名 / 权限 / 密码 / 人脸 / 指纹 / IC / 保存 / 删除 ---- */

static void draft_name(void *ud, const char *text)
{
    (void)ud;
    snprintf(s_draft_name, sizeof(s_draft_name), "%s", text);
    if (s_add_mode)
        snprintf(s_pending_name, sizeof(s_pending_name), "%s", text);
    else
        s_dirty = true;
    refresh();
}

static void on_name(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = _("姓名"),
        .start_alpha = true,
        .max_len = DG_NAME_LEN - 1,
        .initial = s_add_mode ? s_pending_name : s_draft_name,
        .validate = dg_ui_valid_name,
        .on_confirm = draft_name,
    };
    dg_popup_input(&cfg);               /* 弹窗自带取消(spec 修复后) */
}

static void draft_pwd(void *ud, const char *pwd)
{
    (void)ud;
    if (s_add_mode) {
        snprintf(s_pending_pwd, sizeof(s_pending_pwd), "%s", pwd);
        s_pending_has_pwd = pwd[0] != '\0';
    } else {
        snprintf(s_draft_pwd, sizeof(s_draft_pwd), "%s", pwd);
        s_dirty = true;
    }
    refresh();
}

static void on_pwd(lv_event_t *e)
{
    (void)e;
    const dg_popup_input_cfg_t cfg = {
        .title = s_add_mode ? _("设置密码") : _("修改密码"),
        .mask_text = true,
        .start_alpha = false,
        .max_len = DG_PWD_MAX_LEN - 1,
        .initial = s_add_mode ? s_pending_pwd : s_draft_pwd,
        .validate = dg_ui_valid_pwd,
        .on_confirm = draft_pwd,
    };
    dg_popup_input(&cfg);
}

static void draft_role(void *ud, int idx)
{
    (void)ud;
    if (s_add_mode)
        return;                          /* ADD 权限固定普通,入口已拦 */
    s_draft_role = (int32_t)idx;
    s_dirty = true;
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
    dg_popup_choice(_("权限"), opts, 3, draft_role, NULL, NULL);
}

static void apply_face_pick(void *ud, int idx);   /* on_face 先用后定义 */

static void on_face(lv_event_t *e)
{
    (void)e;
    user_rec_t rec;
    if (enroll_service_user_get(s_uid, &rec) != DG_OK)
        return;
    if (rec.face_vec_len > 0 || enroll_service_draft_active(s_uid)) {
        /* 已有(库内或草稿):重录 / 清除(清除=红色,弹窗带取消) */
        const char *const opts[] = { _("重录"), _("清除") };
        dg_popup_choice_ex(_("人脸"), opts, 2, 1u << 1, apply_face_pick,
                           NULL, NULL);
        return;
    }
    goto_capture();
}

static void apply_face_pick(void *ud, int idx)
{
    (void)ud;
    if (idx == 0) {
        goto_capture();                  /* 重录:新草稿覆盖旧草稿 */
        return;
    }
    /* 清除 = 草稿语义:先丢服务内草稿(若有),再挂清除标志;保存才生效。
     * DB 本就无人脸且无草稿时无事可做 */
    enroll_service_discard_draft(s_uid);
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (enroll_service_user_get(s_uid, &rec) == DG_OK && rec.face_vec_len > 0) {
        s_face_clear_pending = true;
        s_dirty = true;
    }
    refresh();
}

/* ---- 指纹(2026-10-01:每用户 ≤3 枚,录入/逐枚删除走 enroll 事件) ---- */

static void apply_finger_del(void *ud, int idx)
{
    int32_t page = (int32_t)(uintptr_t)ud;
    (void)idx;                            /* 单选项确认弹窗:仅「删除」 */
    bridge_enroll_request_arg(s_uid, DG_ENROLL_FINGER_DEL, page);
    refresh();
}

static void apply_finger_pick(void *ud, int idx)
{
    (void)ud;
    if (s_fp_enrolling) {                 /* 录入中再点 = 取消本次 */
        cancel_bio_flows();
        refresh();
        return;
    }
    if (idx == 0) {
        if (s_fp_cnt >= DG_FINGER_PAGES_MAX) {
            dg_popup_fail(_("该用户指纹已达上限"), 1500, NULL, NULL);
            return;
        }
        s_fp_enrolling = true;
        bridge_enroll_request(s_uid, DG_ENROLL_FINGER);
        refresh();
        return;
    }
    /* idx>0:删除第 idx-1 枚(红色确认,破坏性操作;模式同「删除用户」) */
    int32_t page = s_fp_pages[idx - 1];
    const char *const del_opts[] = { _("删除") };
    dg_popup_choice_ex(_("确认删除该指纹"), del_opts, 1, 1u << 0,
                       apply_finger_del, NULL, (void *)(uintptr_t)page);
}

static void on_finger(lv_event_t *e)
{
    (void)e;
    if (s_add_mode) {
        dg_popup_fail(_("请先保存用户"), 1500, NULL, NULL);
        return;
    }
    if (enroll_service_finger_pages(s_uid, s_fp_pages, DG_FINGER_PAGES_MAX,
                                    &s_fp_cnt) != DG_OK)
        s_fp_cnt = 0;

    if (s_fp_enrolling) {
        dg_popup_fail(_("正在录入，请按压指纹"), 1200, NULL, NULL);
        return;
    }

    /* 选项 = [录入新指纹(未满时)] + 逐枚 [删除 指纹N] */
    char labels[1 + DG_FINGER_PAGES_MAX][48];
    const char *opts[1 + DG_FINGER_PAGES_MAX];
    int n = 0;
    if (s_fp_cnt < DG_FINGER_PAGES_MAX)
        opts[n++] = _("录入新指纹");
    for (uint32_t i = 0; i < s_fp_cnt; i++) {
        snprintf(labels[n], sizeof(labels[n]), _("删除 第%d枚指纹"), (int)i + 1);
        opts[n] = labels[n];
        n++;
    }
    if (n == 1)
        apply_finger_pick(NULL, 0);       /* 未录任何枚:免选择直达录入 */
    else
        dg_popup_choice(_("指纹"), opts, n, apply_finger_pick, NULL, NULL);
}

/* ---- IC 卡(绑卡 = 下一张刷入的卡;解绑红色确认) ---- */

static void apply_ic_unbind(void *ud, int idx);

static void apply_ic_pick(void *ud, int idx)
{
    (void)ud;
    if (idx == 0) {                       /* 录入/重新录入 */
        s_ic_enrolling = true;
        bridge_enroll_request(s_uid, DG_ENROLL_IC);
        refresh();
        return;
    }
    /* 解绑:红色确认(模式同「删除用户」) */
    const char *const unbind_opts[] = { _("解绑") };
    dg_popup_choice_ex(_("确认解绑该IC卡"), unbind_opts, 1, 1u << 0,
                       apply_ic_unbind, NULL, NULL);
}

static void apply_ic_unbind(void *ud, int idx)
{
    (void)ud;
    (void)idx;
    bridge_enroll_request(s_uid, DG_ENROLL_IC_CLEAR);
    refresh();
}

static void on_ic(lv_event_t *e)
{
    (void)e;
    if (s_add_mode) {
        dg_popup_fail(_("请先保存用户"), 1500, NULL, NULL);
        return;
    }
    if (s_ic_enrolling) {                 /* 再点 = 取消等待 */
        cancel_bio_flows();
        refresh();
        return;
    }
    user_rec_t rec;
    if (enroll_service_user_get(s_uid, &rec) != DG_OK)
        return;
    if (rec.ic_card[0]) {
        const char *const opts[] = { _("重新录入"), _("解绑") };
        dg_popup_choice_ex(_("IC卡"), opts, 2, 1u << 1, apply_ic_pick,
                           NULL, NULL);
    } else {
        apply_ic_pick(NULL, 0);
    }
}

/* 保存(右上按钮 / 「保存退出」共用):ADD 建用户;EDIT 提交草稿。
 * 返回 true = 成功(dg_edit_nav 据此执行「保存退出」的返回) */
static bool save(void)
{
    if (s_add_mode) {
        if (!s_pending_has_pwd) {
            dg_popup_fail(_("请先设置密码"), 1500, NULL, NULL);
            return false;
        }
        int rc = enroll_service_user_save(s_uid, s_pending_name,
                                          DG_ROLE_NORMAL, s_pending_pwd);
        if (rc != DG_OK) {
            dg_popup_fail(err_text(rc), 2000, NULL, NULL);
            return false;
        }
        /* 建好即转编辑模式:人脸/指纹/IC 从这里开始可录 */
        s_add_mode = false;
        memset(s_pending_pwd, 0, sizeof(s_pending_pwd));
        dg_popup_success(_("已保存"), 800, NULL, NULL);
        refresh();
        return true;
    }

    /* EDIT:草稿校验 → 存在性前置(user_save 对不存在的 uid 会走建用户
     * 分支,编辑页语义必须报「用户不存在」)→ 字段+密码一次落库 → 特征草稿提交 */
    if (dg_ui_valid_name(s_draft_name)) {
        dg_popup_fail(_("姓名不合法"), 1500, NULL, NULL);
        return false;
    }
    user_rec_t rec;
    if (enroll_service_user_get(s_uid, &rec) != DG_OK) {
        dg_popup_fail(_("用户不存在"), 1500, NULL, NULL);
        return false;
    }
    int rc = enroll_service_user_save(s_uid, s_draft_name, s_draft_role,
                                      s_draft_pwd[0] ? s_draft_pwd : NULL);
    if (rc != DG_OK) {
        dg_popup_fail(err_text(rc), 2000, NULL, NULL);
        return false;
    }
    s_draft_pwd[0] = '\0';
    s_dirty = false;                    /* 字段部分已保存;dirty 只剩特征草稿 */

    /* 特征草稿:有人脸草稿 = commit(草稿覆盖清除语义);否则清除标志生效 */
    if (enroll_service_draft_active(s_uid)) {
        rc = enroll_service_commit_draft(s_uid);
        if (rc != DG_OK) {
            /* 查重冲突等:草稿保留,可重拍覆盖或返回时放弃 */
            dg_popup_fail(err_text(rc), 2000, NULL, NULL);
            refresh();
            return false;
        }
        dg_avatar_invalidate(s_uid);    /* DB 头像已换,缓存作废重解码 */
    } else if (s_face_clear_pending) {
        rc = enroll_service_clear_face(s_uid);
        if (rc != DG_OK) {
            dg_popup_fail(err_text(rc), 2000, NULL, NULL);
            refresh();
            return false;
        }
        dg_avatar_invalidate(s_uid);
    }
    s_face_clear_pending = false;
    dg_popup_success(_("已保存"), 800, NULL, NULL);
    refresh();
    return true;
}

static void apply_del(void *ud, int idx)
{
    (void)ud;
    if (idx != 0)
        return;
    dg_avatar_invalidate(s_uid);        /* 用户即删:头像缓存同步作废 */
    /* DG_ENROLL_DELETE:经 enroll 服务,DB+视觉特征库一起删 */
    bridge_enroll_request(s_uid, DG_ENROLL_DELETE);   /* C4:出站统一经桥 */
    navigator_back();
}

static void on_del(lv_event_t *e)
{
    (void)e;
    const char *const opts[] = { _("删除") };
    dg_popup_choice_ex(_("确认删除该用户"), opts, 1, 1u << 0, apply_del,
                       NULL, NULL);      /* 删除=红色选项(破坏性) */
}

/* ---- 统一编辑导航(左上返回 + 右上保存 + 未保存退出确认) ----
 * dirty 全覆盖(2026-09-27):字段草稿、ADD 待建内容的姓名/密码、
 * 服务内人脸草稿、人脸清除标志——任何一种存在,返回都询问 */

static bool nav_is_dirty(void)
{
    if (s_dirty)
        return true;
    if (s_add_mode && (s_pending_name[0] || s_pending_has_pwd))
        return true;                     /* ADD:填了东西还没建用户 */
    if (enroll_service_draft_active(s_uid))
        return true;                     /* 人脸已拍未保存 */
    return s_face_clear_pending;
}

/* 「直接退出」:丢弃全部未保存草稿(字段草稿随页面静态清零);
 * 指纹/IC 录入流一并撤销(未落库模板由 provider 回滚) */
static void nav_discard(void)
{
    cancel_bio_flows();
    enroll_service_discard_draft(s_uid);
    s_face_clear_pending = false;
    s_dirty = false;
    s_pending_name[0] = '\0';
    s_pending_has_pwd = false;
    memset(s_pending_pwd, 0, sizeof(s_pending_pwd));
}

static const dg_edit_nav_ops_t s_nav_ops = {
    .is_dirty = nav_is_dirty,
    .save = save,
    .discard = nav_discard,
};

/* ---- 录入结果回执(桥转发,UI 线程;人脸录入的回执由拍摄页处理,这里只管清除/删除) ---- */

/* 指纹录入中间提示(两次按压;文案映射 progress.step) */
static void show_fp_progress(int32_t step)
{
    const char *text = _("请再次按压同一手指");
    if (step == DG_ENROLL_FP_STEP_PRESS1)
        text = _("请按压指纹");
    else if (step == DG_ENROLL_FP_STEP_RETRY2)
        text = _("两次按压指纹不一致，请用同一手指");
    dg_popup_fail(text, 2500, NULL, NULL);   /* 中性提示走醒目样式,自动消失 */
}

static void on_evt(const ui_evt_t *evt)
{
    if (evt->kind == UI_EVT_ENROLL_PROGRESS) {
        if (strcmp(evt->progress.user_id, s_uid) != 0 ||
            evt->progress.kind != DG_ENROLL_FINGER)
            return;
        show_fp_progress(evt->progress.step);
        return;
    }
    if (evt->kind != UI_EVT_ENROLL_RESULT)
        return;
    if (strcmp(evt->enroll.user_id, s_uid) != 0)
        return;
    const int err = evt->enroll.err;

    switch (evt->enroll.kind) {
    case DG_ENROLL_FINGER:
        s_fp_enrolling = false;          /* 终态:成功/失败都收流 */
        if (err == DG_OK)
            dg_popup_success(_("指纹已录入"), 1200, NULL, NULL);
        else if (err != DG_ERR_DUP_FINGER)
            dg_popup_fail(err_text(err), 2000, NULL, NULL);
        /* DUP_FINGER 文案更具体:直接红字提示重复 */
        if (err == DG_ERR_DUP_FINGER)
            dg_popup_fail(_("该指纹已绑定其他用户"), 2000, NULL, NULL);
        break;
    case DG_ENROLL_FINGER_DEL:
        if (err == DG_OK)
            dg_popup_success(_("已删除"), 800, NULL, NULL);
        else
            dg_popup_fail(err_text(err), 2000, NULL, NULL);
        break;
    case DG_ENROLL_IC:
        s_ic_enrolling = false;
        if (err == DG_OK)
            dg_popup_success(_("已绑定"), 1200, NULL, NULL);
        else
            dg_popup_fail(err == DG_ERR_DUP_IC ? _("该卡已绑定其他用户")
                                               : err_text(err), 2000, NULL, NULL);
        break;
    case DG_ENROLL_IC_CLEAR:
        if (err == DG_OK)
            dg_popup_success(_("已解绑"), 800, NULL, NULL);
        else
            dg_popup_fail(err_text(err), 2000, NULL, NULL);
        break;
    case DG_ENROLL_FACE_CLEAR:
        if (err == DG_OK) {
            dg_avatar_invalidate(s_uid); /* 头像随人脸清除,缓存同步作废 */
            dg_popup_success(_("已清除"), 800, NULL, NULL);
        }
        break;
    default:
        break;                           /* FACE/DELETE 由拍摄页/确认弹窗处理 */
    }
    refresh();
}

void page_user_edit_evt(const ui_evt_t *evt)
{
    if (evt->kind != UI_EVT_ENROLL_RESULT && evt->kind != UI_EVT_ENROLL_PROGRESS)
        return;
    on_evt(evt);
}

/* ---- 页面装配 ---- */

static lv_obj_t *page_create_(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, DG_COL_TEXT(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 20);
    return lbl;
}

void page_user_edit_create(lv_obj_t *parent)
{
    s_title = page_create_(parent, s_add_mode ? _("添加用户") : _("用户编辑"));

    /* 统一编辑导航:左上返回(带未保存确认)+ 右上保存 */

    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, DG_SCREEN_W, DG_SCREEN_H - 100);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 10, 0);
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
    s_img_face = lv_image_create(face_row);
    /* 预览窗 96×96 + CONTAIN:内容按窗口等比内缩居中,任何尺寸头像都
     * 铺满格心(旧实现控件随源 160×160 自适应 + 手工 scale,内容缩到
     * 96 却锚在控件左上,包围盒溢出行底且「填不满」)。右移量按新宽度
     * 同步收窄,与「修改」按钮保持原间距 */
    lv_obj_set_size(s_img_face, 96, 96);
    lv_image_set_inner_align(s_img_face, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(s_img_face, LV_ALIGN_RIGHT_MID, -282, 0);
    lv_obj_add_flag(s_img_face, LV_OBJ_FLAG_HIDDEN);

    row = row_create(col, _("指纹"), &s_val_finger, NULL);
    s_btn_finger = row_action_btn_label(row, _("录入"), on_finger);

    row = row_create(col, _("IC卡"), &s_val_ic, NULL);
    s_btn_ic = row_action_btn_label(row, _("录入"), on_ic);

    /* 删除用户:编辑列表最下一项(仅 EDIT;ADD 无此选项)。删除类操作=红 */
    s_row_del = dg_btn_create_danger(col, LV_SYMBOL_TRASH, _("删除用户"));
    lv_obj_set_size(s_row_del, DG_SCREEN_W - 2 * DG_PAD, 88);
    lv_obj_add_event_cb(s_row_del, on_del, LV_EVENT_CLICKED, NULL);

    dg_edit_nav_create(parent, &s_nav_ops, true);

    refresh();
}

void page_user_edit_destroy(void)
{
    s_title = s_val_name = s_val_role = s_val_pwd = NULL;
    s_val_face = s_val_finger = s_val_ic = NULL;
    s_img_face = NULL;
    s_btn_pwd = s_btn_face = NULL;
    s_btn_finger = s_btn_ic = NULL;
    s_row_del = NULL;
    cancel_bio_flows();                  /* 页面销毁:录入流不悬挂 */
    memset(s_pending_pwd, 0, sizeof(s_pending_pwd));
    memset(s_draft_pwd, 0, sizeof(s_draft_pwd));   /* 页面销毁即擦:明文不留静态区 */
}

void page_user_edit_open(const char *uid)
{
    /* 换编辑对象:上一位用户的遗留人脸草稿永远等不到保存,当场作废
     * (同 uid 重进则保留草稿,预览与「保存」依然可用) */
    if (s_uid[0] && (!uid || strcmp(uid, s_uid) != 0))
        enroll_service_discard_draft(s_uid);

    s_pending_name[0] = '\0';
    s_pending_has_pwd = false;
    memset(s_pending_pwd, 0, sizeof(s_pending_pwd));
    s_face_clear_pending = false;

    /* 按“用户是否存在”自判模式:存在 = 编辑;不存在 = 添加(ID 为待创建)。
     * 列表页因此不需要知道模式语义——传入 ID 进来就是同一个入口。 */
    user_rec_t rec;
    if (uid && uid[0] && enroll_service_user_get(uid, &rec) == DG_OK) {
        s_add_mode = false;
        snprintf(s_uid, sizeof(s_uid), "%s", uid);
        /* 草稿 = 当前库值;保存才落库 */
        snprintf(s_draft_name, sizeof(s_draft_name), "%s", rec.user_name);
        s_draft_role = rec.role;
        s_draft_pwd[0] = '\0';
    } else {
        s_add_mode = true;
        snprintf(s_uid, sizeof(s_uid), "%s", uid ? uid : "");
        s_draft_name[0] = '\0';
        s_draft_role = DG_ROLE_NORMAL;
        s_draft_pwd[0] = '\0';
    }
    s_dirty = false;
}
