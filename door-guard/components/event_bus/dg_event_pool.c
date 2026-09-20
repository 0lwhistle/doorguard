/*
 * dg_event_pool 实现 — 见 dg_event_pool.h
 */
#include "dg_event_pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>

struct dg_event_pool {
    uint8_t *base;              /* 块区起点(对齐到 max_align_t) */
    size_t block_size;
    int blocks;
    int in_use;
    void *free_head;            /* 侵入式空闲链:链指针存在空闲块自身首部 */
    pthread_mutex_t mtx;
};

dg_event_pool_t *dg_event_pool_create(size_t block_size, int blocks)
{
    if (block_size == 0 || blocks <= 0)
        return NULL;

    dg_event_pool_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    /* 按最大对齐要求补齐块大小,保证每个块可存放任意基础类型结构 */
    const size_t align = sizeof(max_align_t);
    block_size = (block_size + align - 1) / align * align;

    p->base = calloc((size_t)blocks, block_size);
    if (!p->base) {
        free(p);
        return NULL;
    }
    p->block_size = block_size;
    p->blocks = blocks;

    /* 空闲链串起全部块 */
    for (int i = 0; i < blocks; i++) {
        void *blk = p->base + (size_t)i * block_size;
        *(void **)blk = p->free_head;
        p->free_head = blk;
    }

    pthread_mutex_init(&p->mtx, NULL);
    return p;
}

void *dg_event_pool_alloc(dg_event_pool_t *pool)
{
    if (!pool)
        return NULL;

    pthread_mutex_lock(&pool->mtx);
    void *blk = pool->free_head;
    if (blk) {
        pool->free_head = *(void **)blk;
        pool->in_use++;
    }
    pthread_mutex_unlock(&pool->mtx);
    return blk;
}

void dg_event_pool_free(dg_event_pool_t *pool, void *ptr)
{
    if (!pool || !ptr)
        return;

    pthread_mutex_lock(&pool->mtx);
    /* 上界保护:范围外指针直接忽略,防调用方误归还堆块破坏空闲链 */
    if (!dg_event_pool_contains(pool, ptr))
        goto out;

    *(void **)ptr = pool->free_head;
    pool->free_head = ptr;
    pool->in_use--;
out:
    pthread_mutex_unlock(&pool->mtx);
}

bool dg_event_pool_contains(const dg_event_pool_t *pool, const void *ptr)
{
    if (!pool || !ptr)
        return false;
    const uint8_t *p = (const uint8_t *)ptr;
    const uint8_t *end = pool->base + (size_t)pool->blocks * pool->block_size;
    return p >= pool->base && p < end
           && ((size_t)(p - pool->base) % pool->block_size) == 0;
}

int dg_event_pool_in_use(dg_event_pool_t *pool)
{
    if (!pool)
        return 0;
    pthread_mutex_lock(&pool->mtx);
    int n = pool->in_use;
    pthread_mutex_unlock(&pool->mtx);
    return n;
}

void dg_event_pool_destroy(dg_event_pool_t *pool)
{
    if (!pool)
        return;
    pthread_mutex_destroy(&pool->mtx);
    free(pool->base);
    free(pool);
}
