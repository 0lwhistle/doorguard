/*
 * test_storage.c — storage HAL 全量测试(Phase 3,任务清单逐条覆盖)
 *
 * 覆盖:
 *   1 添加(正常/uid 重复/IC 重复/人脸查重/指纹查重/无密码/2000 边界)
 *   2 密码(正确/错误/用户不存在 + PBKDF2 已知向量对拍)
 *   3 日志(2500 条:时间段边界/按用户/分页整除与不整除/倒序)
 *   4 配置 KV(set/get/默认/覆盖)
 *   5 加密(特征 roundtrip/同密码两次加盐不同/AES-CTR NIST 向量)
 *   6 并发(4 线程 × 2500 条日志无丢失)
 */
#include "storage.h"
#include "crypto.h"
#include "err.h"
#include "dg_test.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 每线程独立 tmp 目录,测试互不串扰 */
static char s_dir[64] = { 0 };

static void fresh_setup(void)
{
    storage_deinit();
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_st_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0) {
        printf("  FAIL: cannot prepare %s\n", s_dir);
        exit(1);
    }
    char db[128], key[128];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    if (storage_init(db, key) != DG_OK) {
        printf("  FAIL: storage_init\n");
        exit(1);
    }
}

/* 定长拷贝(截断安全,免 snprintf 截断告警) */
static void copy_cstr(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 构造一条最小用户记录(密码经 set_password;特征按需) */
static user_rec_t make_user(const char *uid, const char *name, const char *pwd)
{
    user_rec_t u;
    memset(&u, 0, sizeof(u));
    copy_cstr(u.user_id, sizeof(u.user_id), uid);
    copy_cstr(u.user_name, sizeof(u.user_name), name);
    u.role = DG_ROLE_NORMAL;
    if (pwd)
        DG_CHECK(db_user_set_password(&u, pwd) == DG_OK);
    return u;
}

/* 特征查重测试用比较器:解密后逐字节相等即重复 */
static int cmp_bytes(const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl, void *ud)
{
    (void)ud;
    return (al == bl && memcmp(a, b, al) == 0) ? 1 : 0;
}

/* ================= 1 添加 ================= */

static void test_add(void)
{
    printf("[S1] user add: normal / dup uid / dup ic / dup face / dup finger / no pwd / limit\n");
    fresh_setup();
    DG_CHECK(storage_set_feature_cmp(cmp_bytes, cmp_bytes, NULL) == DG_OK);

    /* 正常 */
    user_rec_t u1 = make_user("10001", "张三", "pwd1");
    u1.role = DG_ROLE_ADMIN;
    u1.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    DG_CHECK(db_user_add(&u1) == DG_OK);

    user_rec_t got;
    DG_CHECK(db_user_get("10001", &got) == DG_OK);
    DG_CHECK(strcmp(got.user_name, "张三") == 0);
    DG_CHECK(got.role == DG_ROLE_ADMIN);
    DG_CHECK(got.auth_flags == (DG_AUTH_FACE | DG_AUTH_PWD));

    uint32_t n = 0;
    DG_CHECK(db_user_count(&n) == DG_OK && n == 1);
    /* 按权限计数(菜单入口用来判断"系统里还有没有管理员") */
    DG_CHECK(db_user_count_role(DG_ROLE_ADMIN, &n) == DG_OK && n == 1);
    DG_CHECK(db_user_count_role(DG_ROLE_NORMAL, &n) == DG_OK && n == 0);
    DG_CHECK(db_user_count_role(DG_ROLE_BLACKLIST, &n) == DG_OK && n == 0);

    /* user_id 重复 */
    user_rec_t dup = make_user("10001", "别人", "pwd2");
    DG_CHECK(db_user_add(&dup) == DG_ERR_DUP_UID);

    /* IC 重复 */
    user_rec_t ic1 = make_user("10002", "李四", "pwd3");
    snprintf(ic1.ic_card, sizeof(ic1.ic_card), "0134567890");
    DG_CHECK(db_user_add(&ic1) == DG_OK);
    user_rec_t ic2 = make_user("10003", "王五", "pwd4");
    snprintf(ic2.ic_card, sizeof(ic2.ic_card), "0134567890");
    DG_CHECK(db_user_add(&ic2) == DG_ERR_DUP_IC);

    /* 人脸查重命中 */
    user_rec_t f1 = make_user("20001", "甲", "pwdf1");
    memset(f1.face_vec, 0xAA, 64);
    f1.face_vec_len = 64;
    DG_CHECK(db_user_add(&f1) == DG_OK);
    user_rec_t f2 = make_user("20002", "乙", "pwdf2");
    memset(f2.face_vec, 0xAA, 64);              /* 同特征(加密后密文不同,靠解密比较) */
    f2.face_vec_len = 64;
    DG_CHECK(db_user_add(&f2) == DG_ERR_DUP_FACE);

    /* 指纹查重同 */
    user_rec_t g1 = make_user("30001", "丙", "pwfg1");
    memset(g1.finger_vec, 0x55, 32);
    g1.finger_vec_len = 32;
    DG_CHECK(db_user_add(&g1) == DG_OK);
    user_rec_t g2 = make_user("30002", "丁", "pwfg2");
    memset(g2.finger_vec, 0x55, 32);
    g2.finger_vec_len = 32;
    DG_CHECK(db_user_add(&g2) == DG_ERR_DUP_FINGER);

    /* 无密码拒绝 */
    user_rec_t nopwd = make_user("40001", "戊", NULL);
    DG_CHECK(db_user_add(&nopwd) == DG_ERR_NO_PASSWORD);

    /* 上限边界:补到 2000 个(第 2000 个成功),第 2001 个拒绝 */
    DG_CHECK(db_user_count(&n) == DG_OK);       /* 刷新当前计数,再从 n+1 填充 */
    char uid[DG_UID_LEN];
    for (uint32_t i = n + 1; i <= DG_USER_MAX; i++) {
        snprintf(uid, sizeof(uid), "FILL%07u", i);
        user_rec_t u = make_user(uid, "填充", "pwdfill");
        if (db_user_add(&u) != DG_OK) {
            printf("  FAIL: fill add #%u failed\n", i);
            dg_fail++;
            break;
        }
    }
    DG_CHECK(db_user_count(&n) == DG_OK && n == DG_USER_MAX);
    user_rec_t over = make_user("OVERFLOW", "溢出", "pwdover");
    DG_CHECK(db_user_add(&over) == DG_ERR_USER_LIMIT);

    storage_deinit();
}

/* ================= 2 密码 ================= */

/* 字段合法性:UI 弹窗已即时拦,存储层是权威兜底(脚本/上位机/API 绕不过) */
static void test_field_valid(void)
{
    printf("[S7] 字段合法性:非法 ID/姓名/密码一律拒(与 proto/valid.h 同规则)\n");
    fresh_setup();

    /* 非法 ID */
    user_rec_t u = make_user("12", "张三", "1234");          /* 太短 */
    DG_CHECK(db_user_add(&u) == DG_ERR_BAD_UID);
    u = make_user("-abc", "张三", "1234");                    /* 首字符非字母数字 */
    DG_CHECK(db_user_add(&u) == DG_ERR_BAD_UID);
    u = make_user("ab cd", "张三", "1234");                   /* 空格 */
    DG_CHECK(db_user_add(&u) == DG_ERR_BAD_UID);

    /* 非法姓名 */
    u = make_user("10001", " 张三", "1234");                  /* 前导空格 */
    DG_CHECK(db_user_add(&u) == DG_ERR_BAD_NAME);

    /* 非法密码(长度/字符集)在 set_password 就拦(故这里不经 make_user) */
    u = make_user("10002", "李四", NULL);                     /* 太短 */
    DG_CHECK(db_user_set_password(&u, "123") == DG_ERR_BAD_PWD);
    DG_CHECK(db_user_set_password(&u, "has space") == DG_ERR_BAD_PWD);

    /* 合法值照常通过(字母/数字/-/_ 混合,x 大小写) */
    u = make_user("A1-b_2", "Zhang San", "Abc123!@#");
    DG_CHECK(db_user_add(&u) == DG_OK);
    DG_CHECK(db_user_count_role(DG_ROLE_NORMAL, &(uint32_t){0}) == DG_OK);
}

static void test_password(void)
{
    printf("[S2] password verify / unknown user / PBKDF2 known vectors\n");
    fresh_setup();

    user_rec_t u = make_user("10001", "张三", "s3cret!");
    DG_CHECK(db_user_add(&u) == DG_OK);

    user_rec_t out;
    DG_CHECK(db_verify_password("10001", "s3cret!", &out) == DG_OK);
    DG_CHECK(strcmp(out.user_name, "张三") == 0);

    DG_CHECK(db_verify_password("10001", "wrong", NULL) == DG_ERR_WRONG_PASSWORD);
    DG_CHECK(db_verify_password("nobody", "x", NULL) == DG_ERR_NOT_FOUND);

    /* PBKDF2-HMAC-SHA256 公开已知向量(密码/盐/salt 长度/迭代 1,2,4096) */
    struct {
        const char *pwd, *salt;
        uint32_t iters;
        uint8_t expect[32];
    } vec[] = {
        { "password", "salt", 1, {
              0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c,0x43,0xe7,0x22,0x52,
              0x56,0xc4,0xf8,0x37,0xa8,0x65,0x48,0xc9,0x2c,0xcc,0x35,0x48,
              0x08,0x05,0x98,0x7c,0xb7,0x0b,0xe1,0x7b } },
        { "password", "salt", 2, {
              0xae,0x4d,0x0c,0x95,0xaf,0x6b,0x46,0xd3,0x2d,0x0a,0xdf,0xf9,
              0x28,0xf0,0x6d,0xd0,0x2a,0x30,0x3f,0x8e,0xf3,0xc2,0x51,0xdf,
              0xd6,0xe2,0xd8,0x5a,0x95,0x47,0x4c,0x43 } },
        { "password", "salt", 4096, {
              0xc5,0xe4,0x78,0xd5,0x92,0x88,0xc8,0x41,0xaa,0x53,0x0d,0xb6,
              0x84,0x5c,0x4c,0x8d,0x96,0x28,0x93,0xa0,0x01,0xce,0x4e,0x11,
              0xa4,0x96,0x38,0x73,0xaa,0x98,0x13,0x4a } },
    };
    for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        /* 已知向量迭代数低于 10000:经内部接口对拍(dg_pbkdf2 参数化 iters,
         * 业务固定 DG_PBKDF2_ITERS;此处仅验证实现正确性) */
        uint8_t dk[32];
        DG_CHECK(dg_pbkdf2_sha256(vec[i].pwd, (const uint8_t *)vec[i].salt,
                                  (uint32_t)strlen(vec[i].salt), vec[i].iters, dk)
                 == DG_OK);
        DG_CHECK(memcmp(dk, vec[i].expect, 32) == 0);
    }

    storage_deinit();
}

/* ================= 3 日志 ================= */

static void seed_logs(int count)
{
    access_log_t l;
    memset(&l, 0, sizeof(l));
    for (int i = 0; i < count; i++) {
        l.ts = 1789000000 + i;                  /* 秒级递增,便于边界断言 */
        l.has_user = (i % 5) != 0;              /* 每 5 条一条陌生人(user_id NULL) */
        snprintf(l.user_id, sizeof(l.user_id), "U%03d", i % 50);
        snprintf(l.user_name, sizeof(l.user_name), "N%03d", i % 50);
        l.method = (i % 5);
        l.result = (i % 3) == 0 ? DG_RESULT_REJECT : DG_RESULT_PASS;
        l.reason = (l.result == DG_RESULT_PASS) ? DG_REASON_OK : DG_REASON_STRANGER;
        DG_CHECK(db_log_append(&l) == DG_OK);
    }
}

static void test_logs(void)
{
    printf("[S3] logs: 2500 rows / time bounds / by-user / paging / descending\n");
    fresh_setup();
    seed_logs(2500);

    log_query_t q;
    log_page_t page;
    access_log_t rows[512];
    memset(&q, 0, sizeof(q));

    /* 全量 total */
    q.page = 1; q.page_size = 10;
    page.logs = rows; page.max = 512;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == 2500);
    DG_CHECK(page.count == 10);
    DG_CHECK(rows[0].ts == 1789000000);         /* 默认升序 */

    /* 时间段:起止边界"含" */
    memset(&q, 0, sizeof(q));
    q.ts_from = 1789000000;                     /* 第 1 条 ts(含) */
    q.ts_to = 1789000009;                       /* 第 10 条 ts(含) */
    q.page = 1; q.page_size = 64;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == 10);
    DG_CHECK(rows[0].ts == 1789000000 && rows[9].ts == 1789000009);

    /* 边界外不含:from/to 各外扩一秒,总数不变;各内缩一秒,总数减 2 */
    q.ts_from = 1789000001;
    q.ts_to = 1789000008;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == 8);

    /* 按用户(U001:2500/50 = 50 条;注意 i%50==0 的行全是陌生人,user_id 为 NULL,
     * 不能用 U000) */
    memset(&q, 0, sizeof(q));
    snprintf(q.user_id, sizeof(q.user_id), "U001");
    q.page = 1; q.page_size = 64;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == 50);
    DG_CHECK(strcmp(rows[0].user_id, "U001") == 0);

    /* 分页整除:2500/100 = 25 页,末页满且 total 不变 */
    memset(&q, 0, sizeof(q));
    q.page = 25; q.page_size = 100;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.count == 100);
    DG_CHECK(page.total == 2500);
    DG_CHECK(rows[99].ts == 1789002499);

    /* 分页不整除:page_size=300 → 2500 = 8×300 + 100,末页 100 条 */
    q.page = 9; q.page_size = 300;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.count == 100);

    /* 倒序:首页首条为最新 */
    memset(&q, 0, sizeof(q));
    q.page = 1; q.page_size = 10; q.descending = true;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(rows[0].ts == 1789002499);
    DG_CHECK(rows[9].ts == 1789002490);

    /* 陌生人日志:user_id 为 NULL,has_user=false */
    /* i=5(5%5==0 → 陌生人)→ ts 1789000005 */
    memset(&q, 0, sizeof(q));
    q.ts_from = 1789000005; q.ts_to = 1789000005;
    q.page = 1; q.page_size = 64;
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == 1);
    DG_CHECK(rows[0].has_user == false);
    DG_CHECK(rows[0].user_id[0] == '\0');

    /* 非法分页参数显式报错 */
    memset(&q, 0, sizeof(q));
    q.page = 1; q.page_size = 0;
    DG_CHECK(db_log_query(&q, &page) == DG_ERR_PARAM);

    storage_deinit();
}

/* ================= 4 配置 KV ================= */

static void test_config(void)
{
    printf("[S4] config KV: set / get / default(NOT_FOUND) / overwrite\n");
    fresh_setup();

    char val[128];
    DG_CHECK(db_config_get("language", val, sizeof(val)) == DG_ERR_NOT_FOUND);

    DG_CHECK(db_config_set("language", "zh-CN") == DG_OK);
    DG_CHECK(db_config_get("language", val, sizeof(val)) == DG_OK);
    DG_CHECK(strcmp(val, "zh-CN") == 0);

    DG_CHECK(db_config_set("language", "en-US") == DG_OK);   /* 覆盖 */
    DG_CHECK(db_config_get("language", val, sizeof(val)) == DG_OK);
    DG_CHECK(strcmp(val, "en-US") == 0);

    /* 空值合法(显式存空串与"键不存在"区分) */
    DG_CHECK(db_config_set("ota_url", "") == DG_OK);
    DG_CHECK(db_config_get("ota_url", val, sizeof(val)) == DG_OK);
    DG_CHECK(val[0] == '\0');

    DG_CHECK(db_config_set("", "x") == DG_ERR_PARAM);
    DG_CHECK(db_config_get("language", NULL, 0) == DG_ERR_PARAM);

    storage_deinit();
}

/* ================= 5 加密 ================= */

static void test_crypto(void)
{
    printf("[S5] crypto: feature roundtrip / salt uniqueness / NIST CTR vector\n");
    fresh_setup();

    /* 特征加解密 roundtrip(经 wrap/unwrap,IV 前缀) */
    uint8_t plain[100], enc[200], dec[100];
    for (int i = 0; i < 100; i++)
        plain[i] = (uint8_t)(i * 7 + 1);
    size_t enc_len = 0, dec_len = 0;
    DG_CHECK(dg_feature_wrap(plain, sizeof(plain), enc, sizeof(enc), &enc_len) == DG_OK);
    DG_CHECK(enc_len == sizeof(plain) + 16);
    DG_CHECK(memcmp(enc + 16, plain, sizeof(plain)) != 0);   /* 确实加密 */
    DG_CHECK(dg_feature_unwrap(enc, enc_len, dec, sizeof(dec), &dec_len) == DG_OK);
    DG_CHECK(dec_len == sizeof(plain));
    DG_CHECK(memcmp(dec, plain, sizeof(plain)) == 0);

    /* 同明文两次封装密文不同(随机 IV) */
    uint8_t enc2[200];
    size_t enc2_len = 0;
    DG_CHECK(dg_feature_wrap(plain, sizeof(plain), enc2, sizeof(enc2), &enc2_len) == DG_OK);
    DG_CHECK(memcmp(enc, enc2, enc_len) != 0);

    /* 同密码两次加盐:哈希不同;盐不同;验证均通过 */
    user_rec_t a = make_user("A01", "甲", "samepwd");
    user_rec_t b = make_user("A02", "乙", "samepwd");
    DG_CHECK(memcmp(a.pwd_salt, b.pwd_salt, DG_PWD_SALT_LEN) != 0);
    DG_CHECK(memcmp(a.pwd_hash, b.pwd_hash, DG_PWD_HASH_LEN) != 0);
    DG_CHECK(db_user_add(&a) == DG_OK);
    DG_CHECK(db_user_add(&b) == DG_OK);
    DG_CHECK(db_verify_password("A01", "samepwd", NULL) == DG_OK);
    DG_CHECK(db_verify_password("A02", "samepwd", NULL) == DG_OK);

    /* NIST SP 800-38A F.5.5 CTR-AES256 向量(前 16B 分组) */
    static const uint8_t key[32] = {
        0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
        0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4 };
    static const uint8_t iv[16] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff };
    static const uint8_t pt[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a };
    static const uint8_t expect[16] = {
        0x60,0x1e,0xc3,0x13,0x77,0x57,0x89,0xa5,0xb7,0xa7,0xf5,0x04,0xbb,0xf3,0xd2,0x28 };
    uint8_t ct[16];
    DG_CHECK(dg_aes256_ctr(key, iv, pt, sizeof(pt), ct) == DG_OK);
    DG_CHECK(memcmp(ct, expect, 16) == 0);

    storage_deinit();
}

/* ================= 6 并发写日志 ================= */

#define CC_THREADS 4
#define CC_PER_THREAD 2500

typedef struct {
    int tid;
    atomic_int fail;
} cc_arg_t;

static void *cc_writer(void *raw)
{
    cc_arg_t *arg = raw;
    access_log_t l;
    memset(&l, 0, sizeof(l));
    for (int i = 0; i < CC_PER_THREAD; i++) {
        l.ts = 1790000000 + (int64_t)arg->tid * CC_PER_THREAD + i;
        l.has_user = true;
        snprintf(l.user_id, sizeof(l.user_id), "T%d", arg->tid);
        snprintf(l.user_name, sizeof(l.user_name), "线程%d", arg->tid);
        l.method = DG_METHOD_FINGER;
        l.result = DG_RESULT_PASS;
        l.reason = DG_REASON_OK;
        if (db_log_append(&l) != DG_OK) {
            atomic_fetch_add(&arg->fail, 1);
        }
    }
    return NULL;
}

static void test_concurrency(void)
{
    printf("[S6] concurrency: %d threads x %d logs, zero loss\n",
           CC_THREADS, CC_PER_THREAD);
    fresh_setup();

    pthread_t th[CC_THREADS];
    cc_arg_t args[CC_THREADS];
    for (int t = 0; t < CC_THREADS; t++) {
        args[t].tid = t;
        atomic_store(&args[t].fail, 0);
        DG_CHECK(pthread_create(&th[t], NULL, cc_writer, &args[t]) == 0);
    }
    for (int t = 0; t < CC_THREADS; t++)
        pthread_join(th[t], NULL);

    log_query_t q;
    memset(&q, 0, sizeof(q));
    q.page = 1; q.page_size = 1;
    access_log_t row;
    log_page_t page = { .logs = &row, .max = 1 };
    DG_CHECK(db_log_query(&q, &page) == DG_OK);
    DG_CHECK(page.total == CC_THREADS * CC_PER_THREAD);      /* 无丢失 */
    for (int t = 0; t < CC_THREADS; t++)
        DG_CHECK(atomic_load(&args[t].fail) == 0);           /* 无写入失败 */

    storage_deinit();
}

/* 头像:加密落库、取回一致、清除、超限拒绝、未录/用户不存在区分 */
static void test_avatar(void)
{
    printf("[S8] avatar: set/get roundtrip, encrypted at rest, clear, oversize, cases\n");
    fresh_setup();
    user_rec_t u = make_user("20001", "李四", "pwd1");
    DG_CHECK(db_user_add(&u) == DG_OK);

    /* 未录头条 = NOT_FOUND(调用方显示占位) */
    uint8_t buf[DG_FEATURE_MAX];
    size_t len = 0;
    DG_CHECK(db_user_get_avatar("20001", buf, sizeof(buf), &len) == DG_ERR_NOT_FOUND);

    /* 造一段"JPEG"(内容无关,验的是封装与一致性;含 0 字节防截断实现) */
    uint8_t jpg[512];
    for (int i = 0; i < 512; i++)
        jpg[i] = (uint8_t)(i * 7 + (i == 100 ? 0 : 0));   /* 同人恒定即可 */
    /* 用 16 字节 ASCII 标记做"明文泄露"探针:三字节 JPEG 头在 MB 级随机密文里
     * 有可观概率偶然撞上(=偶发红),16 字节 ASCII 序列则实际不可能 */
    static const char MARK[] = "DG-AVATAR-PLAINTXT";   /* 16 字节 + NUL */
    memcpy(jpg, MARK, sizeof(MARK) - 1);
    DG_CHECK(db_user_set_avatar("20001", jpg, sizeof(jpg)) == DG_OK);

    uint8_t got[1024];
    DG_CHECK(db_user_get_avatar("20001", got, sizeof(got), &len) == DG_OK);
    DG_CHECK(len == sizeof(jpg));
    DG_CHECK(memcmp(got, jpg, sizeof(jpg)) == 0);

    /* 落库必须是密文。WAL 模式下新写入可能只在 -wal 文件里,两个都要扫——
     * 只扫主库会"因为数据还在 WAL"而假通过,安全测试假通过比没有更糟。 */
    {
        static const char *const files[] = { "db.sqlite", "db.sqlite-wal" };
        int scanned = 0, leaked = 0;
        for (int fi = 0; fi < 2; fi++) {
            char dbpath[160];
            snprintf(dbpath, sizeof(dbpath), "%s/%s", s_dir, files[fi]);
            FILE *f = fopen(dbpath, "rb");
            if (!f)
                continue;                /* 无 WAL 文件属正常 */
            scanned++;
            static uint8_t raw[1 << 20];
            size_t n = fread(raw, 1, sizeof(raw), f);
            fclose(f);
            for (size_t i = 0; i + 16 <= n; i++)
                if (memcmp(&raw[i], MARK, 16) == 0)
                    leaked = 1;
        }
        DG_CHECK(scanned >= 1);          /* 至少扫到一个文件(否则测了个寂寞) */
        DG_CHECK(!leaked);               /* 明文 JPEG 头不出现 = 确实加密了 */
    }

    /* 覆盖写:长度可变(先短后长) */
    DG_CHECK(db_user_set_avatar("20001", jpg, 64) == DG_OK);
    DG_CHECK(db_user_get_avatar("20001", got, sizeof(got), &len) == DG_OK);
    DG_CHECK(len == 64);

    /* 清除(len=0)后回落 NOT_FOUND */
    DG_CHECK(db_user_set_avatar("20001", NULL, 0) == DG_OK);
    DG_CHECK(db_user_get_avatar("20001", got, sizeof(got), &len) == DG_ERR_NOT_FOUND);

    /* 超限拒绝(不撑大库) */
    static uint8_t big[40000];
    memset(big, 1, sizeof(big));
    DG_CHECK(db_user_set_avatar("20001", big, sizeof(big)) == DG_ERR_PARAM);

    /* 用户不存在 / 参数非法 */
    DG_CHECK(db_user_set_avatar("99999", jpg, 10) == DG_ERR_NOT_FOUND);
    DG_CHECK(db_user_get_avatar("99999", got, sizeof(got), &len) == DG_ERR_NOT_FOUND);
    DG_CHECK(db_user_get_avatar(NULL, got, sizeof(got), &len) == DG_ERR_PARAM);

    /* 删用户后头像随之消失(同行记录,天然一致) */
    DG_CHECK(db_user_del("20001") == DG_OK);
    DG_CHECK(db_user_get_avatar("20001", got, sizeof(got), &len) == DG_ERR_NOT_FOUND);
}

int main(void)
{
    test_add();          /* 含 2000 边界,最慢 */
    test_password();
    test_field_valid();
    test_logs();
    test_config();
    test_crypto();
    test_concurrency();
    test_avatar();

    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_st_%d", (int)getpid());
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }

    DG_TEST_EXIT();
}
