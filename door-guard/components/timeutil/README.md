# timeutil — 公共时基组件

## 出处

非 ESP32 模板组件,2026-09-28 C2 收口新建(PENDING_DECISIONS C2:全库
7 处 `static now_ms()` 重复定义——app/main.c、modules/net/netcore.c、
services/{liveness,mdns,vision/vision_rknn,vision/vision_rockiva},声明
定义合计 7 处)。组件 port 层各自的时基(`bus_now_ms`/`tasker_now_ms`,
带回绕语义的 port 契约)**不**在本组件收口范围——那是模板自带的 port
边界,保持自足。

## 接口

```c
#include "components/timeutil/timeutil.h"

now_ms();       /* 墙钟毫秒 REALTIME:看门狗心跳/日志时间戳/绝对时刻 */
now_mono_ms();  /* 单调毫秒 MONOTONIC:耗时测量/超时窗口 */
now_s();        /* 墙钟秒:落库时间戳 */
```

## 设计要点

- **两种时钟显式分名**:REALTIME 与 MONOTONIC 在本库各有真实用户且语义
  不可互换——netcore 心跳必须 REALTIME(与看门狗同基,换 MONOTONIC 相差
  纪元基数必误判超龄,板上实测过),耗时测量必须 MONOTONIC(防 NTP 校正
  跳变)。收口后"抄错时钟"从隐患变成显式选择。
- **零依赖**:仅 libc,任何层可 include;不接 dg_log(clock_gettime 无
  可失败路径,保持叶子组件)。
- 迁移纪律:各调用方换用**与其原时钟同语义**的函数,行为零变化
  (2026-09-28 逐处核对:main/netcore→now_ms;其余四处→now_mono_ms)。

## 测试

`tests/test_timeutil.c`:单调性(两次取值不减)、REALTIME 与 now_s 一致
(±2s)、MONOTONIC 与 REALTIME 数值域不同不混用。
