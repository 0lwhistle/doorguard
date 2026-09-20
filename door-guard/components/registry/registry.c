/*
 * registry.c — services 注册表实现(与 holder 同构;差异见 registry.h 头注)
 */
#include "registry.h"
#include "dg_log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "[REG]";

#define REG_MAX_SERVICES 32
#define REG_NAME_MAX     32

typedef struct {
    char name[REG_NAME_MAX];
    registry_start_fn start_fn;
    registry_heartbeat_fn hb_fn;
    char deps[8][REG_NAME_MAX];          /* 依赖名(服务或跨表模块) */
    int dep_count;
    bool required;
    registry_state_t state;
    uint32_t error_count;
    uint32_t restarts;
    void *user_data;
    const char *last_error;
} reg_entry_t;

static reg_entry_t s_tbl[REG_MAX_SERVICES];
static uint32_t s_cnt;
static bool s_inited;
static registry_dep_resolver_fn s_resolver;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;

static reg_entry_t *find_locked(const char *name)
{
    for (uint32_t i = 0; i < s_cnt; i++)
        if (!strcmp(s_tbl[i].name, name))
            return &s_tbl[i];
    return NULL;
}

registry_err_t registry_init(void)
{
    pthread_mutex_lock(&s_mu);
    s_inited = true;
    pthread_mutex_unlock(&s_mu);
    return REG_OK;
}

void registry_destroy(void)
{
    pthread_mutex_lock(&s_mu);
    memset(s_tbl, 0, sizeof(s_tbl));
    s_cnt = 0;
    s_inited = false;
    s_resolver = NULL;
    pthread_mutex_unlock(&s_mu);
}

registry_err_t registry_register(const char *name, registry_start_fn start_fn,
                                 bool required,
                                 const char *const *deps, int dep_count,
                                 registry_heartbeat_fn hb_fn, void *user_data)
{
    if (!s_inited || !name || !start_fn)
        return !s_inited ? REG_ERR_NOT_INITIALIZED : REG_ERR_INVALID_PARAM;
    if (!name[0] || strlen(name) >= REG_NAME_MAX || dep_count > 8)
        return REG_ERR_INVALID_PARAM;

    pthread_mutex_lock(&s_mu);
    if (s_cnt >= REG_MAX_SERVICES || find_locked(name)) {
        pthread_mutex_unlock(&s_mu);
        return s_cnt >= REG_MAX_SERVICES ? REG_ERR_NO_MEMORY
                                         : REG_ERR_ALREADY_REGISTERED;
    }
    reg_entry_t *e = &s_tbl[s_cnt++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->start_fn = start_fn;
    e->hb_fn = hb_fn;
    e->required = required;
    e->user_data = user_data;
    e->state = REG_STATE_REGISTERED;
    for (int i = 0; i < dep_count; i++)
        snprintf(e->deps[i], sizeof(e->deps[i]), "%s", deps[i]);
    e->dep_count = dep_count;
    pthread_mutex_unlock(&s_mu);
    return REG_OK;
}

void registry_set_dep_resolver(registry_dep_resolver_fn fn)
{
    pthread_mutex_lock(&s_mu);
    s_resolver = fn;
    pthread_mutex_unlock(&s_mu);
}

/* 依赖就绪判定:表内 READY,或注入解析器放行(跨表,如 holder 的 modules)。
 * 调用方须持 s_mu;解析器不得回调本表(装配层保证)。 */
static bool dep_satisfied_locked(const reg_entry_t *e)
{
    for (int i = 0; i < e->dep_count; i++) {
        reg_entry_t *d = find_locked(e->deps[i]);
        if (d) {
            if (d->state != REG_STATE_READY)
                return false;
        } else if (s_resolver) {
            if (!s_resolver(e->deps[i]))
                return false;
        } else {
            return false;                    /* 表内外都无法满足 */
        }
    }
    return true;
}

static registry_err_t start_one_locked(reg_entry_t *e)
{
    e->state = REG_STATE_INITIALIZING;
    int rc = e->start_fn();
    if (rc == 0) {
        e->state = REG_STATE_READY;
        e->last_error = NULL;
        DG_LOGI(TAG, "service %s READY", e->name);
        return REG_OK;
    }
    e->state = REG_STATE_ERROR;
    e->error_count++;
    e->last_error = "start_fn failed";
    DG_LOGW(TAG, "service %s 启动失败(rc=%d)%s", e->name, rc,
            e->required ? "(必需)" : "(选修,降级继续)");
    return REG_ERR_DEPENDENCY;
}

registry_err_t registry_init_all(bool stop_on_required_error)
{
    if (!s_inited)
        return REG_ERR_NOT_INITIALIZED;

    pthread_mutex_lock(&s_mu);
    /* 依赖分批:每批把"依赖已满足"的全部启动;一批无进展 = 缺依赖/循环依赖 */
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (uint32_t i = 0; i < s_cnt; i++) {
            reg_entry_t *e = &s_tbl[i];
            if (e->state != REG_STATE_REGISTERED
                || !dep_satisfied_locked(e))
                continue;
            start_one_locked(e);
            progressed = true;
        }
    }
    registry_err_t rc = REG_OK;
    for (uint32_t i = 0; i < s_cnt; i++) {
        reg_entry_t *e = &s_tbl[i];
        if (e->state == REG_STATE_REGISTERED) {
            e->state = REG_STATE_ERROR;
            e->error_count++;
            e->last_error = "dependency never satisfied";
            DG_LOGW(TAG, "service %s 依赖无法满足(%d 项),置 ERROR", e->name,
                    e->dep_count);
        }
        if (e->state == REG_STATE_ERROR && e->required
            && stop_on_required_error)
            rc = REG_ERR_DEPENDENCY;
    }
    pthread_mutex_unlock(&s_mu);
    return rc;
}

registry_err_t registry_restart(const char *name)
{
    if (!name)
        return REG_ERR_INVALID_PARAM;
    pthread_mutex_lock(&s_mu);
    reg_entry_t *e = find_locked(name);
    if (!e) {
        pthread_mutex_unlock(&s_mu);
        return REG_ERR_NOT_FOUND;
    }
    if (e->state == REG_STATE_DISABLED || e->state == REG_STATE_INITIALIZING) {
        pthread_mutex_unlock(&s_mu);
        return REG_ERR_STATE;
    }
    e->restarts++;
    registry_err_t rc = start_one_locked(e);
    pthread_mutex_unlock(&s_mu);
    return rc;
}

registry_err_t registry_mark_disabled(const char *name)
{
    if (!name)
        return REG_ERR_INVALID_PARAM;
    pthread_mutex_lock(&s_mu);
    reg_entry_t *e = find_locked(name);
    registry_err_t rc = REG_OK;
    if (!e)
        rc = REG_ERR_NOT_FOUND;
    else {
        e->state = REG_STATE_DISABLED;
        DG_LOGW(TAG, "service %s 已禁用(看门狗判定不可恢复)", e->name);
    }
    pthread_mutex_unlock(&s_mu);
    return rc;
}

registry_state_t registry_state(const char *name)
{
    if (!name)
        return REG_STATE_UNKNOWN;
    pthread_mutex_lock(&s_mu);
    reg_entry_t *e = find_locked(name);
    registry_state_t st = e ? e->state : REG_STATE_UNKNOWN;
    pthread_mutex_unlock(&s_mu);
    return st;
}

bool registry_is_ready(const char *name)
{
    return registry_state(name) == REG_STATE_READY;
}

bool registry_is_required(const char *name)
{
    if (!name)
        return false;
    pthread_mutex_lock(&s_mu);
    reg_entry_t *e = find_locked(name);
    bool req = e ? e->required : false;
    pthread_mutex_unlock(&s_mu);
    return req;
}

uint32_t registry_restart_count(const char *name)
{
    if (!name)
        return 0;
    pthread_mutex_lock(&s_mu);
    reg_entry_t *e = find_locked(name);
    uint32_t n = e ? e->restarts : 0;
    pthread_mutex_unlock(&s_mu);
    return n;
}

uint32_t registry_count(void)
{
    pthread_mutex_lock(&s_mu);
    uint32_t n = s_cnt;
    pthread_mutex_unlock(&s_mu);
    return n;
}

const char *registry_name_at(uint32_t idx)
{
    pthread_mutex_lock(&s_mu);
    const char *n = idx < s_cnt ? s_tbl[idx].name : NULL;
    pthread_mutex_unlock(&s_mu);
    return n;
}

int64_t registry_last_heartbeat_ms(const char *name)
{
    if (!name)
        return -1;
    pthread_mutex_lock(&s_mu);
    reg_entry_t *e = find_locked(name);
    int64_t hb = (e && e->hb_fn) ? e->hb_fn() : -1;
    pthread_mutex_unlock(&s_mu);
    return hb;
}

registry_watch_t registry_watchdog_poll(int64_t now_ms, uint32_t stale_after_ms)
{
    registry_watch_t worst = REG_WATCH_OK;
    pthread_mutex_lock(&s_mu);
    for (uint32_t i = 0; i < s_cnt; i++) {
        reg_entry_t *e = &s_tbl[i];
        if (e->state == REG_STATE_ERROR) {
            worst = REG_WATCH_ERROR;         /* 最严重,可提前定论 */
            break;
        }
        if (e->state == REG_STATE_READY && e->hb_fn) {
            int64_t hb = e->hb_fn();
            if (hb > 0 && now_ms - hb > (int64_t)stale_after_ms
                && worst == REG_WATCH_OK)
                worst = REG_WATCH_STALE;
        }
    }
    pthread_mutex_unlock(&s_mu);
    return worst;
}
