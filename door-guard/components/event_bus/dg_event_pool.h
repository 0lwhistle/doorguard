/*
 * dg_event_pool.h — 事件总线专用定长块内存池
 *
 * 为什么存在:模板 event_bus 依赖其 mem_pool 模块;门禁移植只此一处用池,
 * 引入整个通用池不划算,按同语义自实现(定长块 + 空闲链 + 池满由调用方
 * 退化堆分配)。语义关键点与模板一致:块从空闲链取/还时链指针写在块首,
 * 块内标志不可靠,归属判定必须按地址范围(pool_contains)。
 */
#ifndef DG_EVENT_POOL_H
#define DG_EVENT_POOL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dg_event_pool dg_event_pool_t;

/** 创建池;失败返回 NULL(分配失败或参数非法) */
dg_event_pool_t *dg_event_pool_create(size_t block_size, int blocks);

/** 取一块;池满返回 NULL(调用方决定是否退化堆分配) */
void *dg_event_pool_alloc(dg_event_pool_t *pool);

/** 归还块;非本池地址时静默忽略(归属判定由调用方先做 contains) */
void dg_event_pool_free(dg_event_pool_t *pool, void *ptr);

/** 地址范围归属判定(池/堆混用时的唯一可靠归属手段,见 .c 注释) */
bool dg_event_pool_contains(const dg_event_pool_t *pool, const void *ptr);

/** 当前在用块数(诊断用;需锁内部互斥量,故不带 const) */
int dg_event_pool_in_use(dg_event_pool_t *pool);

void dg_event_pool_destroy(dg_event_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* DG_EVENT_POOL_H */
