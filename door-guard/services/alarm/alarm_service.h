/*
 * alarm_service.h — 远程报警上报(services/alarm)
 *
 * 定位:业务报警的统一出口。设备侧触发源(防拆/胁迫/长期离线等)后续逐个
 * 接入,本服务只负责「报警如何出去」——MQTT <p>/event/alarm 上报 + 本地
 * 日志留痕。平台侧(Home Assistant 等)订阅该主题联动通知。
 *
 * 线程契约:任意线程可调;mqtt_publish_json 内部邮箱投递,未启用/未连接
 * 返回 DG_ERR_NOT_INIT(本地日志仍写,报警不丢线索)。
 */
#ifndef DG_ALARM_SERVICE_H
#define DG_ALARM_SERVICE_H

#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALARM_TAMPER = 0,     /**< 防拆(外壳开启/振动;触发源待硬件接入) */
    ALARM_FORCED_OPEN,    /**< 强行开门(门磁断开但无开门动作;待门磁接入) */
    ALARM_DURESS,         /**< 胁迫开门(胁迫指纹/密码;待业务接入) */
    ALARM_OFFLINE,        /**< 设备长期离线(平台侧判定,设备自身难自证) */
    ALARM_CUSTOM,         /**< 自定义(detail 承载;预留扩展) */
} alarm_type_t;

/** 报警上报:type → JSON {"type":"tamper","code":N,"detail":"...","ts":...}
 *  发 <p>/event/alarm(retain=false,报警不留守值——在线通知走实时,离线
 *  补看 access/service 日志)。detail 可空;过长截断。返回发送结果
 *  (DG_OK / DG_ERR_NOT_INIT = mqtt 未启用;本地日志恒写) */
int alarm_report(alarm_type_t type, const char *detail);

/* ---- 测试注入口:替换发送 sink(NULL = 恢复默认 mqtt 上报) ---- */
typedef int (*alarm_send_fn)(alarm_type_t type, const char *json);
void alarm_sink_set(alarm_send_fn fn);

int alarm_service_start(void);
void alarm_service_stop(void);

#endif /* DG_ALARM_SERVICE_H */
