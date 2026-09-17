#include "ui_events.h"

#include <pthread.h>
#include <string.h>

#define UI_EVT_RING 32

static ui_evt_t s_ring[UI_EVT_RING];
static volatile int s_head = 0, s_tail = 0;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

void ui_evt_push(const ui_evt_t *e)
{
    if (!e)
        return;
    pthread_mutex_lock(&s_mtx);
    int next = (s_head + 1) % UI_EVT_RING;
    if (next != s_tail) {                   /* 满则丢弃最旧不可取,直接丢新 */
        s_ring[s_head] = *e;
        s_head = next;
    }
    pthread_mutex_unlock(&s_mtx);
}

int ui_evt_pop(ui_evt_t *out)
{
    pthread_mutex_lock(&s_mtx);
    if (s_tail == s_head) {
        pthread_mutex_unlock(&s_mtx);
        return 0;
    }
    *out = s_ring[s_tail];
    s_tail = (s_tail + 1) % UI_EVT_RING;
    pthread_mutex_unlock(&s_mtx);
    return 1;
}
