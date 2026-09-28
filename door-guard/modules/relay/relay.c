/*
 * relay.c — 继电器模块实现(包装 drv/gpio/gpio_hal)
 *
 * 降级态:宿主/无继电器硬件时 gpio_hal_init 失败,模块照常装配(holder
 * 状态 READY),脉冲/复位退化为纯日志——上层无需感知宿主与板的差异
 * (原 access_service 内联的 gpio_ready 静态逻辑原样搬入)。
 */
#include "relay.h"
#include "dg_log.h"
#include "gpio_hal.h"

#include <stdbool.h>

static const char *TAG = "[RELAY]";

static bool s_ready;                 /* gpio_hal 已导出引脚 */
static int  s_line = -1;             /* 装配参数:<0 = 未配置(恒降级) */

int relay_module_init(int line_no)
{
    s_line = line_no;
    s_ready = line_no >= 0 && gpio_hal_init(line_no) == DG_OK;
    if (s_ready)
        DG_LOGI(TAG, "继电器就绪 gpio%d", line_no);
    else
        DG_LOGW(TAG, "gpio 不可用(line=%d),继电器降级:开门仅事件可观测",
                line_no);
    return DG_OK;                    /* 降级不是装配失败:模块恒 READY */
}

void relay_module_deinit(void)
{
    gpio_hal_deinit();
    s_ready = false;
}

int relay_door_pulse(uint32_t ms)
{
    if (ms == 0 || ms > 10000)
        return DG_ERR_PARAM;
    if (!s_ready) {
        DG_LOGI(TAG, "开门 %ums(降级:仅事件)", ms);
        return DG_OK;
    }
    return gpio_hal_door_pulse(ms);
}

int relay_reset(void)
{
    if (!s_ready)
        return DG_OK;
    int rc = gpio_hal_set_level(0);
    gpio_hal_deinit();               /* 复位即回收引脚(安全停机语义) */
    s_ready = false;
    return rc;
}

int relay_level(int *level)
{
    if (!s_ready)
        return DG_ERR_NOT_INIT;
    return gpio_hal_get_level(level);
}
