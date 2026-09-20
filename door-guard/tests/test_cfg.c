/*
 * test_cfg.c — 配置服务测试(架构 v2 M2①:双文件)
 *
 * 覆盖:缺失/坏 json/类型错/越界不崩且回退;default 模板与 cur 覆盖的加载序;
 * 首启迁移(DB→cur 一次性,此后 DB 冻结);cfg_set 校验+防抖+cfg_flush 落盘;
 * cfg_reset_key / cfg_reset_all 恢复默认;未知键显式拒绝。
 */
#include "cfg.h"
#include "dg_test.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[64];
static char s_def[96], s_cur[96];

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static void setup(int fresh_db)
{
    storage_deinit();
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_cfg_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        exit(1);
    if (fresh_db) {
        char db[96], key[96];
        snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
        snprintf(key, sizeof(key), "%s/dg.key", s_dir);
        if (storage_init(db, key) != DG_OK)
            exit(1);
    }
    snprintf(s_def, sizeof(s_def), "%s/default.json", s_dir);
    snprintf(s_cur, sizeof(s_cur), "%s/cur_config.json", s_dir);
}

/* ---- C1 缺失文件 / 坏 json / 类型错 / 越界:不崩且回退 ---- */
static void test_bad_inputs_fall_back(void)
{
    printf("[C1] missing files / broken json / wrong type / out of range\n");

    /* 双缺失:全内置默认;cur 首启迁移生成(DB 空 → 空对象文件) */
    setup(1);
    DG_CHECK(cfg_load("/tmp/dg_nonexistent_def.json", s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);
    DG_CHECK(cfg_get()->standby_timeout_s == 30);
    DG_CHECK(cfg_get()->web_port == 8080);
    DG_CHECK(access(s_cur, F_OK) == 0);             /* 首启已生成 cur */

    /* 出厂模板坏 json:回退内置默认 */
    setup(1);
    DG_CHECK(write_file(s_def, "{ \"access\": { \"door_open_ms\": ") == 0);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);

    /* 现用配置坏 json:按空处理,回落模板值 */
    DG_CHECK(write_file(s_def,
        "{ \"access\": { \"door_open_ms\": 5000 } }") == 0);
    DG_CHECK(write_file(s_cur, "not json {{{") == 0);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 5000);      /* 模板生效 */

    /* 类型错 / 越界(模板层):回退 */
    DG_CHECK(write_file(s_def,
        "{ \"access\": { \"door_open_ms\": \"fast\", \"pwd_fail_lock_n\": 0 },"
        "  \"ui\": { \"standby_timeout_s\": true, \"language\": 7 },"
        "  \"network\": { \"web_port\": 80 } }") == 0);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);
    DG_CHECK(cfg_get()->pwd_fail_lock_n == 5);
    DG_CHECK(cfg_get()->standby_timeout_s == 30);   /* bool 也算类型错 */
    DG_CHECK(strcmp(cfg_get()->language, "zh-CN") == 0);
    DG_CHECK(cfg_get()->web_port == 8080);
}

/* ---- C2 加载序:内置默认 → default 模板 → cur 覆盖 ---- */
static void test_layering(void)
{
    printf("[C2] default template + cur overlay layering\n");
    setup(1);
    DG_CHECK(write_file(s_def,
        "{ \"access\": { \"door_open_ms\": 5000 },"
        "  \"ui\": { \"language\": \"en-US\", \"standby_timeout_s\": 45 },"
        "  \"network\": { \"web_port\": 8081, \"ota_port\": 9001,"
        "                \"ntp_server\": \"pool.ntp.org\" },"
        "  \"face\": { \"face_dup_threshold\": 0.85 } }") == 0);
    DG_CHECK(write_file(s_cur,
        "{ \"access\": { \"door_open_ms\": 2000 } }") == 0);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 2000);      /* cur 赢模板 */
    DG_CHECK(strcmp(cfg_get()->language, "en-US") == 0);
    DG_CHECK(cfg_get()->standby_timeout_s == 45);
    DG_CHECK(cfg_get()->web_port == 8081);
    DG_CHECK(cfg_get()->ota_port == 9001);
    DG_CHECK(strcmp(cfg_get()->ntp_server, "pool.ntp.org") == 0);
    DG_CHECK(cfg_get()->face_dup_threshold > 0.849
             && cfg_get()->face_dup_threshold < 0.851);
    /* cur 没有的键回落模板;模板也没有的键回落内置 */
    DG_CHECK(cfg_get()->pwd_fail_lock_n == 5);
}

/* ---- C3 首启迁移:DB → cur 一次性;此后 DB 冻结 ---- */
static void test_db_migration_freeze(void)
{
    printf("[C3] one-shot DB migration then DB freeze\n");
    setup(1);
    DG_CHECK(db_config_set("door_open_ms", "2000") == DG_OK);
    DG_CHECK(db_config_set("language", "en-US") == DG_OK);
    DG_CHECK(db_config_set("face_match_threshold", "0.50") == DG_OK);
    DG_CHECK(db_config_set("web_port", "99999") == DG_OK);      /* 非法:不迁移 */
    DG_CHECK(db_config_set("face_dup_threshold", "abc") == DG_OK); /* 非数字:不迁移 */
    DG_CHECK(db_config_set("web_user", "admin") == DG_OK);      /* 非业务键:不迁移 */

    DG_CHECK(write_file(s_def,
        "{ \"access\": { \"door_open_ms\": 5000 } }") == 0);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 2000);      /* DB 迁移值赢模板 */
    DG_CHECK(strcmp(cfg_get()->language, "en-US") == 0);
    DG_CHECK(cfg_get()->face_match_threshold > 0.499);
    DG_CHECK(cfg_get()->web_port == 8080);          /* 非法值未迁移,走默认 */
    DG_CHECK(access(s_cur, F_OK) == 0);             /* cur 已生成 */

    /* 冻结:此后 DB 改动不再进配置(重新加载仍是迁移值) */
    DG_CHECK(db_config_set("door_open_ms", "7777") == DG_OK);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 2000);
}

/* ---- C4 set / flush 落盘 / reset 恢复默认 ---- */
static void test_set_flush_reset(void)
{
    printf("[C4] set + flush persistence + reset key/all\n");
    setup(1);
    DG_CHECK(write_file(s_def,
        "{ \"access\": { \"door_open_ms\": 5000 },"
        "  \"ui\": { \"language\": \"zh-CN\" } }") == 0);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);

    /* set:校验 → 快照即时生效 */
    DG_CHECK(cfg_set_int("door_open_ms", 12345) == DG_ERR_PARAM);
    DG_CHECK(cfg_set_int("door_open_ms", 4500) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 4500);
    DG_CHECK(cfg_set_str("language", "fr-FR") == DG_ERR_PARAM);
    DG_CHECK(cfg_set_str("language", "en-US") == DG_OK);

    /* flush:原子落盘,文件内容可见;重启语义(重新加载)后仍在 */
    DG_CHECK(cfg_flush() == DG_OK);
    DG_CHECK(cfg_load(s_def, s_cur) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 4500);
    DG_CHECK(strcmp(cfg_get()->language, "en-US") == 0);

    /* reset_key:删 cur 键 → 回落模板值(5000,不是内置 3000) */
    DG_CHECK(cfg_reset_key("door_open_ms") == DG_OK);
    DG_CHECK(cfg_flush() == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 5000);

    /* reset_all:cur 清空 → 全部回落模板/内置 */
    DG_CHECK(cfg_set_str("language", "en-US") == DG_OK);
    DG_CHECK(cfg_reset_all() == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 5000);      /* 模板值 */
    DG_CHECK(strcmp(cfg_get()->language, "zh-CN") == 0);  /* 内置值 */

    /* 未知键显式拒绝 */
    DG_CHECK(cfg_set_int("no_such_key", 1) == DG_ERR_PARAM);
    DG_CHECK(cfg_set_str("no_such_key", "x") == DG_ERR_PARAM);
    DG_CHECK(cfg_reset_key("no_such_key") == DG_ERR_PARAM);
}

int main(void)
{
    test_bad_inputs_fall_back();
    test_layering();
    test_db_migration_freeze();
    test_set_flush_reset();

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }
    storage_deinit();

    DG_TEST_EXIT();
}
