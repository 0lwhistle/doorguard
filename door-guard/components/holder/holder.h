/**
 * @file holder.h
 * @brief 全局硬件模块注册表管理器
 * 
 * 维护一个全局注册表，管理所有硬件模块的注册、初始化和状态检查。
 * 防止单个硬件模块故障导致整个系统崩溃。
 * 
 * @version 1.0
 * @date 2026-09-07
 */

/*
 * holder.h — 全局模块注册表公共 API
 *
 * 移植自 ESP32 模板 ovs/components/modules/holder/holder.h(API 逐个一致,
 * 出处与 port 改动点见本目录 README.md)。door-guard 用途:HAL 与服务的
 * 启动顺序与健康状态注册表(对应 main.c 装配"故障即退出")。
 */
#ifndef HOLDER_H
#define HOLDER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 模块状态枚举
 */
typedef enum {
    HOLDER_MODULE_STATE_UNKNOWN = 0,    /**< 未知状态 */
    HOLDER_MODULE_STATE_REGISTERED,     /**< 已注册但未初始化 */
    HOLDER_MODULE_STATE_INITIALIZING,   /**< 正在初始化 */
    HOLDER_MODULE_STATE_READY,          /**< 初始化成功，就绪 */
    HOLDER_MODULE_STATE_ERROR,          /**< 初始化失败 */
    HOLDER_MODULE_STATE_DISABLED,       /**< 已禁用 */
    HOLDER_MODULE_STATE_MAX
} holder_module_state_t;

/**
 * @brief 模块错误码枚举
 */
typedef enum {
    HOLDER_OK = 0,                      /**< 成功 */
    HOLDER_ERR_INVALID_PARAM,           /**< 无效参数 */
    HOLDER_ERR_NOT_INITIALIZED,         /**< holder未初始化 */
    HOLDER_ERR_ALREADY_REGISTERED,      /**< 模块已注册 */
    HOLDER_ERR_NOT_FOUND,               /**< 模块未找到 */
    HOLDER_ERR_NO_MEMORY,               /**< 内存不足 */
    HOLDER_ERR_MUTEX,                   /**< 互斥锁错误 */
    HOLDER_ERR_INIT_FAILED,             /**< 初始化失败 */
    HOLDER_ERR_DEPENDENCY,              /**< 依赖未满足或存在循环依赖 */
    HOLDER_ERR_MAX
} holder_err_t;

/**
 * @brief 模块初始化函数类型
 * @return 0 成功，其他值失败
 */
typedef int (*holder_module_init_fn)(void);

/**
 * @brief 模块信息结构体
 */
typedef struct {
    const char* name;                   /**< 模块名称 */
    holder_module_init_fn init_fn;      /**< 初始化函数 */
    holder_module_state_t state;        /**< 当前状态 */
    uint32_t error_count;               /**< 错误计数 */
    uint32_t init_time_ms;              /**< 初始化耗时(ms) */
    const char* last_error;             /**< 最后错误信息 */
    bool required;                      /**< 是否必需模块 */
    void* user_data;                    /**< 用户数据 */
    const char* const* dependencies;    /**< 依赖模块名数组（可为NULL） */
    int dependency_count;               /**< 依赖数量 */
} holder_module_info_t;

/**
 * @brief 初始化holder模块
 * 
 * 必须在注册任何模块之前调用。
 * 
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_init(void);

/**
 * @brief 注册模块到holder
 * 
 * @param name 模块名称（唯一标识）
 * @param init_fn 模块初始化函数
 * @param required 是否为必需模块
 * @param user_data 用户数据（可选）
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_register_module(const char* name, 
                                   holder_module_init_fn init_fn,
                                   bool required,
                                   void* user_data);

/**
 * @brief 注册模块并声明依赖（Linux 式总线/设备分层）
 *
 * dependencies 中的模块必须先于本模块初始化。holder_init_all() 会自动
 * 按依赖分批初始化；缺失依赖或循环依赖将把模块置为 ERROR 状态。
 *
 * @param name         模块名称（唯一标识）
 * @param init_fn      模块初始化函数
 * @param required     是否为必需模块
 * @param dependencies 依赖模块名数组（可为 NULL）
 * @param dependency_count 依赖数量
 * @param user_data    用户数据（可选）
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_register_module_ex(const char* name,
                                       holder_module_init_fn init_fn,
                                       bool required,
                                       const char* const* dependencies,
                                       int dependency_count,
                                       void* user_data);

/**
 * @brief 注销模块
 * 
 * @param name 模块名称
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_unregister_module(const char* name);

/**
 * @brief 初始化指定模块
 * 
 * @param name 模块名称
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_init_module(const char* name);

/**
 * @brief 初始化所有已注册模块
 * 
 * 按照注册顺序初始化所有模块。
 * 如果必需模块初始化失败，返回错误。
 * 非必需模块初始化失败会记录错误但不阻止系统启动。
 * 
 * @param stop_on_required_error 遇到必需模块错误时是否停止
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_init_all(bool stop_on_required_error);

/**
 * @brief 获取模块状态
 * 
 * @param name 模块名称
 * @param info 输出模块信息
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_get_module_info(const char* name, holder_module_info_t* info);

/**
 * @brief 检查模块是否就绪
 * 
 * @param name 模块名称
 * @return true 就绪，false 未就绪或不存在
 */
bool holder_is_module_ready(const char* name);

/**
 * @brief 获取模块状态
 * 
 * @param name 模块名称
 * @return 模块状态，HOLDER_MODULE_STATE_UNKNOWN表示模块不存在
 */
holder_module_state_t holder_get_module_state(const char* name);

/**
 * @brief 设置模块状态
 * 
 * @param name 模块名称
 * @param state 新状态
 * @return HOLDER_OK 成功，其他值失败
 */
holder_err_t holder_set_module_state(const char* name, holder_module_state_t state);

/**
 * @brief 打印所有模块状态（调试用）
 */
void holder_print_status(void);

/**
 * @brief 获取已注册模块数量
 * 
 * @return 模块数量
 */
uint32_t holder_get_module_count(void);
/**
 * @brief 按索引获取模块名（配合 holder_get_module_count 遍历）
 * @return 模块名；索引越界返回 NULL
 */
const char* holder_get_module_name(int index);

/**
 * @brief 检查holder是否已初始化
 * 
 * @return true 已初始化，false 未初始化
 */
bool holder_is_initialized(void);

/**
 * @brief 销毁holder模块
 * 
 * 释放所有资源，清空注册表。
 */
void holder_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // HOLDER_H
