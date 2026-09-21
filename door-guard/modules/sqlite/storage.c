/*
 * storage.c — 存储 HAL 实现(SQLite + 加密 + 唯一性/上限校验)
 *
 * 设计要点:
 * - 单连接 + 全局互斥串行化:门禁写入频次(日志/用户)远低于 SQLite 串行
 *   吞吐,换取"查重→插入"的原子性,避免并发重复注册
 * - DDL 与 spec-database §1/§4/§5 逐字一致,不得改动(上位机/运维依赖 schema)
 * - 特征加密落盘(设备密钥 AES-256-CTR),内存按需解密,用后擦除;
 *   绑定一律 SQLITE_TRANSIENT:bind 后立即拷贝,源缓冲复用/出栈都安全
 */
#include "storage.h"
#include "crypto.h"
#include "valid.h"
#include "dg_log.h"

#include <openssl/rand.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdio.h>

static const char *TAG = "[STORAGE]";

static sqlite3 *s_db = NULL;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static char s_db_path[256];   /* 库文件路径(storage_init 时记下):供占用查询 */

/* 特征查重比较器(缺省逐字节相等;enroll 编排注入相似度算法) */
static dg_feature_cmp_fn s_face_cmp;
static dg_feature_cmp_fn s_finger_cmp;
static void *s_cmp_ud;

/* ---- 人脸特征内存缓存(M2②:verify 热路径直读,禁止逐帧查库) ----
 * 写侧:变更在 s_mtx 临界区内"整表拷贝 + 增量"生成新快照后原子切换(发布失败
 * 降级全量重载,再失败置 broken → 快照恒空,1:N 恒不命中,fail-closed);
 * 读侧:storage_features_ro() 取只读快照(读锁,仅与发布瞬间互斥),用完必须
 * 配对 storage_features_ro_done();持快照期间禁止再调 storage 其他接口(防锁序)。
 * 只缓存人脸(1:N 热路径);指纹/IC 走 1:1 与查重迭代器,保持 DB 路径。 */
typedef struct {
    uint32_t        count;
    dg_feat_ent_t  *ents;                   /* 随快照整体替换的 malloc 数组 */
} cache_snap_t;

static cache_snap_t s_cache;
static bool s_cache_broken;               /* 发布/重载双双失败:快照恒空(fail-closed) */
static pthread_rwlock_t s_cache_lk = PTHREAD_RWLOCK_INITIALIZER;

static int cache_load_locked(void);       /* 前置声明(实现在 DB 读区) */

static void cache_ent_fill(dg_feat_ent_t *e, const char *uid, int32_t role,
                           uint32_t auth_flags, const uint8_t *face, uint16_t face_len)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->user_id, sizeof(e->user_id), "%s", uid);
    e->role = role;
    e->auth_flags = auth_flags;
    e->face_len = face_len;
    if (face_len > 0)
        memcpy(e->face_vec, face, face_len);
}

/* 发布新快照:wrlock 内换指针 + 释放旧数组(读者持 rdlock,绝无 use-after-free) */
static void cache_publish_locked(dg_feat_ent_t *ents, uint32_t n)
{
    cache_snap_t ns = { .count = n, .ents = ents };
    pthread_rwlock_wrlock(&s_cache_lk);
    cache_snap_t old = s_cache;
    s_cache = ns;
    pthread_rwlock_unlock(&s_cache_lk);
    free(old.ents);
}

/* 当前快照的拷贝(扩容 extra);调用方持 s_mtx(s_cache 仅在 s_mtx 下变更) */
static dg_feat_ent_t *cache_copy(uint32_t extra, uint32_t *out_cap)
{
    uint32_t cap = s_cache.count + extra;
    dg_feat_ent_t *arr = malloc(sizeof(dg_feat_ent_t) * (cap ? cap : 1));
    if (!arr)
        return NULL;
    if (s_cache.count)
        memcpy(arr, s_cache.ents, sizeof(dg_feat_ent_t) * s_cache.count);
    *out_cap = cap;
    return arr;
}

/* 缓存维护失败的自愈:全量重载;再失败置 broken(fail-closed,日志见) */
static void cache_recover_locked(void)
{
    if (cache_load_locked() != DG_OK) {
        s_cache_broken = true;
        DG_LOGE(TAG, "特征缓存维护失败且重载失败,1:N 快照已置空(fail-closed)");
    }
}

static int cache_add_locked(const user_rec_t *in)
{
    if (in->face_vec_len == 0)
        return DG_OK;                       /* 无脸用户不入 1:N 画廊 */
    uint32_t cap;
    dg_feat_ent_t *arr = cache_copy(1, &cap);
    if (!arr)
        return DG_ERR_NO_MEMORY;
    cache_ent_fill(&arr[s_cache.count], in->user_id, in->role, in->auth_flags,
                   in->face_vec, in->face_vec_len);
    cache_publish_locked(arr, s_cache.count + 1);
    return DG_OK;
}

/* update 语义(len=0=保留脸,role/flags 始终覆盖)与画廊同步 */
static int cache_update_locked(const char *uid, const uint8_t *face,
                               uint16_t face_len, int32_t role, uint32_t auth_flags)
{
    uint32_t idx = s_cache.count;
    for (uint32_t i = 0; i < s_cache.count; i++) {
        if (!strcmp(s_cache.ents[i].user_id, uid)) {
            idx = i;
            break;
        }
    }
    if (idx == s_cache.count && face_len == 0)
        return DG_OK;                       /* 无脸用户,画廊无关 */

    uint32_t cap;
    dg_feat_ent_t *arr = cache_copy(1, &cap);
    if (!arr)
        return DG_ERR_NO_MEMORY;
    if (idx == s_cache.count) {             /* 原无脸,本次新录 */
        cache_ent_fill(&arr[idx], uid, role, auth_flags, face, face_len);
        cache_publish_locked(arr, s_cache.count + 1);
    } else {                                /* 换脸或仅改权限 */
        const dg_feat_ent_t *old = &s_cache.ents[idx];
        cache_ent_fill(&arr[idx], uid, role, auth_flags,
                       face_len > 0 ? face : old->face_vec,
                       face_len > 0 ? face_len : old->face_len);
        cache_publish_locked(arr, s_cache.count);
    }
    return DG_OK;
}

static int cache_del_locked(const char *uid)
{
    for (uint32_t i = 0; i < s_cache.count; i++) {
        if (strcmp(s_cache.ents[i].user_id, uid))
            continue;
        uint32_t cap;
        dg_feat_ent_t *arr = cache_copy(0, &cap);
        if (!arr)
            return DG_ERR_NO_MEMORY;
        memmove(&arr[i], &arr[i + 1],
                sizeof(dg_feat_ent_t) * (s_cache.count - i - 1));
        cache_publish_locked(arr, s_cache.count - 1);
        break;
    }
    return DG_OK;
}

/* ---- DDL(与 spec-database §1/§4/§5 逐字一致,勿改) ---- */

static const char *const s_ddl[] = {
    "CREATE TABLE IF NOT EXISTS users (\n"
    "    id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    user_id     TEXT    NOT NULL UNIQUE,\n"
    "    user_name   TEXT    NOT NULL,\n"
    "    face_vec    BLOB,\n"
    "    finger_vec  BLOB,\n"
    "    pwd_hash    BLOB    NOT NULL,\n"
    "    pwd_salt    BLOB    NOT NULL,\n"
    "    ic_card     TEXT    UNIQUE,\n"
    "    role        INTEGER NOT NULL DEFAULT 0 CHECK (role IN (0,1,2)),\n"
    "    auth_flags  INTEGER NOT NULL DEFAULT 0,\n"
    "    created_at  INTEGER NOT NULL,\n"
    "    updated_at  INTEGER NOT NULL\n"
    ");",
    "CREATE TABLE IF NOT EXISTS access_logs (\n"
    "    id         INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    ts         INTEGER NOT NULL,\n"
    "    user_id    TEXT,\n"
    "    user_name  TEXT,\n"
    "    method     INTEGER NOT NULL,\n"
    "    result     INTEGER NOT NULL,\n"
    "    reason     INTEGER NOT NULL DEFAULT 0\n"
    ");",
    "CREATE INDEX IF NOT EXISTS idx_logs_ts   ON access_logs(ts);",
    "CREATE INDEX IF NOT EXISTS idx_logs_user ON access_logs(user_id, ts);",
    "CREATE TABLE IF NOT EXISTS device_config (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER NOT NULL);",
    NULL,
};

int storage_init(const char *db_path, const char *key_path)
{
    pthread_mutex_lock(&s_mtx);
    if (s_db) {
        pthread_mutex_unlock(&s_mtx);
        return DG_OK;
    }
    if (!db_path || !*db_path || !key_path || !*key_path) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_PARAM;
    }

    int rc = dg_crypto_init(key_path);
    if (rc != DG_OK) {
        DG_LOGE(TAG, "crypto init failed: %d", rc);
        pthread_mutex_unlock(&s_mtx);
        return rc;
    }

    rc = sqlite3_open(db_path, &s_db);
    if (rc != SQLITE_OK) {
        DG_LOGE(TAG, "sqlite open %s failed: %s", db_path,
                s_db ? sqlite3_errmsg(s_db) : sqlite3_errstr(rc));
        if (s_db)
            sqlite3_close(s_db);
        s_db = NULL;
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }

    snprintf(s_db_path, sizeof(s_db_path), "%s", db_path);

    /* WAL:断电只丢未 checkpoint 的日志,不损坏主库 */
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(s_db, 3000);
    /* 库文件 0600(spec-database §3):特征为密文、IC 明文,目录权限另由装配保证 */
    chmod(db_path, 0600);

    for (int i = 0; s_ddl[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(s_db, s_ddl[i], NULL, NULL, &err) != SQLITE_OK) {
            DG_LOGE(TAG, "DDL failed: %s", err ? err : "?");
            sqlite3_free(err);
            sqlite3_close(s_db);
            s_db = NULL;
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DB;
        }
    }

    DG_LOGI(TAG, "storage ready: %s", db_path);

    /* 特征缓存启动装载:装载失败属致命(1:N 恒空),让 storage_init 显式失败,
     * 由装配层"必需模块失败拒绝启动"接管 */
    int crc = cache_load_locked();
    if (crc != DG_OK) {
        DG_LOGE(TAG, "特征缓存装载失败(%d)", crc);
        pthread_mutex_unlock(&s_mtx);
        return crc;
    }
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

void storage_deinit(void)
{
    pthread_mutex_lock(&s_mtx);
    if (s_db) {
        sqlite3_close(s_db);
        s_db = NULL;
    }
    pthread_rwlock_wrlock(&s_cache_lk);
    free(s_cache.ents);
    s_cache.count = 0;
    s_cache.ents = NULL;
    s_cache_broken = false;
    pthread_rwlock_unlock(&s_cache_lk);
    pthread_mutex_unlock(&s_mtx);
    dg_crypto_deinit();
}

/* ---- 存储占用查询(web 上位机"设备信息";不暴露 SQL) ---- */

int db_storage_stats(uint64_t *db_bytes, uint64_t *free_bytes)
{
    pthread_mutex_lock(&s_mtx);
    char path[sizeof(s_db_path)];
    snprintf(path, sizeof(path), "%s", s_db_path);
    pthread_mutex_unlock(&s_mtx);

    if (db_bytes) {
        uint64_t n = 0;
        FILE *f = path[0] ? fopen(path, "rb") : NULL;
        if (f) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long sz = ftell(f);
                if (sz > 0)
                    n = (uint64_t)sz;
            }
            fclose(f);
        }
        *db_bytes = n;
    }
    if (free_bytes) {
        /* 库所在目录的可用空间;目录不存在(库未建)时回退根分区 */
        char dir[sizeof(s_db_path)];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = '\0';
        struct statvfs vfs;
        const char *probe = dir[0] ? dir : "/";
        if (statvfs(probe, &vfs) != 0 && statvfs("/", &vfs) != 0)
            return DG_ERR_IO;
        *free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    }
    return path[0] ? DG_OK : DG_ERR_NOT_INIT;
}

/* ---- 绑定/读取辅助 ---- */

static int bind_text_or_null(sqlite3_stmt *st, int idx, const char *s)
{
    /* 空串转 NULL:ic_card 空 = 未绑卡;日志 user_id 空 = 陌生人 */
    if (!s || !*s)
        return sqlite3_bind_null(st, idx);
    return sqlite3_bind_text(st, idx, s, -1, SQLITE_TRANSIENT);
}

static void col_text_copy(sqlite3_stmt *st, int col, char *out, size_t cap)
{
    if (sqlite3_column_type(st, col) == SQLITE_NULL) {
        out[0] = '\0';
        return;
    }
    const unsigned char *t = sqlite3_column_text(st, col);
    snprintf(out, cap, "%s", t ? (const char *)t : "");
}

/* 行 → user_rec_t;特征 BLOB 自动解密(NULL → len=0) */
static int row_to_user(sqlite3_stmt *st, user_rec_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int64(st, 0);
    col_text_copy(st, 1, out->user_id, sizeof(out->user_id));
    col_text_copy(st, 2, out->user_name, sizeof(out->user_name));

    const void *blob;
    int nbytes;
    size_t plain_len = 0;
    int rc;

    blob = sqlite3_column_blob(st, 3);
    nbytes = sqlite3_column_bytes(st, 3);
    if (blob && nbytes > 0) {
        rc = dg_feature_unwrap(blob, (size_t)nbytes, out->face_vec,
                               sizeof(out->face_vec), &plain_len);
        if (rc != DG_OK)
            return rc;
        out->face_vec_len = (uint16_t)plain_len;
    }

    blob = sqlite3_column_blob(st, 4);
    nbytes = sqlite3_column_bytes(st, 4);
    if (blob && nbytes > 0) {
        rc = dg_feature_unwrap(blob, (size_t)nbytes, out->finger_vec,
                               sizeof(out->finger_vec), &plain_len);
        if (rc != DG_OK)
            return rc;
        out->finger_vec_len = (uint16_t)plain_len;
    }

    blob = sqlite3_column_blob(st, 5);
    nbytes = sqlite3_column_bytes(st, 5);
    if (blob && nbytes > 0)
        memcpy(out->pwd_hash, blob, (size_t)nbytes);
    blob = sqlite3_column_blob(st, 6);
    nbytes = sqlite3_column_bytes(st, 6);
    if (blob && nbytes > 0)
        memcpy(out->pwd_salt, blob, (size_t)nbytes);

    col_text_copy(st, 7, out->ic_card, sizeof(out->ic_card));
    out->role = sqlite3_column_int(st, 8);
    out->auth_flags = (uint32_t)sqlite3_column_int64(st, 9);
    out->created_at = sqlite3_column_int64(st, 10);
    out->updated_at = sqlite3_column_int64(st, 11);
    return DG_OK;
}

/* ---- 唯一性预检(调用方须持 s_mtx) ---- */

/* 特征缓存全量装载(调用方持 s_mtx;storage_init 与自愈路径用) */
static int cache_load_locked(void)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db,
                           "SELECT user_id,role,auth_flags,face_vec FROM users",
                           -1, &st, NULL) != SQLITE_OK)
        return DG_ERR_DB;
    dg_feat_ent_t *arr = malloc(sizeof(dg_feat_ent_t) * (size_t)DG_USER_MAX);
    if (!arr) {
        sqlite3_finalize(st);
        return DG_ERR_NO_MEMORY;
    }
    uint32_t n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < DG_USER_MAX) {
        dg_feat_ent_t *e = &arr[n];
        memset(e, 0, sizeof(*e));
        col_text_copy(st, 0, e->user_id, sizeof(e->user_id));
        e->role = sqlite3_column_int(st, 1);
        e->auth_flags = (uint32_t)sqlite3_column_int64(st, 2);
        const void *blob = sqlite3_column_blob(st, 3);
        int nbytes = sqlite3_column_bytes(st, 3);
        size_t plain_len = 0;
        if (blob && nbytes > 0
            && dg_feature_unwrap(blob, (size_t)nbytes, e->face_vec,
                                 sizeof(e->face_vec), &plain_len) == DG_OK
            && plain_len > 0) {
            e->face_len = (uint16_t)plain_len;
            n++;
        }
        /* 解密失败的行:跳过并告警(坏行不拖垮整个画廊;长度自诊断会另报) */
        if (blob && nbytes > 0 && e->face_len == 0)
            DG_LOGW(TAG, "特征缓存:跳过坏行 uid=%s", e->user_id);
    }
    sqlite3_finalize(st);
    cache_publish_locked(arr, n);
    s_cache_broken = false;
    return DG_OK;
}

static bool uid_exists_locked(const char *user_id)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, "SELECT 1 FROM users WHERE user_id=?1", -1, &st, NULL)
        != SQLITE_OK)
        return true;                            /* 预检失败按"存在"拒绝,宁可错杀 */
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return exists;
}

static bool ic_exists_locked(const char *ic, const char *exclude_uid)
{
    sqlite3_stmt *st;
    const char *sql = exclude_uid
        ? "SELECT 1 FROM users WHERE ic_card=?1 AND user_id<>?2"
        : "SELECT 1 FROM users WHERE ic_card=?1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return true;
    sqlite3_bind_text(st, 1, ic, -1, SQLITE_TRANSIENT);
    if (exclude_uid)
        sqlite3_bind_text(st, 2, exclude_uid, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return exists;
}

/**
 * 特征 1:N 查重(spec-database §2:相似度语义,必须逐行解密比较):
 * 返回 1 重复 / 0 不重复 / <0 错误。exclude_uid 用于 update 时排除自身。
 */
static int feature_dup_locked(bool is_face, const char *exclude_uid,
                              const uint8_t *plain, uint16_t len)
{
    dg_feature_cmp_fn cmp = is_face ? s_face_cmp : s_finger_cmp;

    const char *col = is_face ? "face_vec" : "finger_vec";
    char sql[128];
    if (exclude_uid)
        snprintf(sql, sizeof(sql),
                 "SELECT user_id, %s FROM users WHERE %s NOT NULL AND user_id<>?1",
                 col, col);
    else
        snprintf(sql, sizeof(sql),
                 "SELECT user_id, %s FROM users WHERE %s NOT NULL", col, col);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (exclude_uid)
        sqlite3_bind_text(st, 1, exclude_uid, -1, SQLITE_TRANSIENT);

    int dup = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 1);
        int nbytes = sqlite3_column_bytes(st, 1);
        if (!blob || nbytes <= 16)
            continue;
        uint8_t existing[DG_FEATURE_MAX];
        size_t existing_len = 0;
        if (dg_feature_unwrap(blob, (size_t)nbytes, existing,
                              sizeof(existing), &existing_len) != DG_OK)
            continue;                           /* 单行解密失败跳过,不中断查重 */

        int match;
        if (cmp)
            match = cmp(plain, len, existing, (uint16_t)existing_len, s_cmp_ud);
        else
            match = (existing_len == len
                     && dg_constant_time_cmp(plain, existing, len) == 0);
        dg_secure_wipe(existing, sizeof(existing));
        if (match == 1) {
            dup = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    return dup;
}

int storage_set_feature_cmp(dg_feature_cmp_fn face_cmp,
                            dg_feature_cmp_fn finger_cmp, void *ud)
{
    pthread_mutex_lock(&s_mtx);
    if (face_cmp)
        s_face_cmp = face_cmp;
    if (finger_cmp)
        s_finger_cmp = finger_cmp;
    s_cmp_ud = ud;
    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

/* ---- users 公共接口 ---- */

int db_user_set_password(user_rec_t *rec, const char *plain_pwd)
{
    if (!rec || !plain_pwd || !*plain_pwd || strlen(plain_pwd) >= DG_PWD_MAX_LEN)
        return DG_ERR_PARAM;
    /* 密码合法性(长度/字符集):规则唯一权威见 proto/valid.h */
    int vrc = dg_valid_pwd(plain_pwd);
    if (vrc != DG_OK)
        return vrc;
    if (RAND_bytes(rec->pwd_salt, DG_PWD_SALT_LEN) != 1)
        return DG_ERR_INTERNAL;
    return dg_pbkdf2_sha256(plain_pwd, rec->pwd_salt, DG_PWD_SALT_LEN,
                            DG_PBKDF2_ITERS, rec->pwd_hash);
}

int db_user_add(const user_rec_t *in)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!in || !in->user_id[0] || !in->user_name[0])
        return DG_ERR_PARAM;
    /* 字段合法性(ID/姓名):UI 弹窗已即时校验,这里是权威兜底
     * (上位机/脚本/未来 API 都绕不过) */
    int vrc = dg_valid_uid(in->user_id);
    if (vrc != DG_OK)
        return vrc;
    vrc = dg_valid_name(in->user_name);
    if (vrc != DG_OK)
        return vrc;

    pthread_mutex_lock(&s_mtx);

    /* 密码必填(spec-database §1:业务层 INSERT 前校验,不靠 DB 约束兜底):
     * 合法哈希不会全零,全零 = 调用方未调用 set_password */
    static const uint8_t zero_hash[DG_PWD_HASH_LEN] = { 0 };
    if (dg_constant_time_cmp(in->pwd_hash, zero_hash, DG_PWD_HASH_LEN) == 0) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_NO_PASSWORD;
    }

    if (uid_exists_locked(in->user_id)) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DUP_UID;
    }
    if (in->ic_card[0] && ic_exists_locked(in->ic_card, NULL)) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DUP_IC;
    }

    int dup;
    if (in->face_vec_len > 0) {
        dup = feature_dup_locked(true, NULL, in->face_vec, in->face_vec_len);
        if (dup < 0) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DB;
        }
        if (dup) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DUP_FACE;
        }
    }
    if (in->finger_vec_len > 0) {
        dup = feature_dup_locked(false, NULL, in->finger_vec, in->finger_vec_len);
        if (dup < 0) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DB;
        }
        if (dup) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DUP_FINGER;
        }
    }

    /* 用户上限(spec-database §1:添加前 COUNT 校验,边界含第 2000 个) */
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM users", -1, &st, NULL)
            != SQLITE_OK) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DB;
        }
        bool full = false;
        if (sqlite3_step(st) == SQLITE_ROW)
            full = sqlite3_column_int(st, 0) >= DG_USER_MAX;
        sqlite3_finalize(st);
        if (full) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_USER_LIMIT;
        }
    }

    const char *sql =
        "INSERT INTO users(user_id,user_name,face_vec,finger_vec,pwd_hash,pwd_salt,"
        "ic_card,role,auth_flags,created_at,updated_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }

    uint8_t enc[DG_FEATURE_MAX + 16];
    size_t enc_len = 0;
    int rc = DG_OK;
    int64_t now = (int64_t)time(NULL);

    sqlite3_bind_text(st, 1, in->user_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, in->user_name, -1, SQLITE_TRANSIENT);

    if (in->face_vec_len > 0) {
        if (dg_feature_wrap(in->face_vec, in->face_vec_len, enc, sizeof(enc), &enc_len)
            != DG_OK)
            rc = DG_ERR_INTERNAL;
        else
            sqlite3_bind_blob(st, 3, enc, (int)enc_len, SQLITE_TRANSIENT);
    }
    if (rc == DG_OK && in->finger_vec_len > 0) {
        if (dg_feature_wrap(in->finger_vec, in->finger_vec_len, enc, sizeof(enc), &enc_len)
            != DG_OK)
            rc = DG_ERR_INTERNAL;
        else
            sqlite3_bind_blob(st, 4, enc, (int)enc_len, SQLITE_TRANSIENT);
    }

    if (rc == DG_OK) {
        sqlite3_bind_blob(st, 5, in->pwd_hash, DG_PWD_HASH_LEN, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 6, in->pwd_salt, DG_PWD_SALT_LEN, SQLITE_TRANSIENT);
        bind_text_or_null(st, 7, in->ic_card);
        sqlite3_bind_int(st, 8, in->role);
        sqlite3_bind_int64(st, 9, (sqlite3_int64)in->auth_flags);
        sqlite3_bind_int64(st, 10, now);
        sqlite3_bind_int64(st, 11, now);

        if (sqlite3_step(st) != SQLITE_DONE) {
            /* UNIQUE 冲突兜底(预检遗漏时映射为业务码而非裸 DB 错) */
            DG_LOGE(TAG, "user add: %s", sqlite3_errmsg(s_db));
            rc = DG_ERR_DUP_UID;
        }
    }
    sqlite3_finalize(st);
    if (rc == DG_OK && cache_add_locked(in) != DG_OK) {
        DG_LOGE(TAG, "特征缓存增量更新失败(add),触发全量重载");
        cache_recover_locked();
    }
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_update(const user_rec_t *in)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!in || !in->user_id[0])
        return DG_ERR_PARAM;
    int vrc = dg_valid_uid(in->user_id);
    if (vrc != DG_OK)
        return vrc;
    if (in->user_name[0]) {
        vrc = dg_valid_name(in->user_name);
        if (vrc != DG_OK)
            return vrc;
    }

    pthread_mutex_lock(&s_mtx);

    /* 语义:完整记录更新;特例 —— user_name 空=保留,face/finger len=0=保留,
     * pwd_hash 全零=保留密码,ic_card 始终覆盖(空串=解绑),role/auth_flags 覆盖 */
    user_rec_t existing;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, "SELECT * FROM users WHERE user_id=?1", -1, &st, NULL)
        != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, in->user_id, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (!found) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_NOT_FOUND;
    }
    int grc = row_to_user(st, &existing);
    sqlite3_finalize(st);
    if (grc != DG_OK) {
        pthread_mutex_unlock(&s_mtx);
        return grc;
    }

    const char *new_name = in->user_name[0] ? in->user_name : existing.user_name;
    const char *new_ic = in->ic_card;
    if (new_ic[0] && strcmp(new_ic, existing.ic_card) != 0
        && ic_exists_locked(new_ic, in->user_id)) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DUP_IC;
    }

    int dup;
    if (in->face_vec_len > 0) {
        dup = feature_dup_locked(true, in->user_id, in->face_vec, in->face_vec_len);
        if (dup < 0) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DB;
        }
        if (dup) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DUP_FACE;
        }
    }
    if (in->finger_vec_len > 0) {
        dup = feature_dup_locked(false, in->user_id, in->finger_vec, in->finger_vec_len);
        if (dup < 0) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DB;
        }
        if (dup) {
            pthread_mutex_unlock(&s_mtx);
            return DG_ERR_DUP_FINGER;
        }
    }

    /* 非覆盖字段回填既有值(特征需重新加密落盘) */
    const uint8_t *face_plain = in->face_vec_len > 0 ? in->face_vec : existing.face_vec;
    uint16_t face_len = in->face_vec_len > 0 ? in->face_vec_len : existing.face_vec_len;
    const uint8_t *finger_plain =
        in->finger_vec_len > 0 ? in->finger_vec : existing.finger_vec;
    uint16_t finger_len =
        in->finger_vec_len > 0 ? in->finger_vec_len : existing.finger_vec_len;

    const char *sql =
        "UPDATE users SET user_name=?2, face_vec=?3, finger_vec=?4, pwd_hash=?5,"
        " pwd_salt=?6, ic_card=?7, role=?8, auth_flags=?9, updated_at=?10"
        " WHERE user_id=?1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }

    uint8_t enc[DG_FEATURE_MAX + 16];
    size_t enc_len = 0;
    int rc = DG_OK;
    static const uint8_t zero_hash[DG_PWD_HASH_LEN] = { 0 };
    const uint8_t *hash = dg_constant_time_cmp(in->pwd_hash, zero_hash, DG_PWD_HASH_LEN)
                              ? in->pwd_hash : existing.pwd_hash;
    const uint8_t *salt = dg_constant_time_cmp(in->pwd_hash, zero_hash, DG_PWD_HASH_LEN)
                              ? in->pwd_salt : existing.pwd_salt;

    sqlite3_bind_text(st, 1, in->user_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, new_name, -1, SQLITE_TRANSIENT);

    if (face_len > 0) {
        if (dg_feature_wrap(face_plain, face_len, enc, sizeof(enc), &enc_len) != DG_OK)
            rc = DG_ERR_INTERNAL;
        else
            sqlite3_bind_blob(st, 3, enc, (int)enc_len, SQLITE_TRANSIENT);
    }
    if (rc == DG_OK && finger_len > 0) {
        if (dg_feature_wrap(finger_plain, finger_len, enc, sizeof(enc), &enc_len)
            != DG_OK)
            rc = DG_ERR_INTERNAL;
        else
            sqlite3_bind_blob(st, 4, enc, (int)enc_len, SQLITE_TRANSIENT);
    }
    if (rc == DG_OK) {
        sqlite3_bind_blob(st, 5, hash, DG_PWD_HASH_LEN, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 6, salt, DG_PWD_SALT_LEN, SQLITE_TRANSIENT);
        bind_text_or_null(st, 7, new_ic);
        sqlite3_bind_int(st, 8, in->role);
        sqlite3_bind_int64(st, 9, (sqlite3_int64)in->auth_flags);
        sqlite3_bind_int64(st, 10, (int64_t)time(NULL));
        if (sqlite3_step(st) != SQLITE_DONE) {
            DG_LOGE(TAG, "user update: %s", sqlite3_errmsg(s_db));
            rc = DG_ERR_DB;
        }
    }
    sqlite3_finalize(st);
    if (rc == DG_OK
        && cache_update_locked(in->user_id, face_plain, face_len,
                               in->role, in->auth_flags) != DG_OK) {
        DG_LOGE(TAG, "特征缓存增量更新失败(update),触发全量重载");
        cache_recover_locked();
    }
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_del(const char *user_id)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!user_id || !*user_id)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, "DELETE FROM users WHERE user_id=?1", -1, &st, NULL)
        != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(s_db) == 0)
        rc = sqlite3_changes(s_db) == 0 ? DG_ERR_NOT_FOUND : DG_ERR_DB;
    sqlite3_finalize(st);
    if (rc == DG_OK && cache_del_locked(user_id) != DG_OK) {
        DG_LOGE(TAG, "特征缓存增量更新失败(del),触发全量重载");
        cache_recover_locked();
    }
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_clear_face(const char *user_id)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!user_id || !*user_id)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db,
                           "UPDATE users SET face_vec=NULL WHERE user_id=?1",
                           -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(s_db) == 0)
        rc = sqlite3_changes(s_db) == 0 ? DG_ERR_NOT_FOUND : DG_ERR_DB;
    sqlite3_finalize(st);
    /* M2 特征缓存同步:人脸已清,1:N 缓存里该用户一并移除(缓存恢复兜底) */
    if (rc == DG_OK && cache_del_locked(user_id) != DG_OK) {
        DG_LOGE(TAG, "特征缓存增量更新失败(clear face),触发全量重载");
        cache_recover_locked();
    }
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_get(const char *user_id, user_rec_t *out)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!user_id || !*user_id || !out)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, "SELECT * FROM users WHERE user_id=?1", -1, &st, NULL)
        != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        rc = row_to_user(st, out);
    else
        rc = DG_ERR_NOT_FOUND;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_count(uint32_t *n)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!n)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM users", -1, &st, NULL)
        != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    if (sqlite3_step(st) == SQLITE_ROW)
        *n = (uint32_t)sqlite3_column_int(st, 0);
    else
        rc = DG_ERR_DB;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_count_role(int32_t role, uint32_t *n)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!n)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM users WHERE role=?1", -1,
                           &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_int(st, 1, role);
    if (sqlite3_step(st) == SQLITE_ROW)
        *n = (uint32_t)sqlite3_column_int(st, 0);
    else
        rc = DG_ERR_DB;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_verify_password(const char *user_id, const char *pwd, user_rec_t *out)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!user_id || !*user_id || !pwd)
        return DG_ERR_PARAM;

    user_rec_t rec;
    int rc = db_user_get(user_id, &rec);
    if (rc != DG_OK)
        return rc;                              /* 含 DG_ERR_NOT_FOUND:用户不存在 */

    uint8_t computed[DG_PWD_HASH_LEN];
    rc = dg_pbkdf2_sha256(pwd, rec.pwd_salt, DG_PWD_SALT_LEN, DG_PBKDF2_ITERS,
                          computed);
    if (rc != DG_OK)
        return rc;

    if (dg_constant_time_cmp(computed, rec.pwd_hash, DG_PWD_HASH_LEN) != 0) {
        dg_secure_wipe(computed, sizeof(computed));
        return DG_ERR_WRONG_PASSWORD;
    }
    dg_secure_wipe(computed, sizeof(computed));

    if (out)
        *out = rec;
    return DG_OK;
}

int db_find_by_ic(const char *ic, user_rec_t *out)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!ic || !*ic || !out)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, "SELECT * FROM users WHERE ic_card=?1", -1, &st, NULL)
        != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, ic, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        rc = row_to_user(st, out);
    else
        rc = DG_ERR_NOT_FOUND;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

/* ---- 特征迭代器(enroll 编排查重遍历) ---- */

static int feature_iter_locked(bool is_face, dg_feature_iter_fn fn, void *ud)
{
    const char *col = is_face ? "face_vec" : "finger_vec";
    char sql[96];
    snprintf(sql, sizeof(sql),
             "SELECT user_id, %s FROM users WHERE %s NOT NULL ORDER BY id", col, col);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK)
        return DG_ERR_DB;

    int rc = DG_OK;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *uid = (const char *)sqlite3_column_text(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int nbytes = sqlite3_column_bytes(st, 1);
        if (!uid || !blob || nbytes <= 16)
            continue;

        uint8_t plain[DG_FEATURE_MAX];
        size_t plain_len = 0;
        if (dg_feature_unwrap(blob, (size_t)nbytes, plain, sizeof(plain), &plain_len)
            != DG_OK)
            continue;                           /* 单行损坏跳过 */

        int stop = fn(uid, plain, (uint16_t)plain_len, ud);
        dg_secure_wipe(plain, sizeof(plain));
        if (stop != 0)
            break;                              /* 回调要求中止(如已命中) */
    }
    sqlite3_finalize(st);
    return rc;
}

int db_user_iter_face(dg_feature_iter_fn fn, void *ud)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!fn)
        return DG_ERR_PARAM;
    pthread_mutex_lock(&s_mtx);
    int rc = feature_iter_locked(true, fn, ud);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_user_iter_finger(dg_feature_iter_fn fn, void *ud)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!fn)
        return DG_ERR_PARAM;
    pthread_mutex_lock(&s_mtx);
    int rc = feature_iter_locked(false, fn, ud);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

/* ---- access_logs ---- */

int db_log_append(const access_log_t *log)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!log)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    const char *sql = "INSERT INTO access_logs(ts,user_id,user_name,method,result,reason)"
                      " VALUES(?1,?2,?3,?4,?5,?6)";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_int64(st, 1, log->ts);
    bind_text_or_null(st, 2, log->has_user ? log->user_id : NULL);
    bind_text_or_null(st, 3, log->has_user ? log->user_name : NULL);
    sqlite3_bind_int(st, 4, log->method);
    sqlite3_bind_int(st, 5, log->result);
    sqlite3_bind_int(st, 6, log->reason);
    if (sqlite3_step(st) != SQLITE_DONE) {
        DG_LOGE(TAG, "log append: %s", sqlite3_errmsg(s_db));
        rc = DG_ERR_DB;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_log_query(const log_query_t *q, log_page_t *out)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!q || !out || !out->logs || out->max == 0)
        return DG_ERR_PARAM;
    if (q->page_size == 0 || q->page == 0)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);

    /* 过滤条件动态拼接:匿名 ? 按出现顺序绑定,避免具名 ?N 与条件化绑定错位 */
    char where[192] = "";
    size_t w = 0;
    if (q->ts_from > 0)
        w += (size_t)snprintf(where + w, sizeof(where) - w, " ts>=? AND");
    if (q->ts_to > 0)
        w += (size_t)snprintf(where + w, sizeof(where) - w, " ts<=? AND");
    if (q->user_id[0])
        w += (size_t)snprintf(where + w, sizeof(where) - w, " user_id=? AND");
    if (w > 0)
        where[w - 4] = '\0';                    /* 去掉尾部 AND */

    char sql[512];
    /* page/page_size 已校验为正整数,直接内联(避免动态 ?N 编号错位);
     * 时间/用户等过滤值仍走绑定,不拼用户输入 */
    snprintf(sql, sizeof(sql),
             "SELECT id,ts,user_id,user_name,method,result,reason FROM access_logs"
             "%s%s ORDER BY ts %s, id %s LIMIT %u OFFSET %u",
             w ? " WHERE" : "", where,
             q->descending ? "DESC" : "ASC", q->descending ? "DESC" : "ASC",
             q->page_size, (q->page - 1) * q->page_size);

    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }

    int idx = 0;
    if (q->ts_from > 0)
        sqlite3_bind_int64(st, ++idx, q->ts_from);
    if (q->ts_to > 0)
        sqlite3_bind_int64(st, ++idx, q->ts_to);
    if (q->user_id[0])
        sqlite3_bind_text(st, ++idx, q->user_id, -1, SQLITE_TRANSIENT);

    out->count = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (out->count >= out->max) {
            rc = DG_ERR_PARAM;                  /* 页容量不足:显式报错而非截断 */
            break;
        }
        access_log_t *l = &out->logs[out->count++];
        l->id = sqlite3_column_int64(st, 0);
        l->ts = sqlite3_column_int64(st, 1);
        l->has_user = sqlite3_column_type(st, 2) != SQLITE_NULL;
        col_text_copy(st, 2, l->user_id, sizeof(l->user_id));
        col_text_copy(st, 3, l->user_name, sizeof(l->user_name));
        l->method = sqlite3_column_int(st, 4);
        l->result = sqlite3_column_int(st, 5);
        l->reason = sqlite3_column_int(st, 6);
    }
    sqlite3_finalize(st);
    if (rc != DG_OK) {
        pthread_mutex_unlock(&s_mtx);
        return rc;
    }

    /* 总条数(分页 UI 用) */
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM access_logs%s%s",
             w ? " WHERE" : "", where);
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    idx = 0;
    if (q->ts_from > 0)
        sqlite3_bind_int64(st, ++idx, q->ts_from);
    if (q->ts_to > 0)
        sqlite3_bind_int64(st, ++idx, q->ts_to);
    if (q->user_id[0])
        sqlite3_bind_text(st, ++idx, q->user_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        out->total = (uint32_t)sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    pthread_mutex_unlock(&s_mtx);
    return DG_OK;
}

/* ---- device_config KV ---- */

int db_config_set(const char *key, const char *value)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!key || !*key || !value)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    const char *sql = "INSERT INTO device_config(key,value,updated_at) VALUES(?1,?2,?3)"
                      " ON CONFLICT(key) DO UPDATE SET value=?2, updated_at=?3";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (int64_t)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) {
        DG_LOGE(TAG, "config set: %s", sqlite3_errmsg(s_db));
        rc = DG_ERR_DB;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

int db_config_get(const char *key, char *out, size_t out_size)
{
    if (!s_db)
        return DG_ERR_NOT_INIT;
    if (!key || !*key || !out || out_size == 0)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mtx);
    sqlite3_stmt *st;
    int rc = DG_OK;
    if (sqlite3_prepare_v2(s_db, "SELECT value FROM device_config WHERE key=?1", -1,
                           &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_DB;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        col_text_copy(st, 0, out, out_size);
    else
        rc = DG_ERR_NOT_FOUND;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s_mtx);
    return rc;
}

/* ---- 特征只读快照(M2②;只读直调登记制白名单项,proposal §1) ----
 * 用法:const dg_feat_snap_t *s = storage_features_ro(); 遍历 s->ents;
 *      storage_features_ro_done();  —— 必须成对、不可嵌套;持快照期间
 *      禁止调用 storage 其他接口(写侧持 s_mtx 等 wrlock,防锁序倒挂)。
 * broken 状态返回 count=0:消费方(1:N 比对)恒不命中,安全侧失败。 */
const dg_feat_snap_t *storage_features_ro(void)
{
    pthread_rwlock_rdlock(&s_cache_lk);
    if (s_cache_broken) {
        static const dg_feat_snap_t empty = { .count = 0, .ents = NULL };
        return &empty;                      /* 不解锁:ro_done 统一解,见下 */
    }
    return (const dg_feat_snap_t *)&s_cache;
}

void storage_features_ro_done(void)
{
    pthread_rwlock_unlock(&s_cache_lk);
}
