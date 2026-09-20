# gpio_hal — 门控 GPIO(sysfs)

## 实现

B4 rootfs **无 libgpiod/gpiod CLI**(实测),采用内核 sysfs 接口:

```
/sys/class/gpio/export        ← 写行号导出
/sys/class/gpio/gpioN/direction ← "out"
/sys/class/gpio/gpioN/value   ← 1/0 读写(开门脉冲/对拍)
/sys/class/gpio/unexport      ← 退出时释放
```

rootfs 升级带 libgpiod 后迁移(gpio_hal.h 接口不变,只换 .c)。

## 引脚配置(待硬件确认!)

- 引脚号:`configs/device.json` → `access.relay_gpio_line`(出厂占位 0)
- **继电器实际接线引脚未确认**——引脚号确认前不要在未知引脚上做物理对拍
  (2026-09-18 事故:未知引脚写 direction 导致板上内核挂起,需物理断电,
  见 DEVLOG)
- 继电器类型(电平保持/脉冲触发)待确认,当前按电平型实现

## 接口

```c
gpio_hal_init(cfg_get()->relay_gpio_line);  /* 导出+设输出 */
gpio_hal_door_pulse(3000);                  /* 拉高 3s 后拉低(阻塞) */
gpio_hal_get_level(&lv);                    /* 对拍/自检 */
gpio_hal_deinit();                          /* unexport */
```

错误:EBUSY→`DG_ERR_BUSY`(引脚被内核占用),其余 IO 失败→`DG_ERR_IO`。

## 对拍方法(引脚确认后)

```bash
# 应用侧:触发开门(access_service OPEN_DOOR → gpio_hal_door_pulse)
# 对拍侧(另一终端):
cat /sys/class/gpio/gpio<N>/value   # 开门期间应为 1,结束后 0
```
