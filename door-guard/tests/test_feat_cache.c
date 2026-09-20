/*
 * test_feat_cache.c — 人脸特征内存缓存测试(M2②)
 *
 * 覆盖:启动全量装载;增/改/删的画廊增量同步;无脸用户不入快照;
 * update 语义(len=0 保留脸、role/flags 覆盖);重启(deinit+init)后
 * 与 DB 一致;ro/ro_done 成对使用。
 */
#include "dg_test.h"
#include "storage.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[64];

static void face_make(uint8_t *out, uint8_t seed)
{
    for (int i = 0; i < 64; i++)
        out[i] = (uint8_t)(seed * 10 + i);
}

static int user_make(user_rec_t *rec, const char *uid, const uint8_t *face,
                     int32_t role, uint32_t flags)
{
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->user_id, sizeof(rec->user_id), "%s", uid);
    snprintf(rec->user_name, sizeof(rec->user_name), "user-%s", uid);
    rec->role = role;
    rec->auth_flags = flags;
    if (face) {
        memcpy(rec->face_vec, face, 64);
        rec->face_vec_len = 64;
    }
    return db_user_set_password(rec, "pwd-123456");
}

static const dg_feat_ent_t *snap_find(const dg_feat_snap_t *s, const char *uid)
{
    for (uint32_t i = 0; i < s->count; i++)
        if (!strcmp(s->ents[i].user_id, uid))
            return &s->ents[i];
    return NULL;
}

int main(void)
{
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_fc_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        exit(1);
    char db[96], key[96];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    DG_CHECK(storage_init(db, key) == DG_OK);

    /* 空库:快照为空 */
    const dg_feat_snap_t *s = storage_features_ro();
    DG_CHECK(s->count == 0);
    storage_features_ro_done();

    /* 录入:u1 普通/u2 管理员(都有脸);u3 无脸(不入画廊);u4 黑名单有脸 */
    uint8_t f1[64], f2[64], f2b[64], f4[64];
    face_make(f1, 1);
    face_make(f2, 2);
    face_make(f2b, 8);
    face_make(f4, 4);
    user_rec_t rec;
    DG_CHECK(user_make(&rec, "1001", f1, DG_ROLE_NORMAL,
                       DG_AUTH_FACE | DG_AUTH_PWD) == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
    DG_CHECK(user_make(&rec, "1002", f2, DG_ROLE_ADMIN, DG_AUTH_ALL) == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
    DG_CHECK(user_make(&rec, "1003", NULL, DG_ROLE_NORMAL, DG_AUTH_PWD) == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);
    DG_CHECK(user_make(&rec, "1004", f4, DG_ROLE_BLACKLIST, DG_AUTH_ALL) == DG_OK);
    DG_CHECK(db_user_add(&rec) == DG_OK);

    s = storage_features_ro();
    DG_CHECK(s->count == 3);                        /* u3 无脸被排除 */
    const dg_feat_ent_t *e1 = snap_find(s, "1001");
    const dg_feat_ent_t *e2 = snap_find(s, "1002");
    const dg_feat_ent_t *e4 = snap_find(s, "1004");
    DG_CHECK(e1 && e2 && e4);
    DG_CHECK(e1->face_len == 64 && memcmp(e1->face_vec, f1, 64) == 0);
    DG_CHECK(e1->role == DG_ROLE_NORMAL);
    DG_CHECK(e2->role == DG_ROLE_ADMIN && e2->auth_flags == DG_AUTH_ALL);
    DG_CHECK(e4->role == DG_ROLE_BLACKLIST);        /* 黑名单在快照里,过滤归消费方 */
    DG_CHECK(snap_find(s, "1003") == NULL);
    storage_features_ro_done();

    /* update u2:换脸 + 关人脸开关 → 快照同歩 */
    user_rec_t up;
    DG_CHECK(user_make(&up, "1002", f2b, DG_ROLE_ADMIN, DG_AUTH_PWD) == DG_OK);
    DG_CHECK(db_user_update(&up) == DG_OK);
    s = storage_features_ro();
    e2 = snap_find(s, "1002");
    DG_CHECK(e2 && memcmp(e2->face_vec, f2b, 64) == 0);
    DG_CHECK(e2->auth_flags == DG_AUTH_PWD);        /* 消费方按此跳过其 1:N */
    storage_features_ro_done();

    /* update u1:仅改权限(无脸入参=保留脸)→ 脸不变、角色变 */
    DG_CHECK(user_make(&up, "1001", NULL, DG_ROLE_BLACKLIST, DG_AUTH_PWD) == DG_OK);
    DG_CHECK(db_user_update(&up) == DG_OK);
    s = storage_features_ro();
    e1 = snap_find(s, "1001");
    DG_CHECK(e1 && e1->face_len == 64 && memcmp(e1->face_vec, f1, 64) == 0);
    DG_CHECK(e1->role == DG_ROLE_BLACKLIST);
    storage_features_ro_done();

    /* 删除 u4 → 画廊减一 */
    DG_CHECK(db_user_del("1004") == DG_OK);
    s = storage_features_ro();
    DG_CHECK(s->count == 2 && snap_find(s, "1004") == NULL);
    storage_features_ro_done();

    /* 重启语义:deinit+init 后与 DB 一致(全量装载路径) */
    storage_deinit();
    DG_CHECK(storage_init(db, key) == DG_OK);
    s = storage_features_ro();
    DG_CHECK(s->count == 2);
    e1 = snap_find(s, "1001");
    e2 = snap_find(s, "1002");
    DG_CHECK(e1 && e1->role == DG_ROLE_BLACKLIST
             && memcmp(e1->face_vec, f1, 64) == 0);
    DG_CHECK(e2 && memcmp(e2->face_vec, f2b, 64) == 0);
    storage_features_ro_done();

    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }

    DG_TEST_EXIT();
}
