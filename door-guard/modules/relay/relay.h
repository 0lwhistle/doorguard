/*
 * relay.h — 开门继电器(modules 层,包装 drv/gpio)
 *
 * 职责 = 继电器开关/开门脉冲(architecture §1:services 不直落 drv,
 * 继电器包成 module)。上层(access_service 开门、main 安全停机)只认
 * 本接口,不摸 gpio_hal。引脚号由装配层(main.c)从配置传入,模块自身
 * 不依赖 cfg(modules 层禁 include services)。
 *
 * 降级语义(宿主无 GPIO / 引脚未接):init 拿不到引脚时进入降级态——
 * 脉冲/复位返回 DG_OK 并仅留日志,开门语义由 EV_AUTH_DOOR_OPEN 事件承载,
 * 不阻塞业务;参数非法仍显式报错(边界纪律)。
 */
#ifndef DG_RELAY_H
#define DG_RELAY_H

#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 装配:记录引脚号并导出(失败进入降级态,恒返回 DG_OK;装配层无需分支) */
int relay_module_init(int line_no);

/** 回收引脚(unexport;宿主/降级态静默成功) */
void relay_module_deinit(void);

/** 开门脉冲:拉高 ms 毫秒后拉低(阻塞;0 < ms ≤ 10000,越界 DG_ERR_PARAM)。
 *  降级态仅日志、返回 DG_OK;已就绪则透传 gpio 结果 */
int relay_door_pulse(uint32_t ms);

/** 电平复位到断开态并回收引脚(安全停机专用,不走开门脉冲) */
int relay_reset(void);

/** 电平读取(对拍/自检;降级态 DG_ERR_NOT_INIT) */
int relay_level(int *level);

#ifdef __cplusplus
}
#endif

#endif /* DG_RELAY_H */
