/*
 * test_cfg.c — 配置体系测试(Phase 4)
 *
 * 覆盖:坏 json/缺键/类型错不崩且回退默认;json 与 DB 的覆盖优先级
 * (DB 用户设置 > json 出厂值 > 代码默认);cfg_set 校验与持久化。
 */
#include "cfg.h"
#include "dg_test.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[64];

/* 写临时 json 供 cfg_load 用 */
static const char *write_json(const char *content)
{
    static char path[96];
    snprintf(path, sizeof(path), "%s/device.json", s_dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        exit(1);
    fputs(content, f);
    fclose(f);
    return path;
}

static void setup_db(void)
{
    storage_deinit();
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_cfg_%d", (int)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        exit(1);
    char db[96], key[96];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    if (storage_init(db, key) != DG_OK)
        exit(1);
}

/* ---- C1 缺失文件 / 坏 json / 缺键 / 类型错:不崩且回退默认 ---- */
static void test_bad_inputs_fall_back(void)
{
    printf("[C1] missing file / broken json / missing key / wrong type\n");

    /* 文件不存在:WARN + 全默认 */
    setup_db();
    DG_CHECK(cfg_load("/tmp/dg_cfg_nonexistent.json") == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);
    DG_CHECK(cfg_get()->standby_timeout_s == 30);
    DG_CHECK(cfg_get()->web_port == 8080);

    /* 坏 json:截断 + 非法 token */
    setup_db();
    DG_CHECK(cfg_load(write_json("{ \"access\": { \"door_open_ms\": ")) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);
    DG_CHECK(cfg_load(write_json("not json at all {{{")) == DG_OK);
    DG_CHECK(cfg_get()->standby_timeout_s == 30);

    /* 空对象 = 全部缺键:默认 */
    DG_CHECK(cfg_load(write_json("{}")) == DG_OK);
    DG_CHECK(cfg_get()->face_dup_threshold > 0.899
             && cfg_get()->face_dup_threshold < 0.901);
    DG_CHECK(strcmp(cfg_get()->language, "zh-CN") == 0);

    /* 类型错:数字写成字符串、布尔写成数字 → 回退默认 */
    DG_CHECK(cfg_load(write_json(
        "{ \"access\": { \"door_open_ms\": \"fast\" },"
        "  \"ui\": { \"standby_timeout_s\": true },"
        "  \"face\": { \"face_dup_threshold\": \"high\" } }")) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);
    DG_CHECK(cfg_get()->standby_timeout_s == 30);
    DG_CHECK(cfg_get()->face_dup_threshold > 0.899);

    /* 越界值:回退默认 */
    DG_CHECK(cfg_load(write_json(
        "{ \"access\": { \"door_open_ms\": 99999, \"pwd_fail_lock_n\": 0 },"
        "  \"ui\": { \"standby_timeout_s\": 3 },"
        "  \"network\": { \"web_port\": 80 } }")) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 3000);
    DG_CHECK(cfg_get()->pwd_fail_lock_n == 5);
    DG_CHECK(cfg_get()->standby_timeout_s == 30);
    DG_CHECK(cfg_get()->web_port == 8080);
}

/* ---- C2 正常加载:合法值全部生效 ---- */
static void test_valid_load(void)
{
    printf("[C2] valid json values applied\n");
    setup_db();
    DG_CHECK(cfg_load(write_json(
        "{ \"access\": { \"door_open_ms\": 5000, \"pwd_fail_lock_n\": 3,"
        "                \"pwd_fail_lock_s\": 120 },"
        "  \"face\": { \"face_dup_threshold\": 0.85 },"
        "  \"ui\": { \"standby_timeout_s\": 45, \"language\": \"en-US\" },"
        "  \"network\": { \"web_port\": 8081, \"ota_port\": 9001,"
        "                \"ntp_server\": \"pool.ntp.org\" } }")) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 5000);
    DG_CHECK(cfg_get()->pwd_fail_lock_n == 3);
    DG_CHECK(cfg_get()->pwd_fail_lock_s == 120);
    DG_CHECK(cfg_get()->face_dup_threshold > 0.849
             && cfg_get()->face_dup_threshold < 0.851);
    DG_CHECK(cfg_get()->standby_timeout_s == 45);
    DG_CHECK(strcmp(cfg_get()->language, "en-US") == 0);
    DG_CHECK(cfg_get()->web_port == 8081);
    DG_CHECK(cfg_get()->ota_port == 9001);
    DG_CHECK(strcmp(cfg_get()->ntp_server, "pool.ntp.org") == 0);
}

/* ---- C3 优先级:DB 用户设置 > json 出厂值;非法 DB 值不生效 ---- */
static void test_db_overrides_json(void)
{
    printf("[C3] DB device_config overrides device.json\n");
    setup_db();

    /* json 定义 door=5000;DB 覆盖为 2000 */
    DG_CHECK(db_config_set("door_open_ms", "2000") == DG_OK);
    DG_CHECK(db_config_set("standby_timeout_s", "20") == DG_OK);
    DG_CHECK(db_config_set("language", "en-US") == DG_OK);
    DG_CHECK(cfg_load(write_json(
        "{ \"access\": { \"door_open_ms\": 5000 },"
        "  \"ui\": { \"standby_timeout_s\": 45, \"language\": \"zh-CN\" } }"))
         == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 2000);      /* DB 赢 json */
    DG_CHECK(cfg_get()->standby_timeout_s == 20);
    DG_CHECK(strcmp(cfg_get()->language, "en-US") == 0);

    /* DB 无该键 → json 生效 */
    DG_CHECK(cfg_get()->pwd_fail_lock_n == 5);

    /* DB 里非法值(越界/非数字)→ 不生效,回落 json 值 */
    DG_CHECK(db_config_set("web_port", "99999") == DG_OK);
    DG_CHECK(db_config_set("face_dup_threshold", "abc") == DG_OK);
    DG_CHECK(cfg_load(write_json(
        "{ \"network\": { \"web_port\": 8081 },"
        "  \"face\": { \"face_dup_threshold\": 0.85 } }")) == DG_OK);
    DG_CHECK(cfg_get()->web_port == 8081);          /* 回落 json,不是默认 8080 */
    DG_CHECK(cfg_get()->face_dup_threshold > 0.849);

    /* cfg_set:校验拒绝越界;合法值写 DB 并刷快照;reload 后仍在 */
    DG_CHECK(cfg_set_int("door_open_ms", 12345) == DG_ERR_PARAM);
    DG_CHECK(cfg_set_int("door_open_ms", 4500) == DG_OK);
    DG_CHECK(cfg_set_str("language", "fr-FR") == DG_ERR_PARAM);
    DG_CHECK(cfg_set_str("language", "en-US") == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 4500);
    /* 模拟重启:同一 DB 重新加载(不删库,持久化的意义就在此) */
    DG_CHECK(cfg_load(write_json("{}")) == DG_OK);
    DG_CHECK(cfg_get()->door_open_ms == 4500);      /* 持久化生效 */
    DG_CHECK(strcmp(cfg_get()->language, "en-US") == 0);

    /* 未知键显式拒绝 */
    DG_CHECK(cfg_set_int("no_such_key", 1) == DG_ERR_PARAM);
    DG_CHECK(cfg_set_str("no_such_key", "x") == DG_ERR_PARAM);
}

int main(void)
{
    test_bad_inputs_fall_back();
    test_valid_load();
    test_db_overrides_json();

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }
    storage_deinit();

    DG_TEST_EXIT();
}
