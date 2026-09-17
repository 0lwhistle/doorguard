/*
 * dg_popup.h — 统一弹窗(spec-ui §1:success/fail/input/choice)
 *
 * 纪律(spec-auth-business §5):任意时刻只有一个弹窗;新弹窗自动关闭旧的。
 * success/fail 自带自动关闭定时;回调在关闭时触发(UI 层消费)。
 */
#ifndef DG_POPUP_H
#define DG_POPUP_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 成功弹窗(绿,timeout_ms 后自动关闭;cb 在关闭时调用,可为 NULL) */
void dg_popup_success(const char *text, uint32_t timeout_ms, void (*on_close)(void *), void *ud);

/** 失败弹窗(红,同上) */
void dg_popup_fail(const char *text, uint32_t timeout_ms, void (*on_close)(void *), void *ud);

/** 输入弹窗(标题 + textarea + 数字键盘;确认回调携输入文本) */
void dg_popup_input(const char *title, bool mask_text,
                    void (*on_confirm)(void *ud, const char *text),
                    void (*on_cancel)(void *ud), void *ud);

/** 选择弹窗(标题 + 横排选项;on_pick 携选项下标) */
void dg_popup_choice(const char *title, const char *const *options, int cnt,
                     void (*on_pick)(void *ud, int idx),
                     void (*on_cancel)(void *ud), void *ud);

/** 关闭当前弹窗(若在) */
void dg_popup_close(void);

/** 当前是否有弹窗(spec-auth-business:popup_active 标志) */
bool dg_popup_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_POPUP_H */
