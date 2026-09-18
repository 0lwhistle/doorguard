/*
 * test_vision_backend.c — 视觉后端契约回归(vision_backend.h)
 *
 * 为什么单独一个用例:后端可插拔(换 ROCKIVA 模型 / 换 rknn 开源模型)是
 * 一条长期契约,它的四件事必须锁死,否则换模型时才会炸:
 *   1. 注册表:注册/选择/同名覆盖/非法参数
 *   2. 转发:库维护走后端 lib_add/lib_del;模式变更走后端 on_mode
 *   3. 注入:后端 compare 被交给 storage(查重命中即拒,错误码 DG_ERR_DUP_FACE)
 *   4. 口径:model_tag 与 device_config.face_model_tag 不一致时
 *      features_compatible()=false(后端据此不发命中:宁可不开门,不可错开门)
 *
 * 全部用假后端(mock ops)驱动,不依赖相机/NPU/ROCKIVA。
 */
#include "dg_test.h"
#include "cfg.h"
#include "event_bus.h"
#include "storage.h"
#include "tasker.h"
#include "vision_backend.h"
#include "vision_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[128];

/* ---- 假后端:记录调用,便于断言转发是否到达 ---- */

static int s_start_calls;
static int s_add_calls;
static int s_del_calls;
static int s_mode_calls;
static dg_vision_mode_t s_last_mode;
static char s_last_uid[DG_UID_LEN];
static bool s_fail_start;

static int mock_start(bool enable_mock)
{
    (void)enable_mock;
    s_start_calls++;
    return s_fail_start ? DG_ERR_IO : DG_OK;
}

static int mock_lib_add(const char *uid, const uint8_t *feat, uint16_t len)
{
    (void)uid; (void)feat; (void)len;
    s_add_calls++;
    return DG_OK;
}

static int mock_lib_del(const char *uid)
{
    (void)uid;
    s_del_calls++;
    return DG_OK;
}

/* 比较器:长度一致即判"重复"(让查重路径可预测地命中) */
static int mock_cmp(const uint8_t *a, uint16_t alen,
                    const uint8_t *b, uint16_t blen, void *ud)
{
    (void)ud;
    return (a && b && alen == blen) ? 1 : 0;
}

static void mock_on_mode(dg_vision_mode_t mode, const char *uid)
{
    s_mode_calls++;
    s_last_mode = mode;
    snprintf(s_last_uid, sizeof(s_last_uid), "%s", uid ? uid : "");
}

static const vision_backend_ops_t mock_ops = {
    .name = "mock",
    .model_tag = "mock-v1",
    .has_landmarks = true,
    .start = mock_start,
    .lib_add = mock_lib_add,
    .lib_del = mock_lib_del,
    .compare = mock_cmp,
    .on_mode = mock_on_mode,
};

static int wait_true(bool (*fn)(void), int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i += 5) {
        if (fn())
            return 1;
        usleep(5000);
    }
    return 0;
}

static void env_set(const char *k, const char *v)
{
    if (v)
        setenv(k, v, 1);
    else
        unsetenv(k);
}

static bool mode_hook_hit(void) { return s_mode_calls > 0; }

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(tasker_init() == TASK_OK);
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_vbe_%d", (int)getpid());
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", s_dir, s_dir);
    if (system(cmd) != 0)
        return 1;
    char db[192], key[192], json[192];
    snprintf(db, sizeof(db), "%s/db.sqlite", s_dir);
    snprintf(key, sizeof(key), "%s/dg.key", s_dir);
    snprintf(json, sizeof(json), "%s/device.json", s_dir);
    DG_CHECK(storage_init(db, key) == DG_OK);
    /* 配置里点名后端与口径:同时验 face.backend / face.model_tag 两个键的解析 */
    FILE *f = fopen(json, "wb");
    if (!f)
        return 1;
    fputs("{\"face\":{\"backend\":\"mock\",\"model_tag\":\"mock-v1\"}}", f);
    fclose(f);
    cfg_load(json);
    DG_CHECK(vision_service_start() == DG_OK);

    /* ---- 1. 注册表参数校验 ---- */
    DG_CHECK(vision_backend_register(NULL) == DG_ERR_PARAM);
    DG_CHECK(vision_backend_register(&mock_ops) == DG_OK);
    DG_CHECK(vision_backend_register(&mock_ops) == DG_OK);   /* 同名覆盖,不占槽 */
    DG_CHECK(vision_backend_active() == NULL);               /* 未启动:无生效后端 */

    /* ---- 2. 启动与转发 ---- */
    DG_CHECK(vision_backend_start(false) == DG_OK);
    DG_CHECK(s_start_calls == 1);
    DG_CHECK(vision_backend_active() == &mock_ops);
    DG_CHECK(strcmp(vision_backend_name(), "mock") == 0);
    DG_CHECK(strcmp(vision_backend_model_tag(), "mock-v1") == 0);

    uint8_t feat[32];
    memset(feat, 0x5A, sizeof(feat));
    DG_CHECK(vision_service_library_add("10001", feat, sizeof(feat)) == DG_OK);
    DG_CHECK(vision_service_library_remove("10001") == DG_OK);
    DG_CHECK(s_add_calls == 1 && s_del_calls == 1);

    /* 模式变更转发到后端(1:1 要带目标 uid) */
    s_mode_calls = 0;
    DG_CHECK(vision_service_set_mode(DG_VMODE_VERIFY_11, "10001") == DG_OK);
    DG_CHECK(wait_true(mode_hook_hit, 1000));
    DG_CHECK(s_last_mode == DG_VMODE_VERIFY_11);
    DG_CHECK(strcmp(s_last_uid, "10001") == 0);

    /* ---- 3. compare 已注入 storage:同长特征判重 → DG_ERR_DUP_FACE ---- */
    user_rec_t u;
    memset(&u, 0, sizeof(u));
    snprintf(u.user_id, sizeof(u.user_id), "10001");
    snprintf(u.user_name, sizeof(u.user_name), "张三");
    u.role = DG_ROLE_NORMAL;
    u.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    u.face_vec_len = sizeof(feat);
    memcpy(u.face_vec, feat, sizeof(feat));
    DG_CHECK(db_user_set_password(&u, "1234") == DG_OK);
    DG_CHECK(db_user_add(&u) == DG_OK);

    user_rec_t dup;
    memset(&dup, 0, sizeof(dup));
    snprintf(dup.user_id, sizeof(dup.user_id), "10002");
    snprintf(dup.user_name, sizeof(dup.user_name), "李四");
    dup.role = DG_ROLE_NORMAL;
    dup.auth_flags = DG_AUTH_FACE | DG_AUTH_PWD;
    dup.face_vec_len = sizeof(feat);            /* 同长 → mock_cmp 判重 */
    memcpy(dup.face_vec, feat, sizeof(feat));
    DG_CHECK(db_user_set_password(&dup, "1234") == DG_OK);
    DG_CHECK(db_user_add(&dup) == DG_ERR_DUP_FACE);

    /* ---- 4. 口径校验:换 tag 重启 → 命中被屏蔽 ---- */
    DG_CHECK(vision_service_features_compatible());          /* 首次登记后一致 */
    char tag[64] = { 0 };
    DG_CHECK(db_config_get("face_model_tag", tag, sizeof(tag)) == DG_OK);
    DG_CHECK(strcmp(tag, "mock-v1") == 0);                   /* 首次启动已登记 */
    DG_CHECK(db_config_set("face_model_tag", "other-model-v9") == DG_OK);
    DG_CHECK(vision_backend_start(false) == DG_OK);          /* 模拟换模型后重启 */
    DG_CHECK(!vision_service_features_compatible());

    /* ---- 5. 启动失败要如实上报(装配层据此记 ERROR,不阻塞其余业务) ---- */
    s_fail_start = true;
    DG_CHECK(vision_backend_start(false) != DG_OK);

    /* ---- 清理 ---- */
    vision_service_stop();
    storage_deinit();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    if (system(cmd) != 0) { /* 清理失败不影响结论 */ }
    event_bus_deinit();
    env_set("DG_VISION_BACKEND", NULL);

    DG_TEST_EXIT();
}
