/*
 * navigator.c — 页面导航器实现(自 page_mgr 演进,学自 ESP32 ovs 工程)
 *
 * 页面整建整删(LVGL 内存池有限):每页一个根容器,被盖住即销毁,
 * 返回时重建(无对象级状态保留);跨页状态走事件/服务层。
 * 生命周期:on_exit 在页面销毁前、on_enter 在创建后——presenter 借此
 * 启停自己的定时器、拉取数据(ESP32 同款约定)。
 */
#include "navigator.h"
#include "theme.h"
#include "dg_log.h"

#include <string.h>

static const navigator_page_t *s_pages[NAV_MAX_PAGES];
static int s_page_cnt = 0;

static const navigator_page_t *s_stack[NAV_MAX_STACK];
static int s_depth = 0;

static lv_obj_t *s_root = NULL;

static const navigator_page_t *find_page(const char *name)
{
    for (int i = 0; i < s_page_cnt; i++) {
        if (!strcmp(s_pages[i]->name, name))
            return s_pages[i];
    }
    return NULL;
}

/* 销毁当前页:on_exit 先于 LVGL 子树删除 */
static void destroy_current(void)
{
    if (s_depth == 0)
        return;
    const navigator_page_t *cur = s_stack[s_depth - 1];
    if (cur->on_exit)
        cur->on_exit();
    lv_obj_t *obj = lv_obj_get_child(s_root, 0);
    if (obj)
        lv_obj_delete(obj);
    if (cur->destroy)
        cur->destroy();
}

/* 构建页容器 + create + on_enter */
static void create_page(const navigator_page_t *ops)
{
    lv_obj_t *page = lv_obj_create(s_root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, DG_SCREEN_W, DG_SCREEN_H);
    /* 容器默认不透明底(video-plane 改造,2026-09-22):remove_style_all
     * 后容器本透明,而 scr/display 底现在也是透明的——不给底,所有页面都
     * 会透出下层视频。要透的页面(主页/拍摄页)自行覆盖为 TRANSP */
    lv_obj_set_style_bg_color(page, DG_COL_BG(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    ops->create(page);
    /* 直接模式(2026-09-22)下新建容器自身的全屏失效不可依赖(实测待机页
     * 黑底不落 fb):建页末尾强制整页重绘一次,杜绝切页残影 */
    lv_obj_invalidate(page);
    if (ops->on_enter)
        ops->on_enter();
    DG_LOGI("[PAGE]", "open %s (depth=%d)", ops->name, s_depth);
}

void navigator_init(lv_obj_t *root)
{
    s_root = root;
}

nav_err_t navigator_register(const navigator_page_t *page)
{
    if (!page || !page->name || !page->create)
        return NAV_ERR_PARAM;
    for (int i = 0; i < s_page_cnt; i++) {
        if (!strcmp(s_pages[i]->name, page->name))
            return NAV_ERR_STATE;          /* 重复注册 */
    }
    if (s_page_cnt >= NAV_MAX_PAGES)
        return NAV_ERR_FULL;
    s_pages[s_page_cnt++] = page;
    return NAV_OK;
}

nav_err_t navigator_push(const char *name)
{
    const navigator_page_t *ops = find_page(name);
    if (!ops)
        return NAV_ERR_NOT_FOUND;
    if (s_depth >= NAV_MAX_STACK) {
        DG_LOGW("[PAGE]", "栈满,拒绝 push %s", name);
        return NAV_ERR_FULL;
    }
    destroy_current();
    s_stack[s_depth++] = ops;
    create_page(ops);
    return NAV_OK;
}

nav_err_t navigator_switch(const char *name)
{
    const navigator_page_t *ops = find_page(name);
    if (!ops)
        return NAV_ERR_NOT_FOUND;
    if (s_depth > 0 && !strcmp(s_stack[s_depth - 1]->name, name))
        return NAV_OK;                     /* 幂等 */

    /* 目标在栈中:回退到它(home↔standby 反复切换不涨栈,实测教训) */
    for (int i = s_depth - 2; i >= 0; i--) {
        if (strcmp(s_stack[i]->name, name))
            continue;
        destroy_current();
        s_depth = i + 1;
        create_page(ops);
        return NAV_OK;
    }

    /* 不在栈中:替换栈顶(平级切换,不加深) */
    destroy_current();
    s_stack[s_depth - 1] = ops;
    create_page(ops);
    return NAV_OK;
}

void navigator_back(void)
{
    if (s_depth <= 1)
        return;                            /* 根页面不回退 */
    destroy_current();
    s_depth--;
    create_page(s_stack[s_depth - 1]);
}

nav_err_t navigator_reload(void)
{
    if (s_depth == 0)
        return NAV_ERR_STATE;
    const navigator_page_t *cur = s_stack[s_depth - 1];
    if (cur->on_exit)
        cur->on_exit();
    lv_obj_t *obj = lv_obj_get_child(s_root, 0);
    if (obj)
        lv_obj_delete(obj);
    if (cur->destroy)
        cur->destroy();
    create_page(cur);
    return NAV_OK;
}

const char *navigator_current(void)
{
    return s_depth > 0 ? s_stack[s_depth - 1]->name : NULL;
}

void navigator_dispatch_evt(const ui_evt_t *evt)
{
    if (s_depth > 0 && s_stack[s_depth - 1]->on_evt)
        s_stack[s_depth - 1]->on_evt(evt);
}
