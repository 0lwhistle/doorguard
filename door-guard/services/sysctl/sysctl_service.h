/*
 * sysctl_service.h — 系统控制服务(设备重启的统一入口)
 *
 * UI 与 web 都通过 EV_SYS_REBOOT 请求重启(写与命令走事件总线,架构 §1);
 * 本服务订阅后延迟执行(delay_ms 让 HTTP 回执/UI 弹窗先落地),经
 * modules/sysctl 原语执行。
 */
#ifndef DG_SYSCTL_SERVICE_H
#define DG_SYSCTL_SERVICE_H

#include "err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sysctl_service_start(void);
void sysctl_service_stop(void);

/** 最近一次重启执行的结果(测试/诊断观测)。
 *  SYSCTL_ERR_NONE = 尚未执行过;板上成功路径系统已关停,观察不到返回;
 *  宿主构建恒为 DG_OK(模拟)。 */
int32_t sysctl_service_last_err(void);

#define SYSCTL_ERR_NONE INT32_MIN

#ifdef __cplusplus
}
#endif

#endif /* DG_SYSCTL_SERVICE_H */
