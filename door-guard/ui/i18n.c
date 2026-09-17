/*
 * i18n.c — 语言表加载与翻译
 */
#include "i18n.h"
#include "cJSON.h"
#include "dg_log.h"
#include "event_bus.h"
#include "err.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "[I18N]";

/* 语言表常驻缓存:标签字符串会被页面长期持有(如静态 label 文本),
 * 切换语言后旧表不能释放,否则消费方持野指针 */
#define I18N_TABLE_MAX 4
static cJSON *s_tables[I18N_TABLE_MAX];
static char s_table_lang[I18N_TABLE_MAX][16];
static int s_table_cnt = 0;
static cJSON *s_table = NULL;          /* 当前语言表(键=原文,值=译文) */
static char s_lang[16] = "zh-CN";
static char s_lang_dir[192];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

int i18n_init(const char *lang_dir)
{
    if (!lang_dir || !*lang_dir)
        return DG_ERR_PARAM;
    pthread_mutex_lock(&s_mtx);
    snprintf(s_lang_dir, sizeof(s_lang_dir), "%s", lang_dir);
    pthread_mutex_unlock(&s_mtx);
    /* 首次加载:直接置语言(不发刷新事件,页面尚未创建) */
    extern int i18n_apply_locked(const char *lang);
    return i18n_apply_locked(s_lang);
}

int i18n_apply_locked(const char *lang)
{
    /* 已加载过:直接切换(缓存常驻,见 s_tables 注释) */
    for (int i = 0; i < s_table_cnt; i++) {
        if (!strcmp(s_table_lang[i], lang)) {
            s_table = s_tables[i];
            snprintf(s_lang, sizeof(s_lang), "%s", lang);
            DG_LOGI(TAG, "语言切换为 %s", lang);
            return DG_OK;
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.json", s_lang_dir, lang);

    FILE *f = fopen(path, "rb");
    if (!f) {
        DG_LOGW(TAG, "语言表 %s 不存在,保持原文", path);
        return DG_ERR_NOT_FOUND;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *table = cJSON_Parse(buf);
    if (!table) {
        DG_LOGW(TAG, "语言表 %s 解析失败,保持原文", path);
        return DG_ERR_PARAM;
    }

    pthread_mutex_lock(&s_mtx);
    if (s_table_cnt < I18N_TABLE_MAX) {
        s_tables[s_table_cnt] = table;
        snprintf(s_table_lang[s_table_cnt], sizeof(s_table_lang[0]), "%s", lang);
        s_table_cnt++;
    } else {
        DG_LOGW(TAG, "语言表缓存满,本表不缓存");
    }
    s_table = table;
    snprintf(s_lang, sizeof(s_lang), "%s", lang);
    pthread_mutex_unlock(&s_mtx);
    DG_LOGI(TAG, "语言切换为 %s", lang);
    return DG_OK;
}

const char *_(const char *key)
{
    if (!key)
        return "";
    pthread_mutex_lock(&s_mtx);
    const char *out = key;
    if (s_table) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(s_table, key);
        if (cJSON_IsString(v) && v->valuestring)
            out = v->valuestring;
    }
    /* 注意:返回 cJSON 内部字符串;表只在切换时整体替换且进程内常驻,生命周期安全 */
    pthread_mutex_unlock(&s_mtx);
    return out;
}

int i18n_set_language(const char *lang)
{
    if (!lang)
        return DG_ERR_PARAM;
    int rc = i18n_apply_locked(lang);
    if (rc != DG_OK)
        return rc;
    /* 立即生效:各页面订阅刷新事件重刷静态文本(spec-ui §2) */
    event_bus_publish(EVENT_UI_REFRESH_REQUEST, NULL, 0);
    return DG_OK;
}

const char *i18n_current_language(void)
{
    return s_lang;
}
