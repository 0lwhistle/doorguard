/*
 * web_session.c — 会话表实现
 *
 * 单会话策略(2026-09-22):web 管理页同一时刻只允许一个管理员在线,
 * create 签发成功即清除其余全部会话——新登录必胜,自愈性好:崩溃的
 * 浏览器不会永久占坑,被踢方下一个请求拿 401 由前端路由守卫送回登录页。
 * 定长 8 槽结构保留(单会话下最多占 1 槽,容量只是防御性上限)。
 * 原实现"表满顶掉最旧"的换路已被单会话清空覆盖,不存在"token 未入库"
 * 的历史坑。
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
    /* 单会话:签发即清空其余会话(见文件头说明) */
    memset(s_slots, 0, sizeof(s_slots));
    session_t *slot = &s_slots[0];
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
