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

## 2. ESP32 模板复用(模板待用户提供)

目标组件与用途(接口形态为准,不引入 ESP-IDF 依赖,FreeRTOS→pthread):

| 模板组件 | 移植后职责 | 放置 |
|---|---|---|
| tasker | 周期/延迟任务调度(服务层各线程的心跳、超时统一由它驱动) | proto/ 或 third_party/ |
| event_bus | 线程安全发布订阅(UI↔服务解耦的骨干) | proto/ |
| holder | 单例资源持有/生命周期管理(HAL 句柄、服务的启动顺序) | proto/ |
| lvgl 模块化 UI 思想 | 页面/组件组织方式(对齐 spec-ui.md §4 目录结构) | ui/ |

移植纪律:每个组件附 README(原出处、API 对照表、改动点、使用示例)+ 单元测试;
模板到位后在此处补"模板组件 ↔ door-guard 文件"映射表。

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
