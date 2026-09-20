/*
 * cfg.c — 配置加载与持久化(优先级:默认 → device.json → DB device_config)
 */
#include "cfg.h"
#include "cJSON.h"
#include "dg_log.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "[CFG]";

/* 按容量截断拷贝(显式语义,免 snprintf 截断告警) */
static void copy_cstr(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static dg_cfg_t s_cfg;

/* ---- 默认值(唯一的出厂语义来源;json/DB 只是覆盖) ---- */

static void defaults_apply(dg_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->door_open_ms = 3000;
    c->pwd_fail_lock_n = 5;
    c->pwd_fail_lock_s = 60;
    c->face_dup_threshold = 0.90;
    c->face_match_threshold = 0.42;      /* 与 configs/device.json face.match_threshold 一致 */
    c->liveness_enable = 0;              /* 活体算法 B8 落地前默认关 */
    /* 后端名/口径留空 = 用"第一个注册的后端"+该后端自带口径:这样同一份
     * device.json 在 PC(sim)与板上(rockiva)都成立,不用两套配置 */
    c->face_backend[0] = '\0';
    snprintf(c->face_model_dir, sizeof(c->face_model_dir), "/usr/lib");
    c->face_model_tag[0] = '\0';
    c->standby_timeout_s = 30;
    snprintf(c->language, sizeof(c->language), "zh-CN");
    c->web_port = 8080;
    c->ota_port = 9000;
    snprintf(c->ntp_server, sizeof(c->ntp_server), "ntp.aliyun.com");
    c->ota_url[0] = '\0';
}

/* ---- 校验与写入辅助(越界回退默认并 WARN,绝不崩) ---- */

static int clamp_int(int v, int lo, int hi, int dflt, const char *key)
{
    if (v < lo || v > hi) {
        DG_LOGW(TAG, "%s=%d 越界[%d,%d],回退默认 %d", key, v, lo, hi, dflt);
        return dflt;
    }
    return v;
}

static double clamp_dbl(double v, double lo, double hi, double dflt, const char *key)
{
    if (v < lo || v > hi) {
        DG_LOGW(TAG, "%s=%.3f 越界[%.2f,%.2f],回退默认 %.2f", key, v, lo, hi, dflt);
        return dflt;
    }
    return v;
}

/* 从 cJSON 对象按 "a.b.c" 路径取节点;中途非对象/缺失返回 NULL */
static const cJSON *json_path(const cJSON *root, const char *path)
{
    const cJSON *node = root;
    const char *p = path;
    char seg[32];

    while (p && *p) {
        if (!cJSON_IsObject(node))
            return NULL;
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n == 0 || n >= sizeof(seg))
            return NULL;
        memcpy(seg, p, n);
        seg[n] = '\0';
        node = cJSON_GetObjectItemCaseSensitive(node, seg);
        if (!node)
            return NULL;
        p = dot ? dot + 1 : NULL;
    }
    return node;
}

/* ---- json → 快照(键缺失走默认,类型错 WARN 回退) ---- */

static void json_apply(dg_cfg_t *c, const cJSON *root)
{
    const cJSON *item;

    if ((item = json_path(root, "access.door_open_ms"))) {
        if (cJSON_IsNumber(item))
            c->door_open_ms = clamp_int(item->valueint, 1000, 10000, 3000,
                                        "access.door_open_ms");
        else
            DG_LOGW(TAG, "access.door_open_ms 类型错,回退默认");
    }
    if ((item = json_path(root, "access.pwd_fail_lock_n"))) {
        if (cJSON_IsNumber(item))
            c->pwd_fail_lock_n = clamp_int(item->valueint, 1, 10, 5,
                                           "access.pwd_fail_lock_n");
    }
    if ((item = json_path(root, "access.pwd_fail_lock_s"))) {
        if (cJSON_IsNumber(item))
            c->pwd_fail_lock_s = clamp_int(item->valueint, 10, 3600, 60,
                                           "access.pwd_fail_lock_s");
    }
    if ((item = json_path(root, "face.face_dup_threshold"))) {
        if (cJSON_IsNumber(item))
            c->face_dup_threshold = clamp_dbl(item->valuedouble, 0.50, 1.00, 0.90,
                                              "face.face_dup_threshold");
    }
    if ((item = json_path(root, "face.match_threshold"))) {
        if (cJSON_IsNumber(item))
            c->face_match_threshold = clamp_dbl(item->valuedouble, 0.30, 1.00, 0.42,
                                                "face.match_threshold");
    }
    if ((item = json_path(root, "face.liveness_enable"))) {
        if (cJSON_IsBool(item))
            c->liveness_enable = cJSON_IsTrue(item) ? 1 : 0;
        else if (cJSON_IsNumber(item))
            c->liveness_enable = item->valueint ? 1 : 0;
        else
            DG_LOGW(TAG, "face.liveness_enable 类型错,回退默认 0");
    }
    /* 后端/模型参数(json-only:换模型/换框架属部署动作,不进 DB 免被误改) */
    if ((item = json_path(root, "face.backend"))) {
        if (cJSON_IsString(item) && item->valuestring)
            copy_cstr(c->face_backend, sizeof(c->face_backend), item->valuestring);
    }
    if ((item = json_path(root, "face.model_dir"))) {
        if (cJSON_IsString(item) && item->valuestring)
            copy_cstr(c->face_model_dir, sizeof(c->face_model_dir),
                      item->valuestring);
    }
    if ((item = json_path(root, "face.model_tag"))) {
        if (cJSON_IsString(item) && item->valuestring)
            copy_cstr(c->face_model_tag, sizeof(c->face_model_tag),
                      item->valuestring);
    }
    if ((item = json_path(root, "ui.standby_timeout_s"))) {
        if (cJSON_IsNumber(item))
            c->standby_timeout_s = clamp_int(item->valueint, 15, 60, 30,
                                             "ui.standby_timeout_s");
    }
    if ((item = json_path(root, "ui.language"))) {
        if (cJSON_IsString(item) && item->valuestring)
            copy_cstr(c->language, sizeof(c->language), item->valuestring);
        else
            DG_LOGW(TAG, "ui.language 类型错,回退默认");
    }
    if ((item = json_path(root, "network.web_port"))) {
        if (cJSON_IsNumber(item))
            c->web_port = clamp_int(item->valueint, 1024, 65535, 8080,
                                    "network.web_port");
    }
    if ((item = json_path(root, "network.ota_port"))) {
        if (cJSON_IsNumber(item))
            c->ota_port = clamp_int(item->valueint, 1024, 65535, 9000,
                                    "network.ota_port");
    }
    if ((item = json_path(root, "network.ntp_server"))) {
        if (cJSON_IsString(item) && item->valuestring)
            copy_cstr(c->ntp_server, sizeof(c->ntp_server), item->valuestring);
    }
    if ((item = json_path(root, "network.ota_url"))) {
        if (cJSON_IsString(item) && item->valuestring)
            copy_cstr(c->ota_url, sizeof(c->ota_url), item->valuestring);
    }
}

/* ---- DB device_config → 快照(用户设置覆盖出厂值;值非法回退当前值) ---- */

static void db_apply_int(const char *key, int *dst,
                         int lo, int hi, const char *name)
{
    char v[32];
    if (db_config_get(key, v, sizeof(v)) != DG_OK)
        return;
    int parsed;
    if (sscanf(v, "%d", &parsed) != 1) {
        DG_LOGW(TAG, "DB %s='%s' 非整数,忽略", key, v);
        return;
    }
    *dst = clamp_int(parsed, lo, hi, *dst, name);
}

int cfg_load(const char *json_path_str)
{
    defaults_apply(&s_cfg);

    if (json_path_str && *json_path_str) {
        FILE *f = fopen(json_path_str, "rb");
        if (!f) {
            DG_LOGW(TAG, "配置文件 %s 不存在,全部使用默认值", json_path_str);
        } else {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';

            cJSON *root = cJSON_Parse(buf);
            if (!root) {
                /* 坏 json 显式可观测但不拒绝启动:出厂文件损坏时设备仍可用 */
                DG_LOGW(TAG, "配置文件解析失败,使用默认值");
            } else {
                json_apply(&s_cfg, root);
                cJSON_Delete(root);
            }
        }
    }

    /* DB 覆盖(用户设置) */
    db_apply_int("door_open_ms", &s_cfg.door_open_ms, 1000, 10000,
                 "door_open_ms");
    db_apply_int("pwd_fail_lock_n", &s_cfg.pwd_fail_lock_n, 1, 10,
                 "pwd_fail_lock_n");
    db_apply_int("pwd_fail_lock_s", &s_cfg.pwd_fail_lock_s, 10, 3600,
                 "pwd_fail_lock_s");
    db_apply_int("standby_timeout_s", &s_cfg.standby_timeout_s, 15, 60,
                 "standby_timeout_s");
    db_apply_int("web_port", &s_cfg.web_port, 1024, 65535, "web_port");
    db_apply_int("ota_port", &s_cfg.ota_port, 1024, 65535, "ota_port");

    char v[128];
    if (db_config_get("face_dup_threshold", v, sizeof(v)) == DG_OK) {
        double d;
        if (sscanf(v, "%lf", &d) == 1)
            s_cfg.face_dup_threshold = clamp_dbl(d, 0.50, 1.00,
                                                 s_cfg.face_dup_threshold,
                                                 "face_dup_threshold");
        else
            DG_LOGW(TAG, "DB face_dup_threshold='%s' 非数字,忽略", v);
    }
    if (db_config_get("face_match_threshold", v, sizeof(v)) == DG_OK) {
        double d;
        if (sscanf(v, "%lf", &d) == 1)
            s_cfg.face_match_threshold = clamp_dbl(d, 0.30, 1.00,
                                                   s_cfg.face_match_threshold,
                                                   "face_match_threshold");
        else
            DG_LOGW(TAG, "DB face_match_threshold='%s' 非数字,忽略", v);
    }
    db_apply_int("liveness_enable", &s_cfg.liveness_enable, 0, 1, "liveness_enable");
    if (db_config_get("language", v, sizeof(v)) == DG_OK && v[0])
        copy_cstr(s_cfg.language, sizeof(s_cfg.language), v);
    if (db_config_get("ntp_server", v, sizeof(v)) == DG_OK && v[0])
        copy_cstr(s_cfg.ntp_server, sizeof(s_cfg.ntp_server), v);
    if (db_config_get("ota_url", v, sizeof(v)) == DG_OK)
        copy_cstr(s_cfg.ota_url, sizeof(s_cfg.ota_url), v);

    DG_LOGI(TAG, "配置就绪: door=%dms standby=%ds face_dup=%.2f face_match=%.2f "
                 "liveness=%d web=%d ota=%d lang=%s",
            s_cfg.door_open_ms, s_cfg.standby_timeout_s, s_cfg.face_dup_threshold,
            s_cfg.face_match_threshold, s_cfg.liveness_enable,
            s_cfg.web_port, s_cfg.ota_port, s_cfg.language);
    return DG_OK;
}

const dg_cfg_t *cfg_get(void)
{
    return &s_cfg;
}

/* ---- cfg_set:校验 → 写 DB → 刷快照(UI 设置页唯一入口) ---- */

static int set_int_locked(const char *key, int value, int *dst,
                          int lo, int hi, const char *name)
{
    if (value < lo || value > hi) {
        DG_LOGW(TAG, "cfg_set %s=%d 越界[%d,%d],拒绝", name, value, lo, hi);
        return DG_ERR_PARAM;
    }
    char v[32];
    snprintf(v, sizeof(v), "%d", value);
    int rc = db_config_set(key, v);
    if (rc == DG_OK)
        *dst = value;
    return rc;
}

int cfg_set_int(const char *db_key, int value)
{
    if (!db_key)
        return DG_ERR_PARAM;

    if (!strcmp(db_key, "door_open_ms"))
        return set_int_locked(db_key, value, &s_cfg.door_open_ms, 1000, 10000,
                              db_key);
    if (!strcmp(db_key, "pwd_fail_lock_n"))
        return set_int_locked(db_key, value, &s_cfg.pwd_fail_lock_n, 1, 10, db_key);
    if (!strcmp(db_key, "pwd_fail_lock_s"))
        return set_int_locked(db_key, value, &s_cfg.pwd_fail_lock_s, 10, 3600,
                              db_key);
    if (!strcmp(db_key, "standby_timeout_s"))
        return set_int_locked(db_key, value, &s_cfg.standby_timeout_s, 15, 60,
                              db_key);
    if (!strcmp(db_key, "web_port"))
        return set_int_locked(db_key, value, &s_cfg.web_port, 1024, 65535, db_key);
    if (!strcmp(db_key, "ota_port"))
        return set_int_locked(db_key, value, &s_cfg.ota_port, 1024, 65535, db_key);

    DG_LOGW(TAG, "cfg_set 未知键 %s", db_key);
    return DG_ERR_PARAM;
}

int cfg_set_str(const char *db_key, const char *value)
{
    if (!db_key || !value)
        return DG_ERR_PARAM;

    if (!strcmp(db_key, "language")) {
        if (strcmp(value, "zh-CN") != 0 && strcmp(value, "en-US") != 0)
            return DG_ERR_PARAM;
        int rc = db_config_set(db_key, value);
        if (rc == DG_OK)
            copy_cstr(s_cfg.language, sizeof(s_cfg.language), value);
        return rc;
    }
    if (!strcmp(db_key, "ntp_server")) {
        if (!*value || strlen(value) >= sizeof(s_cfg.ntp_server))
            return DG_ERR_PARAM;
        int rc = db_config_set(db_key, value);
        if (rc == DG_OK)
            copy_cstr(s_cfg.ntp_server, sizeof(s_cfg.ntp_server), value);
        return rc;
    }
    if (!strcmp(db_key, "ota_url")) {
        if (strlen(value) >= sizeof(s_cfg.ota_url))
            return DG_ERR_PARAM;
        int rc = db_config_set(db_key, value);
        if (rc == DG_OK)
            copy_cstr(s_cfg.ota_url, sizeof(s_cfg.ota_url), value);
        return rc;
    }
    if (!strcmp(db_key, "face_dup_threshold")) {
        double d;
        if (sscanf(value, "%lf", &d) != 1)
            return DG_ERR_PARAM;
        char v[32];
        snprintf(v, sizeof(v), "%.4f", d);
        int rc = db_config_set(db_key, v);
        if (rc == DG_OK)
            s_cfg.face_dup_threshold = clamp_dbl(d, 0.50, 1.00, 0.90, db_key);
        return rc;
    }

    DG_LOGW(TAG, "cfg_set 未知键 %s", db_key);
    return DG_ERR_PARAM;
}
