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
  不可互换——**看门狗心跳/健康/超时窗口一律 MONOTONIC**(2026-10-03 定案,
  netcore/main 等线程心跳全部 now_mono_ms;板上 RTC 差 51 年且 SNTP 开机会
  大步进,REALTIME 计超时会把超龄判定/自醒间隔整体打穿),墙钟 now_ms 只用于
  日志时间戳/绝对时刻展示。收口后"抄错时钟"从隐患变成显式选择。
- **零依赖**:仅 libc,任何层可 include;不接 dg_log(clock_gettime 无
  可失败路径,保持叶子组件)。
- 迁移纪律:心跳/超时/防抖类一律换 now_mono_ms(2026-10-03 全网收口);
  落库时间戳/UI 时钟显示保持墙钟。历史上"netcore 心跳必须 REALTIME"的旧
  论述与代码相反,2026-10-25 按定案回写——照旧文档改回 REALTIME 会重造
  板上连环误重启事故。

## 测试

`tests/test_timeutil.c`:单调性(两次取值不减)、REALTIME 与 now_s 一致
(±2s)、MONOTONIC 与 REALTIME 数值域不同不混用。
