/*
 * event_bus_types.h — 事件总线公共类型
 *
 * 移植自 ESP32 模板 ovs/components/core/event_bus/event_bus_types.h
 * (出处/改动点见本目录 README.md)。业务事件(EV_AUTH_RESULT 等)在 Phase 2
 * 的 proto/events.h 定义;本文件只保留总线自身机制所需的类型:
 * 事件编码规则、通用事件结构、错误码、类型安全发布宏。
 */
#ifndef EVENT_BUS_TYPES_H
#define EVENT_BUS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量:容量参数为模板实测稳定值,改前先跑压测(test_event_bus_stress) ---- */

#define EVENT_BUS_VERSION_MAJOR     1
#define EVENT_BUS_VERSION_MINOR     0
#define EVENT_BUS_VERSION_PATCH     0

/** 事件队列深度:发布方非阻塞,满即丢弃并计数;32 在门禁事件频次(≤百/秒)下
 *  足够缓冲分发抖动,压测发布方须对 QUEUE_FULL 重试(见 tests/test_event_bus_stress.c) */
#define EVENT_BUS_QUEUE_SIZE        32

/** 最大订阅者数量:UI 页面 + 服务层订阅者总量上限 */
#define EVENT_BUS_MAX_SUBSCRIBERS   64

/** 最大事件数据大小(字节):门禁事件负载均为小结构体,256 富余 */
#define EVENT_BUS_MAX_EVENT_SIZE    256

/** 事件内存池块数:48 × 最大事件尺寸,池满退化堆分配(模板 5.1 修复项2) */
#define EVENT_POOL_BLOCKS           48

/** 分发任务栈字节大小(Linux pthread 仅作为 attr 提示,内核按需增长) */
#define EVENT_BUS_TASK_STACK_SIZE   (64 * 1024)

/** 无事件时任务轮询间隔(ms):也是 deinit 后任务退出的最大延迟 */
#define EVENT_BUS_POLL_INTERVAL_MS  10

/** 无效的订阅 ID(0 保留) */
#define EVENT_BUS_INVALID_SUB_ID    0

/* ---- 模块 ID(事件类型高 16 位);业务域事件在 proto/events.h 按此分配 ---- */

#define DG_MODULE_ID_SYSTEM     0x0001
#define DG_MODULE_ID_UI         0x0002
#define DG_MODULE_ID_AUTH       0x0003
#define DG_MODULE_ID_ENROLL     0x0004
#define DG_MODULE_ID_STORAGE    0x0005
#define DG_MODULE_ID_NET        0x0006
#define DG_MODULE_ID_CAPTURE    0x0007
#define DG_MODULE_ID_VISION     0x0008
#define DG_MODULE_ID_HAL        0x0009
#define DG_MODULE_ID_TEST       0x000A  /* 仅组件测试/demo 使用,业务禁用 */

/**
 * 事件类型:(module_id << 16) | event_id
 * 模板中 WiFi/LoRa/Audio 等域事件不适用于门禁,已按上表重组;
 * SYSTEM/UI 保留模板同名事件,基础组件测试使用 TEST 域。
 */
typedef enum {
    /* 系统事件 */
    EVENT_SYSTEM_STARTUP        = (DG_MODULE_ID_SYSTEM << 16) | 0x0001,
    EVENT_SYSTEM_SHUTDOWN       = (DG_MODULE_ID_SYSTEM << 16) | 0x0002,
    EVENT_SYSTEM_ERROR          = (DG_MODULE_ID_SYSTEM << 16) | 0x0003,

    /* UI 事件 */
    EVENT_UI_PAGE_CHANGE        = (DG_MODULE_ID_UI << 16) | 0x0001,
    EVENT_UI_REFRESH_REQUEST    = (DG_MODULE_ID_UI << 16) | 0x0002,

    /* 组件测试域(非业务) */
    EVENT_TEST_PUBLISH          = (DG_MODULE_ID_TEST << 16) | 0x0001,  /* 带载荷 */
    EVENT_TEST_NO_DATA          = (DG_MODULE_ID_TEST << 16) | 0x0002,  /* 无载荷 */
    EVENT_TEST_REENTRANT        = (DG_MODULE_ID_TEST << 16) | 0x0003,  /* 锁外回调重入 */
    EVENT_TEST_POOL             = (DG_MODULE_ID_TEST << 16) | 0x0004,  /* 池耗尽路径 */

    EVENT_TYPE_MAX              = 0x7FFFFFFF
} event_type_t;

/* ---- TEST 域载荷(仅组件测试与示例使用) ---- */

typedef struct {
    int16_t rssi;
    uint8_t channel;
    uint8_t flags;
} event_test_payload_t;

/* ---- 事件头与通用事件结构(与模板逐字节兼容,柔性数组承载数据) ---- */

typedef struct {
    event_type_t type;          /**< 事件类型 */
    uint32_t timestamp;         /**< 事件产生时间戳(ms,单调时基) */
    uint16_t data_len;          /**< 事件数据长度(不含此头) */
    uint16_t reserved;          /**< 对齐保留;分配来源标志(0=池,1=堆),外部勿改 */
} event_header_t;

typedef struct {
    event_header_t header;
    uint8_t data[];             /**< 柔性数组,data_len 字节 */
} event_t;

/* ---- 错误码(与模板一致,调用方按值判定) ---- */

typedef enum {
    EVENT_BUS_OK                  =  0,
    EVENT_BUS_ERR_INVALID_PARAM   = -1,
    EVENT_BUS_ERR_NOT_INIT        = -2,
    EVENT_BUS_ERR_ALREADY_INIT    = -3,
    EVENT_BUS_ERR_QUEUE_FULL      = -4,
    EVENT_BUS_ERR_NO_MEMORY       = -5,
    EVENT_BUS_ERR_NOT_FOUND       = -6,
    EVENT_BUS_ERR_MAX_SUBSCRIBERS = -7,
    EVENT_BUS_ERR_TIMEOUT         = -8,
    EVENT_BUS_ERR_INTERNAL        = -9,
} event_bus_err_t;

/* ---- 类型安全发布宏 ---- */

/** 带载荷发布:自动 sizeof,防手写长度错 */
#define EVENT_BUS_PUBLISH(type, data_ptr) \
    event_bus_publish(type, data_ptr, sizeof(*(data_ptr)))

/** 无载荷发布 */
#define EVENT_BUS_PUBLISH_EMPTY(type) \
    event_bus_publish(type, NULL, 0)

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_TYPES_H */
