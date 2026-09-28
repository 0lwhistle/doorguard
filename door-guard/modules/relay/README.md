# relay — 开门继电器模块(drv/gpio 的模块层包装)

2026-09-28 随架构收敛 A4 落位(PENDING_DECISIONS A4 拍板:方案 A)。

## 定位

只回答"继电器开/关/开门脉冲"这一件事。此前 `access_service` 直 include
`drv/gpio/gpio_hal.h` 驱开门脉冲、`main.c` 安全停机直摸 gpio——services 落
drv 违反五层栈(services 只到 modules),现统一收进本模块:

- `access_service`(FSM_ACT_OPEN_DOOR)→ `relay_door_pulse()`
- `main.c` safe_shutdown(必需服务不可恢复)→ `relay_reset()`

引脚号由装配层传入(`main.c`:`cfg_get()->relay_gpio_line`,出厂占位 0,
待继电器硬件确认后改配置即可,模块零改动)。模块自身不依赖 cfg——modules
层禁 include services。

## 接口

```c
#include "modules/relay/relay.h"

relay_module_init(cfg_get()->relay_gpio_line);  /* 装配期一次 */
relay_door_pulse(1000);                         /* 开门 1s(阻塞,独立线程调) */
relay_reset();                                  /* 安全停机:复位+回收引脚 */
```

## 设计要点

- **降级态**:宿主(无 GPIO)或引脚未接时 `gpio_hal_init` 失败 → 模块恒
  READY、脉冲/复位退化为纯日志并返回 DG_OK——开门语义由
  `EV_AUTH_DOOR_OPEN` 事件承载,业务零分支(原 access_service 内联的
  gpio_ready 静态逻辑原样搬入)。参数非法仍显式报错。
- **阻塞契约**:`relay_door_pulse` 内部 usleep 开门时长,调用方必须已在
  独立线程(access 的 door_pulse_thread,见其注释——总线线程直接调会把
  唯一分发线程睡死)。
- **复位即回收**:`relay_reset` 拉低后 unexport 引脚,与原 safe_shutdown
  语义一致。
- 电平型继电器假设;脉冲型待硬件确认后在本模块内调整(gpio_hal 语义不变)。

## 测试

`tests/test_relay.c`(宿主):装配(-1 未配置引脚 → 降级)→ 脉冲不崩 →
参数边界 → 复位。板上验收:boot 日志 `继电器就绪 gpio0` + 真实开门脉冲
(见 DEVLOG 2026-09-28)。
