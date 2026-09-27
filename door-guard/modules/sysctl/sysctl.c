/*
 * sysctl.c — 系统控制原语实现
 *
 * 重启路径选择(2026-09-27):优先 system("reboot")——与 SSH/串口人工
 * 验证过的 busybox reboot 同一条路(经 init 走关停流程,DB/文件系统干净);
 * 兜底直接 reboot(RB_AUTOBOOT) 系统调用(init 路径不可用时至少能重启)。
 * 成功时进程在关停中被杀,调用方通常看不到返回。
 */
#include "sysctl.h"
#include "dg_log.h"

#include <stdlib.h>
#include <sys/reboot.h>
#include <unistd.h>

int sysctl_reboot(void)
{
#if defined(DG_SYSCTL_FAKE)
    /* 宿主构建(sim/ctest,CMake 侧按 DG_SIM/DG_BUILD_TESTS 注入
     * DG_SYSCTL_FAKE):模拟重启——WSL 里 root 跑 sim/ctest 曾有先例,
     * 真执行会把宿主机带走;测试只断言"请求被受理并执行到这一步" */
    DG_LOGI("[SYSCTL]", "宿主构建:模拟重启(不真执行)");
    return DG_OK;
#else
    DG_LOGI("[SYSCTL]", "设备重启:sync + reboot(euid=%u)", (unsigned)geteuid());
    sync();
    const int rc = system("reboot");        /* busybox 路径,经 init 关停 */
    if (rc == 0) {
        /* 正常情况下走不到这(system 等待期间进程已被杀);能返回说明
         * 命令已发出但系统仍在,不视为失败 */
        return DG_OK;
    }
    DG_LOGW("[SYSCTL]", "system(\"reboot\")=%d,兜底 reboot() 系统调用", rc);
    return reboot(RB_AUTOBOOT) == 0 ? DG_OK : DG_ERR_INTERNAL;
#endif
}
