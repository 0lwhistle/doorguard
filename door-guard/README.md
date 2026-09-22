# door-guard — 门禁应用模块索引

RK3576 K7 人脸识别门禁主应用。业务规格唯一权威:仓库根 `.agents/skills/door-guard-dev/references/`
(五份:spec-database / spec-auth-business / spec-ui / spec-network / architecture)。
架构 v2 方案与迁移映射:仓库根 `docs/architecture-v2-proposal.md`(2026-09-20 评审通过)。

## 构建/测试/部署

```bash
source env/env.sh            # 仓库根执行
dg-build                     # 板上(aarch64)构建,-c 全新配置
dg-build-pc [-r]             # PC 模拟器(SDL2 720×1280),调 UI 一律先过模拟器
dg-test [--tsan]             # 宿主 ctest 全量(30 用例,含 demo 冒烟);--tsan 并发检查
dg-deploy [-r] <IP>          # 推板运行(默认 192.168.2.95)
dg-ota-upload <IP> <包>       # OTA 上传+校验闭环
tests/web/web_test.sh        # web 上位机功能验收
```

## 模块索引(层级:ui → services → modules → drv ← components + proto 契约)

| 目录 | 职责 | README |
|---|---|---|
| `app/main.c` | 装配顺序:event_bus→tasker→storage→cfg→服务→UI;`#ifdef DG_SIM` 仅此处与 sim 后端 | — |
| `proto/` | **契约层**:err.h(统一错误码)/types.h(表投影)/events.h(EV_* 事件契约)/valid.h(输入校验唯一权威) | proto/README.md |
| `components/event_bus/` | 发布订阅总线(移植自 ESP32 模板;锁外回调/池+堆兜底/原子统计) | components/event_bus/README.md |
| `components/tasker/` | 短任务调度(≤500ms 约定;cond 超时等待/原子化/自旋熔断) | components/tasker/README.md |
| `components/holder/` | 模块注册表(依赖分批初始化/循环依赖检测) | components/holder/README.md |
| `components/logger/` | 极简日志 dg_log(移植组件共用;统一日志体系落地前的基础设施) | — |
| `services/config/` | 配置服务:内置默认→default.json(出厂)→cur_config.json(现用)双文件;DB 仅首启迁移读一次(已冻结);cfg_set 校验+持久化 | services/config/README.md |
| `modules/sqlite/` | SQLite(users/access_logs/device_config,DDL=spec)+ PBKDF2/AES-CTR + 特征查重迭代器 | modules/sqlite/README.md |
| `drv/gpio/` | 门控 GPIO(sysfs;libgpiod 待 rootfs) | drv/gpio/README.md |
| `drv/uart/` | 串口框架+mock 回环(协议等手册,不臆造) | drv/uart/README.md |
| `drv/npu/` | RKNN 运行时薄封装 + RGA 预处理(全仓唯一 include rknn_api.h) | drv/npu/README.md |
| `modules/camera/` | 相机模块:sim(图片循环)/board(rkaiq+V4L2 NV12→RGA 旋转出 RGB) | modules/camera/README.md |
| `modules/display/` | 显示后端:sim=SDL2 720×1280 / board=DRM dumb-buffer + 触摸 evdev | modules/display/README.md |
| `services/access/` | **验证状态机 auth_fsm + access_service**(日志/开门唯一出口) | services/access/README.md |
| `services/vision/` | 视觉服务:特征槽位句柄 + sim/rknn/rockiva 三后端可插拔(板上主线 rknn) | services/vision/README.md |
| `services/enroll/` | 录入编排(请求→抓取→查重→入库→回执) | — |
| `services/capture/` | 取流状态服务(EV_CAPTURE_STATE) | — |
| `services/liveness/` | 活体占位(接口预留;认证管线强制阶段) | — |
| `services/verify/` | auth_provider 统一认证抽象 + face/fingerprint/ic provider(B10b) | services/verify/auth_provider.h |
| `services/web/` | web 上位机(mongoose 统一事件循环 + Vue 前端 `frontend/`,产物内嵌) | services/web/README.md |
| `services/ota/` | OTA(流式+sha256,方案 docs/tech/OTA_PLAN.md) | — |
| `services/ntp/` `services/mdns/` | 时间同步 / mDNS 服务通告 | — |
| `modules/net/` | 网络模块(net_info 网口信息 + **netcore** 统一事件循环:web/OTA/NTP/mDNS 的唯一传输层) | modules/net/README.md |
| `ui/` | LVGL 十页面+widgets+theme+多语言+生成字体 | ui/README.md |
| `configs/default.json` | 设备配置出厂模板(现用值落 /userdata 的 cur_config.json 稀疏覆盖;DB 已冻结) | services/config/README.md |
| `tests/` | ctest 用例(宿主 gcc;test_i18n 键覆盖/裸中文=0/字形覆盖) | — |
| `third_party/` | lvgl 8.3 / cjson / mongoose 7.23(GPLv2)/ stb_image / lv_drivers(drm) / 字体生成工具说明 | 各 LICENSE |

## 验证状态机快速入口

- 规格与边界用例:`services/access/README.md` + `tests/test_auth_fsm.c`
- 模拟器全流程走查步骤:`services/access/README.md` 底部

## 待硬件确认清单(动态,详见 docs/DEVLOG.md)

继电器引脚/类型 · 指纹与读卡协议(UART) · 摄像头 rkaiq+V4L2 链路(B6/B7) · 触摸 GT9xx(待 B5 固件)
