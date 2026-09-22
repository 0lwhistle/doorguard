/*
 * cfg.c — 配置服务(架构 v2 M2①)
 *
 * 数据流:代码内置默认 → default.json(出厂模板) → cur_config.json(现用,稀疏覆盖)
 * 唯一事实:内存里的 cur JSON 树;快照由「默认两层 + cur 树」重算,外部只读。
 * set/reset 改树 → 刷快照 → 防抖原子落盘;DB device_config 仅首启迁移读一次(v2 决议:冻结)。
 * 持久化原子性:临时文件 → fsync → rename;中途掉电最多丢最近一次未防抖改动,不损坏文件。
 */
#include "cfg.h"
#include "cJSON.h"
#include "dg_log.h"
#include "storage.h"
#include "tasker.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *TAG = "[CFG]";

/* 按容量截断拷贝(显式语义,免 snprintf 截断告警) */
static void copy_cstr(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- 业务键元表:set/reset/迁移/json 加载共用,四操作不各写一份 ---- */

typedef enum { CK_INT, CK_DBL, CK_STR } cfg_kind_t;

typedef struct {
    const char *key;        /* 对外键名(cfg_set 与 DB 迁移键一致) */
    const char *json_path;  /* json 文件内路径 */
    cfg_kind_t kind;
    double lo, hi;          /* INT/DBL 合法域;STR = 最大长度-1 */
} cfg_meta_t;

static const cfg_meta_t META[] = {
    { "door_open_ms",         "access.door_open_ms",      CK_INT, 1000, 10000 },
    { "pwd_fail_lock_n",      "access.pwd_fail_lock_n",   CK_INT,    1,    10 },
    { "pwd_fail_lock_s",      "access.pwd_fail_lock_s",   CK_INT,   10,  3600 },
    { "face_dup_threshold",   "face.face_dup_threshold",  CK_DBL, 0.50,  1.00 },
    { "face_match_threshold", "face.match_threshold",     CK_DBL, 0.30,  1.00 },
    { "min_face_px",         "face.min_face_px",          CK_INT,   40,   400 },
    { "blur_min",            "face.blur_min",             CK_DBL,  0.0, 50000 },
    { "det_score_min",       "face.det_score_min",        CK_DBL, 0.30,  1.00 },
    { "liveness_enable",      "face.liveness_enable",     CK_INT,    0,     1 },
    { "standby_timeout_s",    "ui.standby_timeout_s",     CK_INT,   15,    60 },
    { "menu_timeout_s",       "ui.menu_timeout_s",        CK_INT,    5,   120 },
    { "language",             "ui.language",              CK_STR,    0,    15 },
    { "ntp_server",           "network.ntp_server",       CK_STR,    0,    63 },
    { "web_port",             "network.web_port",         CK_INT, 1024, 65535 },
    { "ota_url",              "network.ota_url",          CK_STR,    0,   127 },
};

#define META_N (sizeof(META) / sizeof(META[0]))

static const cfg_meta_t *meta_find(const char *key)
{
    for (size_t i = 0; i < META_N; i++)
        if (!strcmp(META[i].key, key))
            return &META[i];
    return NULL;
}

/* ---- 状态:双缓冲快照(读端拿稳定指针)+ cur/def JSON 树 + 互斥 ---- */

static cJSON *s_def_root;          /* 出厂模板树(常驻,只读) */
static cJSON *s_cur_root;          /* 现用配置树(唯一可变事实) */
static char   s_cur_path[256];
static dg_cfg_t s_buf[2];          /* 双缓冲:重建写另一份,切换后旧读者仍持一致快照 */
static int      s_idx;             /* 读端无锁读(单写者互斥下 int 读原子) */
static bool     s_dirty;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *json_seg(const char *path, char *seg, size_t cap,
                            const char **next)
{
    const char *dot = strchr(path, '.');
    size_t n = dot ? (size_t)(dot - path) : strlen(path);
    if (n == 0 || n >= cap)
        return NULL;
    memcpy(seg, path, n);
    seg[n] = '\0';
    *next = dot ? dot + 1 : NULL;
    return seg;
}

/* 按 "a.b.c" 路径取节点;中途非对象/缺失返回 NULL */
static const cJSON *json_path_get(const cJSON *root, const char *path)
{
    const cJSON *node = root;
    while (node && path && *path) {
        char seg[32];
        const char *next;
        if (!cJSON_IsObject(node) || !json_seg(path, seg, sizeof(seg), &next))
            return NULL;
        node = cJSON_GetObjectItemCaseSensitive(node, seg);
        path = next;
    }
    return node;
}

/* 按 "a.b.c" 路径定位父对象(逐级创建);返回持有最后一段名的父对象 */
static cJSON *json_parent_of(cJSON *root, const char *path, char *last_seg,
                             size_t cap, bool create)
{
    cJSON *node = root;
    while (path) {
        char seg[32];
        const char *next;
        if (!json_seg(path, seg, sizeof(seg), &next))
            return NULL;
        if (!next) {
            snprintf(last_seg, cap, "%s", seg);
            return node;
        }
        cJSON *child = cJSON_GetObjectItemCaseSensitive(node, seg);
        if (!child) {
            if (!create)
                return NULL;
            child = cJSON_CreateObject();
            if (!child || !cJSON_AddItemToObject(node, seg, child))
                return NULL;
        } else if (!cJSON_IsObject(child)) {
            if (!create)
                return NULL;
            cJSON_DeleteItemFromObjectCaseSensitive(node, seg);
            child = cJSON_CreateObject();
            if (!child || !cJSON_AddItemToObject(node, seg, child))
                return NULL;
        }
        node = child;
        path = next;
    }
    return NULL;
}

static bool json_set_num(cJSON *root, const char *path, double v)
{
    char seg[32];
    cJSON *parent = json_parent_of(root, path, seg, sizeof(seg), true);
    if (!parent)
        return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, seg);
    if (item)
        cJSON_SetNumberValue(item, v);
    else {
        item = cJSON_CreateNumber(v);
        if (!item || !cJSON_AddItemToObject(parent, seg, item)) {
            cJSON_Delete(item);
            return false;
        }
    }
    return true;
}

static bool json_set_str(cJSON *root, const char *path, const char *s)
{
    char seg[32];
    cJSON *parent = json_parent_of(root, path, seg, sizeof(seg), true);
    if (!parent)
        return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, seg);
    if (item)
        cJSON_SetValuestring(item, s);
    else {
        item = cJSON_CreateString(s);
        if (!item || !cJSON_AddItemToObject(parent, seg, item)) {
            cJSON_Delete(item);
            return false;
        }
    }
    return true;
}

static bool json_del(cJSON *root, const char *path)
{
    char seg[32];
    cJSON *parent = json_parent_of(root, path, seg, sizeof(seg), false);
    if (!parent)
        return true;                        /* 路径本就不存在 = 已是默认 */
    cJSON_DeleteItemFromObjectCaseSensitive(parent, seg);
    return true;
}

/* ---- 校验辅助(越界回退当前层值并 WARN,绝不崩) ---- */

static int clamp_int(int v, double lo, double hi, int cur, const char *key)
{
    if (v < (int)lo || v > (int)hi) {
        DG_LOGW(TAG, "%s=%d 越界[%d,%d],回退 %d", key, v, (int)lo, (int)hi, cur);
        return cur;
    }
    return v;
}

static double clamp_dbl(double v, double lo, double hi, double cur, const char *key)
{
    if (v < lo || v > hi) {
        DG_LOGW(TAG, "%s=%.3f 越界[%.2f,%.2f],回退 %.2f", key, v, lo, hi, cur);
        return cur;
    }
    return v;
}

/* ---- 内置默认(出厂文件缺失/损坏时的兜底;与 default.json 保持一致) ---- */

static void defaults_apply(dg_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->door_open_ms = 3000;
    c->pwd_fail_lock_n = 5;
    c->pwd_fail_lock_s = 60;
    c->face_dup_threshold = 0.90;
    c->face_match_threshold = 0.42;      /* 与 default.json face.match_threshold 一致 */
    c->face_min_px = 80;                /* 人脸框较小边 ≥80px 才做识别 */
    c->face_blur_min = 50.0;            /* 清晰度下限(板上标定,见日志"清晰度") */
    c->face_det_score_min = 0.70;       /* 检测分数下限 */
    c->liveness_enable = 0;              /* 活体算法 B8 落地前默认关 */
    /* 后端名/口径留空 = 用"第一个注册的后端"+该后端自带口径:同一份模板在
     * PC(sim)与板上(rockiva)都成立,不用两套配置 */
    c->face_backend[0] = '\0';
    snprintf(c->face_model_dir, sizeof(c->face_model_dir), "/usr/lib");
    c->face_model_tag[0] = '\0';
    c->standby_timeout_s = 30;
    c->menu_timeout_s = 15;
    snprintf(c->language, sizeof(c->language), "zh-CN");
    c->web_port = 8080;
    snprintf(c->ntp_server, sizeof(c->ntp_server), "ntp.aliyun.com");
    c->ota_url[0] = '\0';
    c->relay_gpio_line = 0;
    snprintf(c->relay_gpio_chip, sizeof(c->relay_gpio_chip), "/dev/gpiochip0");
}

/* ---- 元表驱动:json → 快照(键缺失走默认,类型错 WARN 回退) ----
 * 按元表定位字段用显式映射而非 offsetof 魔法,编译器可查;12 个键一次列全 */
static void table_field_set(dg_cfg_t *c, const cfg_meta_t *m, const cJSON *item)
{
    if (m->kind == CK_INT) {
        int v = cJSON_IsBool(item) ? (cJSON_IsTrue(item) ? 1 : 0) : item->valueint;
        int cur;
        /* 当前值(下层已应用的)作为越界回退目标 */
        if (!strcmp(m->key, "door_open_ms"))              cur = c->door_open_ms;
        else if (!strcmp(m->key, "pwd_fail_lock_n"))      cur = c->pwd_fail_lock_n;
        else if (!strcmp(m->key, "pwd_fail_lock_s"))      cur = c->pwd_fail_lock_s;
        else if (!strcmp(m->key, "liveness_enable"))      cur = c->liveness_enable;
        else if (!strcmp(m->key, "standby_timeout_s"))    cur = c->standby_timeout_s;
        else if (!strcmp(m->key, "menu_timeout_s"))       cur = c->menu_timeout_s;
        else if (!strcmp(m->key, "web_port"))             cur = c->web_port;
        else return;
        v = clamp_int(v, m->lo, m->hi, cur, m->key);
        if (!strcmp(m->key, "door_open_ms"))              c->door_open_ms = v;
        else if (!strcmp(m->key, "pwd_fail_lock_n"))      c->pwd_fail_lock_n = v;
        else if (!strcmp(m->key, "pwd_fail_lock_s"))      c->pwd_fail_lock_s = v;
        else if (!strcmp(m->key, "liveness_enable"))      c->liveness_enable = v;
        else if (!strcmp(m->key, "standby_timeout_s"))    c->standby_timeout_s = v;
        else if (!strcmp(m->key, "menu_timeout_s"))       c->menu_timeout_s = v;
        else if (!strcmp(m->key, "web_port"))             c->web_port = v;
    } else if (m->kind == CK_DBL) {
        if (!cJSON_IsNumber(item))
            return;
        double v = item->valuedouble;
        if (!strcmp(m->key, "face_dup_threshold"))
            c->face_dup_threshold = clamp_dbl(v, m->lo, m->hi, c->face_dup_threshold, m->key);
        if (!strcmp(m->key, "min_face_px"))
            c->face_min_px = (int32_t)clamp_dbl(v, m->lo, m->hi, c->face_min_px, m->key);
        else if (!strcmp(m->key, "blur_min"))
            c->face_blur_min = clamp_dbl(v, m->lo, m->hi, c->face_blur_min, m->key);
        else if (!strcmp(m->key, "det_score_min"))
            c->face_det_score_min = clamp_dbl(v, m->lo, m->hi, c->face_det_score_min, m->key);
        else if (!strcmp(m->key, "face_match_threshold"))
            c->face_match_threshold = clamp_dbl(v, m->lo, m->hi, c->face_match_threshold, m->key);
    } else { /* CK_STR */
        if (!cJSON_IsString(item) || !item->valuestring)
            return;
        if (strlen(item->valuestring) > (size_t)m->hi) {
            DG_LOGW(TAG, "%s 超长(>%d),回退", m->key, (int)m->hi);
            return;
        }
        if (!strcmp(m->key, "language"))
            copy_cstr(c->language, sizeof(c->language), item->valuestring);
        else if (!strcmp(m->key, "ntp_server"))
            copy_cstr(c->ntp_server, sizeof(c->ntp_server), item->valuestring);
        else if (!strcmp(m->key, "ota_url"))
            copy_cstr(c->ota_url, sizeof(c->ota_url), item->valuestring);
    }
}

static void json_apply_table2(dg_cfg_t *c, const cJSON *root)
{
    for (size_t i = 0; i < META_N; i++) {
        const cJSON *item = json_path_get(root, META[i].json_path);
        if (item)
            table_field_set(c, &META[i], item);
    }
}

/* json-only 键(部署参数/硬件参数,不进 set/迁移) */
static void json_apply_extra(dg_cfg_t *c, const cJSON *root)
{
    const cJSON *item;

    if ((item = json_path_get(root, "face.backend")) &&
        cJSON_IsString(item) && item->valuestring)
        copy_cstr(c->face_backend, sizeof(c->face_backend), item->valuestring);
    if ((item = json_path_get(root, "face.model_dir")) &&
        cJSON_IsString(item) && item->valuestring)
        copy_cstr(c->face_model_dir, sizeof(c->face_model_dir), item->valuestring);
    if ((item = json_path_get(root, "face.model_tag")) &&
        cJSON_IsString(item) && item->valuestring)
        copy_cstr(c->face_model_tag, sizeof(c->face_model_tag), item->valuestring);
    if ((item = json_path_get(root, "access.relay_gpio_line")) &&
        cJSON_IsNumber(item))
        c->relay_gpio_line = item->valueint;
    if ((item = json_path_get(root, "access.relay_gpio_chip")) &&
        cJSON_IsString(item) && item->valuestring)
        copy_cstr(c->relay_gpio_chip, sizeof(c->relay_gpio_chip), item->valuestring);
}

/* 快照重建:默认两层 → cur 层;写入另一份缓冲后切换(读端持旧快照不受影响) */
static void snapshot_rebuild(void)
{
    dg_cfg_t *dst = &s_buf[s_idx ^ 1];
    defaults_apply(dst);
    json_apply_table2(dst, s_def_root);
    json_apply_extra(dst, s_def_root);
    json_apply_table2(dst, s_cur_root);
    json_apply_extra(dst, s_cur_root);
    s_idx ^= 1;
}

/* ---- 原子落盘:临时文件 → fsync → rename ---- */

static int mkdirs_parents(char *path /*会被改写*/)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST)
            return DG_ERR_IO;
        *p = '/';
    }
    return DG_OK;
}

static int atomic_write(const char *path, const char *data)
{
    char tmp[288];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return DG_ERR_IO;
    size_t n = strlen(data);
    if (fwrite(data, 1, n, f) != n || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        remove(tmp);
        return DG_ERR_IO;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return DG_ERR_IO;
    }
    return DG_OK;
}

/* 持锁版落盘(调用方必须已持 s_mu;cfg_load 迁移分支在锁内使用) */
static int flush_locked(void)
{
    if (!s_dirty || !s_cur_path[0])         /* 无路径 = 仅内存模式,无处可写 */
        return DG_OK;
    char *txt = cJSON_Print(s_cur_root);    /* 带缩进:文件给人读/现场排查 */
    int rc = txt ? atomic_write(s_cur_path, txt) : DG_ERR_NO_MEMORY;
    free(txt);
    if (rc == DG_OK)
        s_dirty = false;
    else
        DG_LOGW(TAG, "现用配置落盘失败(%s),保留内存值稍后重试", s_cur_path);
    return rc;
}

int cfg_flush(void)
{
    pthread_mutex_lock(&s_mu);
    int rc = flush_locked();
    pthread_mutex_unlock(&s_mu);
    return rc;
}

/* ---- 防抖落盘(tasker 单次任务;重复 set 先取消重排) ---- */

static enum task_t flush_task_fn(void *ctx)
{
    (void)ctx;
    (void)cfg_flush();
    return TASK_OK;
}

static void flush_schedule(void)
{
    static struct task_node node;
    int rc = tasker_init();                 /* 幂等:测试等未显式初始化场景由此拉起 */
    if (rc == TASK_OK) {
        tasker_cancel_by_name("cfg_flush");
        rc = tasker_task_init_li(&node, 500, 1, "cfg_flush", flush_task_fn, NULL);
        if (rc == TASK_OK)
            rc = tasker_enqueue(&node);
    }
    if (rc != TASK_OK) {
        /* tasker 不可用:同步兜底,绝不丢改动 */
        DG_LOGW(TAG, "防抖任务排不上(%d),同步落盘", rc);
        (void)cfg_flush();
    }
}

/* ---- 首启迁移:DB device_config 已知业务键 → cur(一次性;此后 DB 冻结) ---- */

static int migrate_from_db(void)
{
    int migrated = 0;
    char v[256];

    for (size_t i = 0; i < META_N; i++) {
        const cfg_meta_t *m = &META[i];
        if (db_config_get(m->key, v, sizeof(v)) != DG_OK || !v[0])
            continue;
        if (m->kind == CK_INT) {
            int n;
            if (sscanf(v, "%d", &n) != 1 || n < (int)m->lo || n > (int)m->hi)
                continue;                   /* 非法值不迁移,回落默认 */
            if (!json_set_num(s_cur_root, m->json_path, n))
                return DG_ERR_NO_MEMORY;
        } else if (m->kind == CK_DBL) {
            double d;
            if (sscanf(v, "%lf", &d) != 1 || d < m->lo || d > m->hi)
                continue;
            if (!json_set_num(s_cur_root, m->json_path, d))
                return DG_ERR_NO_MEMORY;
        } else {
            if (strlen(v) > (size_t)m->hi)
                continue;
            if (!json_set_str(s_cur_root, m->json_path, v))
                return DG_ERR_NO_MEMORY;
        }
        migrated++;
    }
    return migrated;
}

/* ---- 解析 json 文件;失败返回 NULL(调用方 WARN) ---- */

static cJSON *parse_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

int cfg_load(const char *default_path, const char *cur_path)
{
    /* NULL 路径 = 不读对应文件(纯内置默认;测试与最小装配用) */
    if (!default_path || !cur_path) {
        pthread_mutex_lock(&s_mu);
        cJSON_Delete(s_def_root);
        s_def_root = cJSON_CreateObject();
        cJSON_Delete(s_cur_root);
        s_cur_root = cJSON_CreateObject();
        s_cur_path[0] = '\0';
        if (!s_def_root || !s_cur_root) {
            pthread_mutex_unlock(&s_mu);
            return DG_ERR_NO_MEMORY;
        }
        snapshot_rebuild();
        pthread_mutex_unlock(&s_mu);
        return DG_OK;
    }

    pthread_mutex_lock(&s_mu);
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", cur_path);

    /* 1. 出厂模板(缺失/坏 → 内置默认,WARN 不拒启) */
    cJSON_Delete(s_def_root);
    s_def_root = parse_file(default_path);
    if (!s_def_root) {
        DG_LOGW(TAG, "出厂模板 %s 缺失或损坏,全部使用内置默认", default_path);
        s_def_root = cJSON_CreateObject();
        if (!s_def_root) {
            pthread_mutex_unlock(&s_mu);
            return DG_ERR_NO_MEMORY;
        }
    }

    /* 2. 现用配置:不存在 → 首启迁移(DB 业务键一次性导出,此后只写本文件) */
    cJSON_Delete(s_cur_root);
    s_cur_root = parse_file(cur_path);
    int load_rc = DG_OK;
    if (!s_cur_root) {
        FILE *probe = fopen(cur_path, "rb");
        if (probe) {                        /* 文件在但解析失败:视为空并 WARN */
            fclose(probe);
            DG_LOGW(TAG, "现用配置 %s 解析失败,按空处理(下次改动覆盖)", cur_path);
        } else {
            char dir[256];
            snprintf(dir, sizeof(dir), "%s", cur_path);
            if (mkdirs_parents(dir) != DG_OK)
                DG_LOGW(TAG, "配置目录创建失败:%s", dir);
            s_cur_root = cJSON_CreateObject();
            if (s_cur_root) {
                int n = migrate_from_db();
                if (n < 0) {
                    cJSON_Delete(s_cur_root);
                    s_cur_root = cJSON_CreateObject();
                    load_rc = DG_ERR_NO_MEMORY;
                } else if (n > 0) {
                    DG_LOGI(TAG, "首启迁移:DB device_config → %s(%d 项,此后 DB 冻结)",
                            cur_path, n);
                }
                s_dirty = true;
                if (flush_locked() != DG_OK && load_rc == DG_OK)
                    load_rc = DG_ERR_IO;    /* 内存值可用,仅落盘失败 */
            }
        }
        if (!s_cur_root) {
            s_cur_root = cJSON_CreateObject();
            if (!s_cur_root) {
                pthread_mutex_unlock(&s_mu);
                return DG_ERR_NO_MEMORY;
            }
        }
    }

    /* 3. 快照:内置默认 → 模板 → 现用 */
    snapshot_rebuild();
    s_dirty = false;                        /* 迁移分支内已自行落盘 */
    pthread_mutex_unlock(&s_mu);

    const dg_cfg_t *c = cfg_get();
    DG_LOGI(TAG, "配置就绪: door=%dms standby=%ds face_dup=%.2f face_match=%.2f "
                 "liveness=%d web=%d lang=%s",
            c->door_open_ms, c->standby_timeout_s, c->face_dup_threshold,
            c->face_match_threshold, c->liveness_enable,
            c->web_port, c->language);
    return load_rc;
}

const dg_cfg_t *cfg_get(void)
{
    return &s_buf[s_idx];
}

/* ---- set / reset:改 cur 树 → 刷快照 → 防抖落盘 ---- */

static int language_valid(const char *v)
{
    return !strcmp(v, "zh-CN") || !strcmp(v, "en-US");
}

int cfg_set_int(const char *key, int value)
{
    const cfg_meta_t *m = meta_find(key);
    if (!m || m->kind != CK_INT)
        return DG_ERR_PARAM;
    if (value < (int)m->lo || value > (int)m->hi) {
        DG_LOGW(TAG, "cfg_set %s=%d 越界[%d,%d],拒绝", key, value,
                (int)m->lo, (int)m->hi);
        return DG_ERR_PARAM;
    }

    pthread_mutex_lock(&s_mu);
    bool ok = json_set_num(s_cur_root, m->json_path, value);
    if (ok) {
        s_dirty = true;
        snapshot_rebuild();
    }
    pthread_mutex_unlock(&s_mu);
    if (!ok)
        return DG_ERR_NO_MEMORY;
    flush_schedule();
    return DG_OK;
}

int cfg_set_str(const char *key, const char *value)
{
    const cfg_meta_t *m = meta_find(key);
    if (!m || m->kind != CK_STR || !value)
        return DG_ERR_PARAM;
    if (!language_valid(value) && !strcmp(key, "language"))
        return DG_ERR_PARAM;                /* 语言是枚举,其余字符串只查长度 */
    if (strlen(value) > (size_t)m->hi)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mu);
    bool ok = json_set_str(s_cur_root, m->json_path, value);
    if (ok) {
        s_dirty = true;
        snapshot_rebuild();
    }
    pthread_mutex_unlock(&s_mu);
    if (!ok)
        return DG_ERR_NO_MEMORY;
    flush_schedule();
    return DG_OK;
}

int cfg_reset_key(const char *key)
{
    const cfg_meta_t *m = meta_find(key);
    if (!m)
        return DG_ERR_PARAM;

    pthread_mutex_lock(&s_mu);
    (void)json_del(s_cur_root, m->json_path);
    s_dirty = true;
    snapshot_rebuild();
    pthread_mutex_unlock(&s_mu);
    flush_schedule();
    return DG_OK;
}

int cfg_reset_all(void)
{
    pthread_mutex_lock(&s_mu);
    cJSON_Delete(s_cur_root);
    s_cur_root = cJSON_CreateObject();
    if (!s_cur_root) {
        pthread_mutex_unlock(&s_mu);
        return DG_ERR_NO_MEMORY;
    }
    s_dirty = true;
    snapshot_rebuild();
    pthread_mutex_unlock(&s_mu);
    return cfg_flush();                     /* 恢复出厂是低频重操作,同步落盘 */
}
