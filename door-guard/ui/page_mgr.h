/*
 * page_mgr.h — 页面管理器(spec-ui §4:栈式导航,深度固定)
 */
#ifndef DG_PAGE_MGR_H
#define DG_PAGE_MGR_H

#include "err.h"
#include "lvgl.h"
#include "ui_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;                    /**< 页面标识(如 "home"/"menu") */
    void (*create)(lv_obj_t *parent);    /**< 进入页面:在 parent 上构建对象树 */
    void (*destroy)(void);               /**< 离开页面:清理非 LVGL 资源(定时器/订阅) */
    /* 总线事件分发(LVGL 线程,仅当本页是当前页时被调用;可空)。
     * GOTO_PAGE 不走这里——切页由 ui 层直接驱动 page_mgr_open */
    void (*on_evt)(const ui_evt_t *evt);
} dg_page_ops_t;

/** 绑定页面根容器(ui 初始化时调用一次) */
int page_mgr_attach_root(lv_obj_t *root);

/** 注册页面(启动期一次性;同名重复注册返回 DG_ERR_PARAM) */
int page_mgr_register(const dg_page_ops_t *ops);

/** 打开页面(当前页先销毁;同名页面重复打开为幂等 no-op) */
int page_mgr_open(const char *name);

/** 返回上一页(栈底为根页面,不再回退) */
void page_mgr_back(void);

const char *page_mgr_current(void);

/** 把非切页事件分发给当前页(事件泵在 ui 层;当前页未注册 on_evt 则丢弃) */
void page_mgr_dispatch_evt(const ui_evt_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* DG_PAGE_MGR_H */
