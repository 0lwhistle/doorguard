# uart_hal — 串口框架(指纹/IC 读卡)

## 协议纪律(最高优先)

**指纹模块与 IC 读卡器的帧协议等硬件手册,不臆造**(任务清单§5)。
本 HAL 只提供传输层:打开/参数/收发/接收线程;帧解析在手册到位后在
`auth/{finger,card}` 实现。**待硬件确认**:指纹模块型号与协议、读卡器
型号与协议、接线 UART 口(DEVLOG 登记)。

## 接口

```c
uart_config_t cfg = {
    .device = "/dev/ttyS3",   /* mock 后端传 "mock" */
    .baud = 115200, .data_bits = 8, .parity = 'N', .stop_bits = 1,
};
uart_hal_open(&cfg, on_frame, user_data);   /* 启动接收线程 */
uart_hal_send(frame, len);                  /* 发送 */
uart_hal_close();
```

- 真实串口:termios 原始模式,读线程 100ms 超时轮询
- **mock 后端**(device=="mock"):send 数据 5ms 后原样回吐接收回调,
  供宿主 ctest 验证框架(协议无关)

## 测试

`tests/test_uart_mock.c`:回环三帧收发一致、重复打开拒绝、关闭后拒绝
发送、参数错误显式返回。
