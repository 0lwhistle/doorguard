/*
 * sysctl.h — 系统控制原语(重启等;五层栈的 modules 层,被 sysctl 服务调用)
 *
 * 为什么单独一层:UI(web)→ 事件总线 → sysctl 服务 → 本模块。重启是
 * "写与命令",按架构规则走事件总线,不在 UI/web 里直接执行。
 *
 * 宿主构建(CMake 按 DG_SIM/DG_BUILD_TESTS 注入 DG_SYSCTL_FAKE)下
 * reboot 是**模拟**的:只打日志并返回 DG_OK——否则 WSL 里跑 sim/ctest
 * (尤其 root 身份)会把宿主机真重启(2026-09-27;此前有过 root 跑 sim
 * 的先例)。
 */
#ifndef DG_SYSCTL_H
#define DG_SYSCTL_H

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 同步执行设备重启:sync → system("reboot")(与串口/SSH 验证过的
 *  busybox 路径同源)→ 失败兜底 reboot(RB_AUTOBOOT) 系统调用。
 *  板上成功路径不返回(系统关停中);宿主构建为模拟,恒返回 DG_OK。
 *  @return DG_OK = 已发起;否则 dg_err_t(如权限不足) */
int sysctl_reboot(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_SYSCTL_H */
