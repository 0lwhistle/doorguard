/*
 * dg_log — door-guard 极简日志(组件移植期的基础设施)
 *
 * 为什么自己写:移植 ESP32 模板组件(event_bus/tasker/holder)时,模板内部
 * 依赖其 logger.h;door-guard 尚无统一日志体系,先用本模块承接,接口形态
 * 与模板 LOGx(TAG, fmt, ...) 一致,后续统一日志模块落地时只换实现不改调用点。
 *
 * 线程安全:单条日志一次 fprintf,stdio 内部有锁;多行前缀不拆分输出。
 * 输出到 stderr:板上由启动脚本重定向,测试时与 stdout 断言输出天然分离。
 */
#ifndef DG_LOG_H
#define DG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DG_LOG_DEBUG = 0,
    DG_LOG_INFO,
    DG_LOG_WARN,
    DG_LOG_ERROR,
} dg_log_level_t;

/* 运行时级别过滤(默认 DG_LOG_INFO);低于级别的日志直接丢弃 */
void dg_log_set_level(dg_log_level_t level);
dg_log_level_t dg_log_get_level(void);

void dg_log_write(dg_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define DG_LOGD(tag, ...) dg_log_write(DG_LOG_DEBUG, tag, __VA_ARGS__)
#define DG_LOGI(tag, ...) dg_log_write(DG_LOG_INFO,  tag, __VA_ARGS__)
#define DG_LOGW(tag, ...) dg_log_write(DG_LOG_WARN,  tag, __VA_ARGS__)
#define DG_LOGE(tag, ...) dg_log_write(DG_LOG_ERROR, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* DG_LOG_H */
