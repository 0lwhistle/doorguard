/*
 * test_sysctl.c — 系统控制服务(设备重启请求链路;宿主 DG_SYSCTL_FAKE 模拟)
 *
 * 宿主构建下 sysctl_reboot() 是模拟(不真重启,防 WSL root 被带走);
 * 本测试钉死:EV_SYS_REBOOT 请求 → 服务受理(去重)→ 延迟执行 →
 * last_err 落 DG_OK;未装配时请求被静默丢弃不崩。
 */
#include "dg_test.h"
#include "event_bus.h"
#include "events.h"
#include "sysctl_service.h"

#include <stdio.h>
#include <unistd.h>

/* 轮询等待 last_err 落值(线程异步执行) */
static int wait_done(int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        if (sysctl_service_last_err() != SYSCTL_ERR_NONE)
            return 1;
        usleep(5000);
    }
    return 0;
}

int main(void)
{
    DG_CHECK(event_bus_init() == EVENT_BUS_OK);
    DG_CHECK(sysctl_service_start() == DG_OK);
    DG_CHECK(sysctl_service_last_err() == SYSCTL_ERR_NONE);

    /* ---- ① 请求 → 延迟执行(30ms)→ 模拟结果 DG_OK ---- */
    const ev_sys_reboot_t ev = { .delay_ms = 30 };
    EVENT_BUS_PUBLISH(EV_SYS_REBOOT, &ev);
    DG_CHECK(wait_done(2000));
    DG_CHECK(sysctl_service_last_err() == DG_OK);

    /* ---- ② 可重复执行:等 ① 线程收尾(pending 清零),再走一轮 ---- */
    usleep(100000);
    EVENT_BUS_PUBLISH(EV_SYS_REBOOT, &ev);
    DG_CHECK(wait_done(2000));
    DG_CHECK(sysctl_service_last_err() == DG_OK);

    /* ---- ③ 服务停掉后请求被丢弃(无订阅者,事件总线自然丢)不崩 ---- */
    sysctl_service_stop();
    EVENT_BUS_PUBLISH(EV_SYS_REBOOT, &ev);
    usleep(50000);

    DG_TEST_EXIT();
}
