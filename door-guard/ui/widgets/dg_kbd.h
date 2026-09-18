/*
 * dg_kbd.h — 屏幕内键盘(数字 + 字母两页;触摸屏唯一输入手段)
 *
 * 布局与交互(详见 spec-ui §6):
 *   数字页 1-9 / ⌫ / 0 / OK(门禁 PIN 常用,默认页)
 *   字母页 QWERTY(⇧ 切大小写)+ 空格 + ⌫ + OK
 *   页脚 "ABC"/"123" 键在两页间切换;状态记在键盘实例内,弹窗关闭即复位
 * 无物理键盘的设备:用户 ID/姓名/密码都靠它输入;中文需输入法,本设备不支持
 * (需要中文姓名请走上位机)。
 */
#ifndef DG_KBD_H
#define DG_KBD_H

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_key)(void *user_data, const char *sym);  /* 数字/字母/空格 " " */
    void (*on_backspace)(void *user_data);
    void (*on_ok)(void *user_data);
    void *user_data;
} dg_kbd_ops_t;

/**
 * 创建键盘(数字 + 字母两页,含页脚切换键)。
 * @param start_alpha true=初始停在字母页(姓名),false=数字页(ID/密码)
 * @return 键盘根对象(随父对象销毁)
 */
lv_obj_t *dg_kbd_create(lv_obj_t *parent, bool start_alpha, const dg_kbd_ops_t *ops);

/** 当前是否字母页(测试/调试) */
bool dg_kbd_is_alpha(lv_obj_t *kbd);

/** 切页(等价于点页脚 ABC/123) */
void dg_kbd_toggle_layout(lv_obj_t *kbd);

#ifdef __cplusplus
}
#endif

#endif /* DG_KBD_H */
