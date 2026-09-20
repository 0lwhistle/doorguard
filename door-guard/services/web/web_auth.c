/*
 * web_auth.c — 上位机凭据与登录风控实现
 *
 * 存储布局(device_config):
 *   web_user       账号名(明文,便于显示"当前账号";账号本身不是秘密)
 *   web_pwd_salt   16B 随机盐,hex(32 字符)
 *   web_pwd_hash   32B PBKDF2 输出,hex(64 字符)
 *   web_pwd_default "1" = 仍是出厂默认口令
 *
 * 锁定表:内存 8 槽 LRU(不落盘)——重启即解锁,避免把管理员永久锁在门外;
 * 门禁设备的威胁模型是"局域网内脚本爆破",重启解锁不会削弱防护。
 */
#include "web_auth.h"
#include "web_session.h"
#include "dg_log.h"
#include "storage.h"
#include "valid.h"
#include "modules/sqlite/crypto.h"

#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "[WEB.AUTH]";

#define DEFAULT_USER       "admin"
#define DEFAULT_PWD        "admin"
#define SALT_LEN           16
#define HASH_LEN           32
#define FAIL_LIMIT         5      /* 连错次数阈值 */
#define LOCK_SECONDS       60     /* 锁定时长 */

#define LOCK_SLOTS         8

typedef struct {
    char    ip[WEB_AUTH_IP_MAX];
    int     fails;
    time_t  locked_until;
    bool    used;
} lock_entry_t;

static lock_entry_t s_locks[LOCK_SLOTS];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- hex 工具 ---- */

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static int hex_decode(const char *in, uint8_t *out, size_t n)
{
    if (!in || strlen(in) != n * 2)
        return DG_ERR_PARAM;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(in + i * 2, "%2x", &v) != 1)
            return DG_ERR_PARAM;
        out[i] = (uint8_t)v;
    }
    return DG_OK;
}

/* ---- 凭据读写 ---- */

static int cred_load(char *user, size_t ucap, uint8_t *salt, uint8_t *hash,
                     bool *is_default)
{
    char salt_hex[SALT_LEN * 2 + 1], hash_hex[HASH_LEN * 2 + 1];
    if (db_config_get("web_user", user, ucap) != DG_OK ||
        db_config_get("web_pwd_salt", salt_hex, sizeof(salt_hex)) != DG_OK ||
        db_config_get("web_pwd_hash", hash_hex, sizeof(hash_hex)) != DG_OK)
        return DG_ERR_NOT_FOUND;
    if (hex_decode(salt_hex, salt, SALT_LEN) != DG_OK ||
        hex_decode(hash_hex, hash, HASH_LEN) != DG_OK)
        return DG_ERR_DB;
    char flag[8] = "";
    *is_default = (db_config_get("web_pwd_default", flag, sizeof(flag)) == DG_OK &&
                   strcmp(flag, "1") == 0);
    return DG_OK;
}

static int cred_store(const char *user, const char *pwd, bool is_default)
{
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    char salt_hex[SALT_LEN * 2 + 1], hash_hex[HASH_LEN * 2 + 1];
    if (RAND_bytes(salt, sizeof(salt)) != 1)
        return DG_ERR_INTERNAL;
    if (dg_pbkdf2_sha256(pwd, salt, sizeof(salt), DG_PBKDF2_ITERS, hash) != DG_OK)
        return DG_ERR_INTERNAL;
    hex_encode(salt, sizeof(salt), salt_hex);
    hex_encode(hash, sizeof(hash), hash_hex);
    if (db_config_set("web_user", user) != DG_OK ||
        db_config_set("web_pwd_salt", salt_hex) != DG_OK ||
        db_config_set("web_pwd_hash", hash_hex) != DG_OK ||
        db_config_set("web_pwd_default", is_default ? "1" : "0") != DG_OK)
        return DG_ERR_IO;
    return DG_OK;
}

int web_auth_ensure(void)
{
    char user[WEB_AUTH_USER_MAX];
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    bool def = false;
    if (cred_load(user, sizeof(user), salt, hash, &def) == DG_OK)
        return DG_OK;
    DG_LOGW(TAG, "未找到凭据,生成默认账号 %s(请在设备菜单或上位机改密)",
            DEFAULT_USER);
    return cred_store(DEFAULT_USER, DEFAULT_PWD, true);
}

int web_auth_verify(const char *user, const char *pwd)
{
    if (!user || !pwd)
        return DG_ERR_PARAM;

    char cur[WEB_AUTH_USER_MAX];
    uint8_t salt[SALT_LEN], expect[HASH_LEN], calc[HASH_LEN];
    bool def = false;
    if (cred_load(cur, sizeof(cur), salt, expect, &def) != DG_OK)
        return DG_ERR_NOT_INIT;

    /* 账号比对与口令比对都算"验证失败":对外统一措辞,不泄露账号是否存在 */
    if (strcmp(cur, user) != 0)
        return DG_ERR_WRONG_PASSWORD;
    if (dg_pbkdf2_sha256(pwd, salt, sizeof(salt), DG_PBKDF2_ITERS, calc) != DG_OK)
        return DG_ERR_INTERNAL;
    return (dg_constant_time_cmp(calc, expect, HASH_LEN) == 0)
               ? DG_OK : DG_ERR_WRONG_PASSWORD;
}

int web_auth_get_user(char *out, size_t cap)
{
    if (!out || cap == 0)
        return DG_ERR_PARAM;
    out[0] = '\0';
    return db_config_get("web_user", out, cap);
}

bool web_auth_is_default(void)
{
    char flag[8] = "";
    return db_config_get("web_pwd_default", flag, sizeof(flag)) == DG_OK &&
           strcmp(flag, "1") == 0;
}

int web_auth_set(const char *user, const char *pwd)
{
    if (dg_valid_uid(user) != DG_OK)
        return DG_ERR_BAD_UID;
    if (dg_valid_pwd(pwd) != DG_OK)
        return DG_ERR_BAD_PWD;

    int rc = cred_store(user, pwd, false);
    if (rc != DG_OK)
        return rc;
    /* 改凭据即踢下线:旧 token 是拿旧口令换来的,不该继续有效 */
    web_session_revoke_all();
    DG_LOGI(TAG, "凭据已更新(账号 %s),所有会话已失效", user);
    return DG_OK;
}

int web_auth_change_pwd(const char *old_pwd, const char *new_pwd)
{
    char user[WEB_AUTH_USER_MAX];
    if (web_auth_get_user(user, sizeof(user)) != DG_OK)
        return DG_ERR_NOT_INIT;
    if (web_auth_verify(user, old_pwd) != DG_OK)
        return DG_ERR_WRONG_PASSWORD;
    return web_auth_set(user, new_pwd);
}

/* ---- 登录风控 ---- */

static lock_entry_t *lock_slot(const char *ip, bool create)
{
    lock_entry_t *free_slot = NULL;
    lock_entry_t *oldest = &s_locks[0];
    for (int i = 0; i < LOCK_SLOTS; i++) {
        if (s_locks[i].used && strcmp(s_locks[i].ip, ip) == 0)
            return &s_locks[i];
        if (!s_locks[i].used && !free_slot)
            free_slot = &s_locks[i];
        if (s_locks[i].locked_until < oldest->locked_until)
            oldest = &s_locks[i];
    }
    if (!create)
        return NULL;
    lock_entry_t *e = free_slot ? free_slot : oldest;   /* 满:顶掉最老的 */
    memset(e, 0, sizeof(*e));
    snprintf(e->ip, sizeof(e->ip), "%s", ip);
    e->used = true;
    return e;
}

bool web_auth_login_blocked(const char *ip, int *retry_after_s)
{
    if (!ip)
        ip = "-";
    bool blocked = false;
    pthread_mutex_lock(&s_mtx);
    lock_entry_t *e = lock_slot(ip, false);
    if (e && e->locked_until > time(NULL)) {
        blocked = true;
        if (retry_after_s)
            *retry_after_s = (int)(e->locked_until - time(NULL));
    }
    pthread_mutex_unlock(&s_mtx);
    return blocked;
}

bool web_auth_login_fail(const char *ip)
{
    if (!ip)
        ip = "-";
    bool just_locked = false;
    pthread_mutex_lock(&s_mtx);
    lock_entry_t *e = lock_slot(ip, true);
    /* 锁定期已过:计数从头开始(否则解锁后一次失败就被重新锁死) */
    if (e->locked_until != 0 && e->locked_until <= time(NULL)) {
        e->fails = 0;
        e->locked_until = 0;
    }
    e->fails++;
    if (e->fails >= FAIL_LIMIT) {
        e->locked_until = time(NULL) + LOCK_SECONDS;
        e->fails = 0;
        just_locked = true;
    }
    pthread_mutex_unlock(&s_mtx);
    if (just_locked)
        DG_LOGW(TAG, "来源 %s 连续失败达 %d 次,锁定 %d 秒", ip, FAIL_LIMIT,
                LOCK_SECONDS);
    return just_locked;
}

void web_auth_login_ok(const char *ip)
{
    if (!ip)
        return;
    pthread_mutex_lock(&s_mtx);
    lock_entry_t *e = lock_slot(ip, false);
    if (e) {
        e->fails = 0;
        e->locked_until = 0;
    }
    pthread_mutex_unlock(&s_mtx);
}
