/*
 * presenter_home.c — 主页展示器:内容事件渲染 + 弹窗文案 + 语言刷新
 *
 * 数据流:后端事件 → bridge(入队) → ui.c 泵(LVGL 线程)→ 本文件 on_evt
 * → page_home_set_* 注入视图。结果弹窗文案需要「最近一次认证结果」,
 * 故 s_last_result 留在本层(视图不持业务状态)。
 */
#include "presenter_home.h"
#include "pages/page_home.h"
#include "navigator/navigator.h"
#include "bridge/bridge.h"
#include "i18n.h"
#include "ui_events.h"
#include "widgets/dg_popup.h"
#include "dg_log.h"

#include <stdio.h>
#include <string.h>

static ev_auth_result_t s_last_result;   /* 最近认证结果(弹窗文案源) */

static void home_on_evt(const ui_evt_t *evt)
{
    switch (evt->kind) {
    case UI_EVT_FACE_BOX:
        page_home_set_facebox(evt->box.state, evt->box.x, evt->box.y,
                              evt->box.w, evt->box.h);
        break;
    case UI_EVT_FACE_LOST:
        page_home_clear_facebox();
        break;
    case UI_EVT_AUTH_RESULT:
        s_last_result = evt->result;
        break;
    case UI_EVT_HINT: {
        const ev_hint_t *h = &evt->hint;
        if (h->method == -5) {
            navigator_reload();              /* 整页重建(语言刷新) */
            break;
        }
        if (h->method == -3) {
            char text[DG_UID_LEN + DG_NAME_LEN + 8];
            snprintf(text, sizeof(text), "%s %s", _("验证成功"),
                     s_last_result.has_user ? s_last_result.user_name : "");
            dg_popup_success(text, 3000, NULL, NULL);
            break;
        }
        if (h->method == -4) {
            dg_popup_fail(_("验证失败"), 3000, NULL, NULL);
            break;
        }
        const char *text = NULL;
        if (h->method == -1)
            text = _("管理员认证");
        else if (h->method == DG_METHOD_FACE_11)
            text = _("请正对摄像头");
        else if (h->method == DG_METHOD_FINGER)
            text = _("请按指纹");
        else if (h->method == DG_METHOD_PWD)
            text = _("请输入密码");
        else if (h->method == DG_METHOD_IC)
            text = _("请刷卡");
        page_home_set_hint(text);
        break;
    }
    case UI_EVT_GOTO_PAGE:
        break;                               /* 切页由 ui 层泵直驱 */
    }
}

void presenter_home_register(void)
{
    static const navigator_page_t ops = {
        .name = "home",
        .create = page_home_create,
        .destroy = page_home_destroy,
        .on_evt = home_on_evt,
    };
    navigator_register(&ops);
    memset(&s_last_result, 0, sizeof(s_last_result));
}
