/*
 * navigator.h — 页面导航器(学自 ESP32 ovs 工程,契约同名)
 *
 * 职责:页面注册表 + 栈管理 + 生命周期(on_enter/on_exit)+ 事件分发。
 * 栈语义:
 *   push    压栈(menu→users,返回走 back)
 *   switch  平级/回退:目标在栈中则回退到它(home↔standby 反复切换不涨栈,
 *           实测纯压栈会撑爆页面栈),否则替换栈顶
 *   back    弹栈,重建栈顶
 *   reload  重建当前页(语言热切换)
 * 被「盖住」的页面对象即销毁(door-guard 单屏小内存策略),on_exit 在销毁前、
 * on_enter 在创建后回调(presenter 在此启停自己的定时器、拉数据)。
 */
#ifndef DG_NAVIGATOR_H
#define DG_NAVIGATOR_H

#include "lvgl.h"
#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_OK = 0,
    NAV_ERR_PARAM = -1,
    NAV_ERR_NOT_FOUND = -2,   /* name 未注册 */
    NAV_ERR_FULL = -3,        /* 注册表/栈满 */
    NAV_ERR_STATE = -4,
} nav_err_t;

typedef struct {
    const char *name;                          /* 唯一 id(静态存储期) */
    void (*create)(lv_obj_t *parent);          /* 构建视图(纯 pages 层) */
    void (*destroy)(void);                     /* 可选:页面静态句柄清理 */
    void (*on_enter)(void);                    /* 可选:显示后(presenter) */
    void (*on_exit)(void);                     /* 可选:盖住/销毁前(presenter) */
    void (*on_evt)(const ui_evt_t *evt);       /* 可选:内容事件(LVGL 线程) */
} navigator_page_t;

#define NAV_MAX_PAGES 10   /* home standby menu users user_edit capture device access_set logs web_set */
#define NAV_MAX_STACK 4

void navigator_init(lv_obj_t *root);
nav_err_t navigator_register(const navigator_page_t *page);
nav_err_t navigator_push(const char *name);
nav_err_t navigator_switch(const char *name);
void navigator_back(void);
nav_err_t navigator_reload(void);              /* 语言热切换 */
const char *navigator_current(void);
void navigator_dispatch_evt(const ui_evt_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* DG_NAVIGATOR_H */
