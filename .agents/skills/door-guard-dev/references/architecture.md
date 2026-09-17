# 架构与模块(含 ESP32 模板复用)

> 全景方案见 `PROJECT_PLAN.md` §三;本文是开发视角的落地约定。

## 1. 软件分层(既有共识,新模块必须落位到某一层)

```
UI 层        ui/ 页面与弹窗(LVGL;PC 模拟器 + 板上双构建)
             只发事件/收事件,不做业务决策
──────────────────────────────────────────────
服务层       capture   取流:rkaiq 3A + V4L2/RKADK → NV12 环形缓冲(dma-buf)
             vision    ROCKIVA 检测/特征/1:N 检索、指纹算法适配、1:1 比对
             liveness  动作活体状态机(预留)
             access    认证融合、门控决策、开门、日志落库(验证事件的唯一出口)
             enroll    用户/特征录入管理(调用 storage 与算法查重)
             net       web 上位机 / OTA 监听 / NTP / mDNS(spec-network.md)
──────────────────────────────────────────────
HAL 层       camera / display(DRM) / npu(librknnrt) / gpio(libgpiod)
             uart(指纹+读卡) / storage(SQLite) / keypad(触摸密码)
             薄封装、可替换;PC 模拟器 = 同接口的 sim 后端
──────────────────────────────────────────────
平台层       官方 SDK(kernel 6.1 + rkaiq + rknpu2 + rga + mpp + libmali + buildroot)
```

- 模块间只经 `proto/` 的消息队列与事件总线通信;服务层线程化,互不直接调用
- 事件命名:`EV_<域>_<动作>`(如 `EV_AUTH_RESULT`、`EV_ENROLL_DONE`、`EV_NET_OTA_PROGRESS`)
- 新建模块步骤:本文档登记职责 → proto/ 定义接口与事件 → 实现 → 测试 → README + 使用示例

## 2. ESP32 模板复用(模板已到位)

模板位置:**`/home/olwhistle/dockerNow/esp32/programs/ovs`**(KickPi 开发机本地;
API 头文件在 `components/api/`,实现+移植层在 `components/core/`,自带 `tests/`、`sim/`)。
模板工程质量高:**公共 API 与 port 层分离**(tasker_port / event_bus_port),
FreeRTOS 依赖被隔离在 port 层,移植 = 实现对应 pthread port,不动核心与业务。

### 2.1 组件映射表

| 模板组件 | 位置(模板) | 关键 API(已确认) | 移植后职责 | 放置 |
|---|---|---|---|---|
| tasker | `components/api/tasker_api/tasker.h` + `components/core/tasker/` | `tasker_init()`、`tasker_enqueue(&node)`(移动语义)、`tasker_task_init_li(...)`(周期/一次性,`TASK_CNT_INF` 无限次)、`tasker_cancel_by_name()` | 服务层短任务调度(心跳/超时/轮询);**约定:≤500ms 短任务才进 tasker,长任务自建 pthread**(audio/web/lvgl 同此) | `proto/tasker/`(port 改 pthread) |
| event_bus | `components/core/event_bus/event_bus.h` | `event_bus_init()`、`event_bus_subscribe(type,cb,ud)`、`event_bus_publish()` / `EVENT_BUS_PUBLISH(type,&data)`、`event_bus_unsubscribe(sub)`;事件数据柔性数组,独立任务异步分发 | UI↔服务解耦骨干;door-guard 事件命名沿用 `EV_<域>_<动作>`,映射到 event_bus 的事件类型注册 | `proto/event_bus/` |
| holder | `components/modules/holder/holder.h` | `holder_module_state_t`(REGISTERED/INITIALIZING/READY/ERROR/DISABLED)、注册/初始化/依赖检查(`HOLDER_ERR_DEPENDENCY` 防循环依赖) | 全局模块注册表:HAL 与服务的启动顺序、健康状态(对应 main.c 装配顺序"故障即退出") | `proto/holder/` |
| lvgl 模块化 UI 思想 | 模板 `main/`+`sim/` 组织方式 | — | 对齐 spec-ui.md §4 的页面/组件/页面管理器组织 | ui/ |

### 2.2 移植纪律

- 先移 event_bus(被依赖最多)→ tasker → holder;每移一个:port 层实现(pthread 互斥/条件变量/
  后台线程)、跑通模板自带 `tests/test_tasker.c` `tests/test_event_bus.c`、附 README(原出处、
  port 改动点、使用示例)
- 只取 API 形态与实现,剥离 esp_* 头;日志接入 door-guard 日志体系
- 模板 `sim/`、`web/` 的组织方式在写 PC 模拟器与上位机时参考(不直接复制)

## 3. 构建体系

| 目标 | 命令 | 说明 |
|---|---|---|
| 板上(aarch64) | `source env/env.sh && dg-build` | 工具链文件 `cmake/aarch64.cmake`,产物 scp 推板 |
| PC 模拟器(x86_64) | `dg-build-pc`(待建,包一层 `cmake -DDG_SIM=ON`) | SDL2 窗口 720×1280;摄像头用 sim 后端;**调 UI 一律先过模拟器** |
| 单元测试 | `dg-test`(待建,ctest) | 纯逻辑(db 校验/状态机/i18n json 一致性)WSL 宿主跑,不依赖板/SDK |

- 代码里 `#ifdef DG_SIM` 只允许出现在 HAL sim 后端与 main 装配处,业务与 UI 层禁止
- rootfs 集成(B10):door-guard 做成 buildroot 包,开机自启替换 LVGL demo

## 4. 目录索引(door-guard/)

`app/` 装配启动 · `ui/` 界面 · `modules/{capture,vision,liveness,access}` ·
`auth/{face,finger,card}` · `hal/{camera,display,npu,gpio,uart,storage}` ·
`proto/` 消息与事件 · `configs/device.json` · `tests/` · `tools/` · `third_party/`
(职责细表见 door-guard/README.md)
