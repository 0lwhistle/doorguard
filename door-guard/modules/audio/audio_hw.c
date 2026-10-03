/*
 * audio_hw.c — 后端选择/注入实现(接口见 audio_hw.h)
 */
#include "audio_hw.h"

#include <pthread.h>

/* 注入态跨测试与播放器线程,读写各一次,互斥够用(轻路径不值得原子化) */
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static const audio_hw_ops_t *s_attached;

#if defined(DG_AUDIO_ALSA) && DG_AUDIO_ALSA
extern const audio_hw_ops_t audio_hw_alsa_ops;   /* audio_hw_alsa.c */
#endif

const audio_hw_ops_t *audio_hw_board(void)
{
#if defined(DG_AUDIO_ALSA) && DG_AUDIO_ALSA
    return &audio_hw_alsa_ops;
#else
    return NULL;          /* 宿主构建:测试注入 sink,不注入则播放器降级 */
#endif
}

void audio_hw_attach(const audio_hw_ops_t *ops)
{
    pthread_mutex_lock(&s_mtx);
    s_attached = ops;
    pthread_mutex_unlock(&s_mtx);
}

const audio_hw_ops_t *audio_hw_current(void)
{
    pthread_mutex_lock(&s_mtx);
    const audio_hw_ops_t *ops = s_attached;
    pthread_mutex_unlock(&s_mtx);
    return ops ? ops : audio_hw_board();
}
