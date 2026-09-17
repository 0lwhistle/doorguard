/*
 * dg_kbd.h — 数字键盘(密码输入/ID 输入用)
 */
#ifndef DG_KBD_H
#define DG_KBD_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_key)(void *user_data, const char *sym);  /* 数字/'-'/'.' */
    void (*on_backspace)(void *user_data);
    void (*on_ok)(void *user_data);
    void *user_data;
} dg_kbd_ops_t;

/** 创建数字键盘(3×4:1-9、退格/0/OK) */
lv_obj_t *dg_kbd_create(lv_obj_t *parent, const dg_kbd_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* DG_KBD_H */
