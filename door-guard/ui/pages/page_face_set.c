/*
 * page_face_set.c — 人脸识别设置页(2026-09-27 用户需求:阈值上屏可调)
 *
 * 三个阈值滑条(检测/识别/活体),显示值 = 配置值 ×100(0.42 显示 42)。
 * 拖动实时刷新数值,松手 cfg_set_dbl 落盘(500ms 防抖原子写盘)——
 * 视觉 worker 每拍都读 cfg 快照,松手即生效,无需重启。
 * 范围沿用 cfg META 的钳制区间(检测 30~95,识别 30~100,活体 0~100),
 * 越界由 cfg_set 兜底回退,UI 不必重复判。
 *
 * 分层:纯视图 + 页内小表驱动;无异步事件,不需要 presenter on_evt。
 */
#include "page_face_set.h"
#include "cfg.h"
#include "dg_log.h"
#include "err.h"
#include "i18n.h"
#include "navigator/navigator.h"
#include "theme.h"
#include "widgets/dg_btn.h"
#include "widgets/dg_popup.h"

#include <stdio.h>

/* 每行一个滑条:标题(左)/当前值(右)/滑条/一句说明 */
typedef struct {
    const char *title;                 /* _() 键 */
    const char *hint;                  /* _() 键 */
    const char *cfg_key;               /* cfg META 键名 */
    int min, max;                      /* 滑条范围(=阈值×100) */
    double (*get)(const dg_cfg_t *c);  /* 当前值读取(cfg 字段映射) */
} face_row_t;

static double get_det(const dg_cfg_t *c) { return c->face_det_threshold; }
static double get_match(const dg_cfg_t *c) { return c->face_match_threshold; }
static double get_antispoof(const dg_cfg_t *c) { return c->antispoof_threshold; }

static const face_row_t ROWS[] = {
    { "人脸检测阈值", "调高:更难出框,误检少;调低:远处侧脸更易检出",
      "det_threshold", 30, 95, get_det },
    { "人脸识别阈值", "调高:防误认他人;调低:更易命中,需防认错",
      "face_match_threshold", 30, 100, get_match },
    { "活体检测阈值", "真脸分数低于此值将要求二次验证;调高更严",
      "antispoof_threshold", 0, 100, get_antispoof },
};
#define ROW_N (int)(sizeof(ROWS) / sizeof(ROWS[0]))

static lv_obj_t *s_sliders[ROW_N];
static lv_obj_t *s_values[ROW_N];

static void on_back(lv_event_t *e)
{
    (void)e;
    navigator_back();
}

/* 松手才落盘:拖动过程只刷显示,cfg_set 的防抖落盘留给最终值 */
static void slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    /* 找到所属行(表小,线性即可) */
    int idx = -1;
    for (int i = 0; i < ROW_N; i++)
        if (s_sliders[i] == slider)
            idx = i;
    if (idx < 0)
        return;

    const int v = lv_slider_get_value(slider);
    if (code == LV_EVENT_VALUE_CHANGED) {
        char t[16];
        snprintf(t, sizeof(t), "%d", v);
        if (s_values[idx])
            lv_label_set_text(s_values[idx], t);
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (cfg_set_dbl(ROWS[idx].cfg_key, (double)v / 100.0) != DG_OK)
            dg_popup_fail(_("保存失败"), 1000, NULL, NULL);
    }
}

static void build_row(lv_obj_t *parent, int idx, int y)
{
    const face_row_t *r = &ROWS[idx];

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _(r->title));
    lv_obj_set_style_text_font(title, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, DG_PAD, y);

    lv_obj_t *val = lv_label_create(parent);
    lv_obj_set_style_text_font(val, DG_FONT_CN, 0);
    lv_obj_set_style_text_color(val, DG_COL_OK(), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -DG_PAD, y);
    s_values[idx] = val;

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, DG_SCREEN_W - 2 * DG_PAD, 56);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, DG_PAD, y + 44);
    lv_slider_set_range(slider, r->min, r->max);
    lv_slider_set_value(slider, (int32_t)(r->get(cfg_get()) * 100.0 + 0.5),
                        LV_ANIM_OFF);
    /* 蓝白主题:已填充段用主色,旋钮加大触摸区 */
    lv_obj_set_style_bg_color(slider, DG_COL_SCRIM(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, DG_COL_OK(), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_RELEASED, NULL);
    s_sliders[idx] = slider;

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, _(r->hint));
    lv_obj_set_style_text_font(hint, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(hint, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);   /* 次要文字:正文色降透明(page_users 同款) */
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, DG_PAD, y + 108);

    /* 初始值文本(与滑条一致) */
    char t[16];
    snprintf(t, sizeof(t), "%d", (int)(r->get(cfg_get()) * 100.0 + 0.5));
    lv_label_set_text(val, t);
}

void page_face_set_create(lv_obj_t *parent)
{
    DG_LOGI("[FACE_SET]", "page create");
    lv_obj_set_style_bg_color(parent, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, _("人脸识别设置"));
    lv_obj_set_style_text_font(title, DG_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, DG_COL_TEXT(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *back = dg_btn_create_light(parent, LV_SYMBOL_LEFT, _("返回"));
    lv_obj_set_size(back, 150, 64);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, DG_PAD, 16);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* 行距 = 标题 44 + 滑条 56 + 说明 + 空隙;1280 高屏三行绰绰有余 */
    for (int i = 0; i < ROW_N; i++)
        build_row(parent, i, 140 + i * 210);

    lv_obj_t *tip = lv_label_create(parent);
    lv_label_set_text(tip, _("松手即保存并生效"));
    lv_obj_set_style_text_font(tip, DG_FONT_SUB, 0);
    lv_obj_set_style_text_color(tip, DG_COL_TEXT(), 0);
    lv_obj_set_style_text_opa(tip, LV_OPA_70, 0);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 140 + ROW_N * 210);
}

void page_face_set_destroy(void)
{
    for (int i = 0; i < ROW_N; i++) {
        s_sliders[i] = NULL;
        s_values[i] = NULL;
    }
    DG_LOGI("[FACE_SET]", "page destroy");
}
