/*
 * registry.h — services 注册表(架构 v2 M2③;与 components/holder 同构)
 *
 * 出处:按 holder(components/holder)的 API 形态同构实现,职责分层不同——
 * holder 管 modules(设备/中间层),registry 管 services(顶层业务)。
 * 相对 holder 的差异:多"心跳钩子"(常驻线程服务周期刷新,供看门狗判活)、
 * restart/mark_disabled(看门狗重启一次→禁用的降级策略原语)、依赖解析器可注入
 * (服务依赖的 modules 在 holder 表里,由装配层提供跨表解析)。
 *
 * 与 holder 相同的约定:init_fn 为简短启动(失败返回非 0),不得回调本表查询;
 * 依赖分批初始化,缺失依赖/循环依赖(一批无进展)→ 相关服务置 ERROR。
 */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REG_STATE_UNKNOWN = 0,
    REG_STATE_REGISTERED,       /**< 已注册未初始化 */
    REG_STATE_INITIALIZING,     /**< 正在启动 */
    REG_STATE_READY,            /**< 运行中 */
    REG_STATE_ERROR,            /**< 启动失败/运行异常 */
    REG_STATE_DISABLED,         /**< 看门狗判定不可恢复,已停用 */
} registry_state_t;

typedef enum {
    REG_OK = 0,
    REG_ERR_INVALID_PARAM,
    REG_ERR_NOT_INITIALIZED,
    REG_ERR_ALREADY_REGISTERED,
    REG_ERR_NOT_FOUND,
    REG_ERR_NO_MEMORY,
    REG_ERR_MUTEX,
    REG_ERR_DEPENDENCY,
    REG_ERR_STATE,
} registry_err_t;

/** 服务启动函数:0 成功,非 0 失败(简短,不得回调本表) */
typedef int (*registry_start_fn)(void);
/** 心跳钩子:返回最近一次活动时间(unix ms);NULL = 无常驻线程,仅状态监控;
 * 返回 <=0 视为暂无数据(不判失联) */
typedef int64_t (*registry_heartbeat_fn)(void);

registry_err_t registry_init(void);
void registry_destroy(void);

/** 注册服务(名字唯一;deps 为依赖服务/模块名数组,可为 NULL) */
registry_err_t registry_register(const char *name, registry_start_fn start_fn,
                                 bool required,
                                 const char *const *deps, int dep_count,
                                 registry_heartbeat_fn hb_fn, void *user_data);

/** 依赖解析器(注入):返回 1 = 名为 name 的依赖已就绪;NULL = 仅表内解析。
 * 装配层用它桥接 holder(modules 层)的就绪状态 */
typedef int (*registry_dep_resolver_fn)(const char *name);
void registry_set_dep_resolver(registry_dep_resolver_fn fn);

/** 分批初始化全部已注册服务;required 失败时按 stop_on_required_error 决定
 * 是否继续(返回 REG_ERR_DEPENDENCY) */
registry_err_t registry_init_all(bool stop_on_required_error);

/** 看门狗原语:重跑 start_fn(READY/ERROR → INITIALIZING → READY/ERROR) */
registry_err_t registry_restart(const char *name);
/** 置 DISABLED(看门狗判定不可恢复;幂等) */
registry_err_t registry_mark_disabled(const char *name);

registry_state_t registry_state(const char *name);
bool registry_is_ready(const char *name);
/** 必需性(看门狗停机策略需要;注册时登记) */
bool registry_is_required(const char *name);
/** 已重启次数(registry_restart 累计;看门狗"最多重启一次"策略用) */
uint32_t registry_restart_count(const char *name);
uint32_t registry_count(void);
/** 按索引取名字(配合 registry_count 遍历);越界返回 NULL */
const char *registry_name_at(uint32_t idx);
/** 最近心跳(unix ms);无心跳钩子返回 -1 */
int64_t registry_last_heartbeat_ms(const char *name);

/* 巡检结论(最严重者胜出;处置策略——重启一次/禁用/停机——归调用方):
 *   REG_WATCH_OK     全部健康
 *   REG_WATCH_STALE  有 READY 服务心跳超龄(仅对注册了心跳钩子的服务判定)
 *   REG_WATCH_ERROR  有服务处于 ERROR 态 */
typedef enum {
    REG_WATCH_OK = 0,
    REG_WATCH_STALE,
    REG_WATCH_ERROR,
} registry_watch_t;

registry_watch_t registry_watchdog_poll(int64_t now_ms, uint32_t stale_after_ms);

#ifdef __cplusplus
}
#endif

#endif /* REGISTRY_H */
