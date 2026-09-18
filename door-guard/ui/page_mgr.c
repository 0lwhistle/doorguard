/*
 * page_mgr.c — 页面管理器实现
 *
 * 页面整建整删(LVGL 内存池有限):每页一个根容器,离页即销毁。
 * 返回上一页时页面重建(无状态保留);需要跨页状态(如查询分页)经
 * 事件/服务层传递,不藏在页面对象里。栈深固定,超深拒绝,不做复杂导航。
 */
#include "page_mgr.h"
#include "theme.h"
#include "dg_log.h"

#include <string.h>

#define DG_PAGE_STACK_MAX 4
#define DG_PAGE_MAX 16

static const dg_page_ops_t *s_pages[DG_PAGE_MAX];
static int s_page_cnt = 0;

static const dg_page_ops_t *s_stack[DG_PAGE_STACK_MAX];
static int s_depth = 0;

static lv_obj_t *s_root = NULL;

int page_mgr_attach_root(lv_obj_t *root)
{
    if (!root)
        return DG_ERR_PARAM;
    s_root = root;
    return DG_OK;
}

int page_mgr_register(const dg_page_ops_t *ops)
{
    if (!ops || !ops->name || !ops->create)
        return DG_ERR_PARAM;
    for (int i = 0; i < s_page_cnt; i++) {
        if (!strcmp(s_pages[i]->name, ops->name)) {
            DG_LOGW("[PAGE]", "页面 %s 重复注册", ops->name);
            return DG_ERR_PARAM;
        }
    }
    if (s_page_cnt >= DG_PAGE_MAX)
        return DG_ERR_NO_MEMORY;
    s_pages[s_page_cnt++] = ops;
    return DG_OK;
}

static const dg_page_ops_t *find_page(const char *name)
{
    for (int i = 0; i < s_page_cnt; i++) {
        if (!strcmp(s_pages[i]->name, name))
            return s_pages[i];
    }
    return NULL;
}

/* 销毁当前页(LVGL 子树 + destroy 回调) */
static void destroy_current_locked(void)
{
    if (s_depth == 0)
        return;
    lv_obj_t *cur = lv_obj_get_child(s_root, 0);
    if (cur)
        lv_obj_del(cur);
    if (s_stack[s_depth - 1]->destroy)
        s_stack[s_depth - 1]->destroy();
}

/* 为 ops 构建新页容器并调用 create */
static void create_page_locked(const dg_page_ops_t *ops)
{
    lv_obj_t *page = lv_obj_create(s_root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, DG_SCREEN_W, DG_SCREEN_H);
    ops->create(page);
    DG_LOGI("[PAGE]", "open %s (depth=%d)", ops->name, s_depth);
}

int page_mgr_open(const char *name)
{
    const dg_page_ops_t *ops = find_page(name);
    if (!ops) {
        DG_LOGE("[PAGE]", "页面 %s 未注册", name);
        return DG_ERR_NOT_FOUND;
    }
    if (s_depth > 0 && !strcmp(s_stack[s_depth - 1]->name, name))
        return DG_OK;                    /* 幂等 */

    /* 目标已在栈中(如待机唤醒回栈底 home):回退而非再压——
     * 纯压栈会让 home/standby 反复交替撑爆页面栈(实测死锁路径之一) */
    for (int i = s_depth - 2; i >= 0; i--) {
        if (strcmp(s_stack[i]->name, name))
            continue;
        destroy_current_locked();
        s_depth = i + 1;
        create_page_locked(ops);
        return DG_OK;
    }

    if (s_depth >= DG_PAGE_STACK_MAX) {
        DG_LOGE("[PAGE]", "页面栈已满(%d),拒绝打开 %s", DG_PAGE_STACK_MAX, name);
        return DG_ERR_STATE;
    }

    destroy_current_locked();
    s_stack[s_depth++] = ops;
    create_page_locked(ops);
    return DG_OK;
}

void page_mgr_back(void)
{
    if (s_depth <= 1)
        return;                          /* 根页面不回退 */
    destroy_current_locked();
    s_depth--;
    create_page_locked(s_stack[s_depth - 1]);
}

const char *page_mgr_current(void)
{
    return s_depth > 0 ? s_stack[s_depth - 1]->name : NULL;
}

void page_mgr_dispatch_evt(const ui_evt_t *evt)
{
    if (s_depth > 0 && s_stack[s_depth - 1]->on_evt)
        s_stack[s_depth - 1]->on_evt(evt);
}
