/*
 * web_session.c — 会话表实现
 *
 * 表结构:定长 8 槽(门禁上位机同时在线的运维人员不会多;定长=无分配、
 * 无泄漏)。顶替策略:优先空槽,否则顶掉"过期最早的槽",保证新登录总能
 * 拿到可用 token——原实现表满时仍返回 token 但没入库,客户端拿到一个
 * 永远无效的 token(实测的坑),这里从设计上排除。
 */
#include "web_session.h"

#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char   token[WEB_TOKEN_LEN + 1];
    time_t expiry;
    bool   used;
} session_t;

static session_t s_slots[WEB_SESSION_MAX];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool slot_valid(const session_t *s, time_t now)
{
    return s->used && s->expiry > now;
}

int web_session_create(char *out, size_t cap, int *expires_in_s)
{
    if (!out || cap < WEB_TOKEN_LEN + 1)
        return DG_ERR_PARAM;

    uint8_t rnd[WEB_TOKEN_LEN / 2];
    if (RAND_bytes(rnd, sizeof(rnd)) != 1)
        return DG_ERR_INTERNAL;
    char token[WEB_TOKEN_LEN + 1];
    for (size_t i = 0; i < sizeof(rnd); i++)
        snprintf(token + i * 2, 3, "%02x", rnd[i]);

    time_t now = time(NULL);
    pthread_mutex_lock(&s_mtx);
    session_t *slot = NULL;
    for (int i = 0; i < WEB_SESSION_MAX; i++) {
        if (!slot_valid(&s_slots[i], now)) {     /* 空槽或已过期 */
            slot = &s_slots[i];
            break;
        }
    }
    if (!slot) {                                 /* 表满:顶掉最早过期的 */
        slot = &s_slots[0];
        for (int i = 1; i < WEB_SESSION_MAX; i++) {
            if (s_slots[i].expiry < slot->expiry)
                slot = &s_slots[i];
        }
    }
    snprintf(slot->token, sizeof(slot->token), "%s", token);
    slot->expiry = now + WEB_SESSION_TTL_S;
    slot->used = true;
    pthread_mutex_unlock(&s_mtx);

    snprintf(out, cap, "%s", token);
    if (expires_in_s)
        *expires_in_s = WEB_SESSION_TTL_S;
    return DG_OK;
}

bool web_session_validate(const char *token, time_t now)
{
    if (!token || !token[0])
        return false;
    bool ok = false;
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < WEB_SESSION_MAX; i++) {
        if (slot_valid(&s_slots[i], now) && strcmp(s_slots[i].token, token) == 0) {
            s_slots[i].expiry = now + WEB_SESSION_TTL_S;   /* 滑动续期 */
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&s_mtx);
    return ok;
}

void web_session_revoke(const char *token)
{
    pthread_mutex_lock(&s_mtx);
    if (!token) {
        memset(s_slots, 0, sizeof(s_slots));
    } else {
        for (int i = 0; i < WEB_SESSION_MAX; i++) {
            if (s_slots[i].used && strcmp(s_slots[i].token, token) == 0)
                memset(&s_slots[i], 0, sizeof(s_slots[i]));
        }
    }
    pthread_mutex_unlock(&s_mtx);
}

void web_session_revoke_all(void)
{
    web_session_revoke(NULL);
}

int web_session_count(time_t now)
{
    int n = 0;
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < WEB_SESSION_MAX; i++) {
        if (slot_valid(&s_slots[i], now))
            n++;
    }
    pthread_mutex_unlock(&s_mtx);
    return n;
}
