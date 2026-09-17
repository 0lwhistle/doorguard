# door-guard — 门禁应用模块索引

RK3576 K7 人脸识别门禁主应用。业务规格唯一权威:仓库根 `.agents/skills/door-guard-dev/references/`
(五份:spec-database / spec-auth-business / spec-ui / spec-network / architecture)。

## 构建/测试/部署

```bash
source env/env.sh            # 仓库根执行
dg-build                     # 板上(aarch64)构建,-c 全新配置
dg-build-pc [-r]             # PC 模拟器(SDL2 720×1280),调 UI 一律先过模拟器
dg-test [--tsan]             # 宿主 ctest 全量(15 用例);--tsan 并发检查
dg-deploy [-r] <IP>          # 推板运行(默认 192.168.2.95)
dg-ota-upload <IP> <包>       # OTA 上传+校验闭环
tests/web/web_test.sh        # web 上位机功能验收
```

## 模块索引(层级:ui → services → hal ← proto 契约)

| 目录 | 职责 | README |
|---|---|---|
| `app/main.c` | 装配顺序:event_bus→tasker→storage→cfg→服务→UI;`#ifdef DG_SIM` 仅此处与 HAL sim 后端 | — |
| `proto/` | **契约层**:err.h(统一错误码)/types.h(表投影)/events.h(EV_* 事件契约)/dg_log | proto/README.md |
| `proto/event_bus/` | 发布订阅总线(移植自 ESP32 模板;锁外回调/池+堆兜底/原子统计) | proto/event_bus/README.md |
| `proto/tasker/` | 短任务调度(≤500ms 约定;cond 超时等待/原子化/自旋熔断) | proto/tasker/README.md |
| `proto/holder/` | 模块注册表(依赖分批初始化/循环依赖检测) | proto/holder/README.md |
| `config/` | 配置体系:默认→device.json→DB 三层覆盖;cfg_set 校验+持久化 | config/README.md |
| `hal/storage/` | SQLite(users/access_logs/device_config,DDL=spec)+ PBKDF2/AES-CTR + 特征查重迭代器 | hal/storage/README.md |
| `hal/gpio/` | 门控 GPIO(sysfs;libgpiod 待 rootfs) | hal/gpio/README.md |
| `hal/uart/` | 串口框架+mock 回环(协议等手册,不臆造) | hal/uart/README.md |
| `hal/camera/` | 相机 HAL:sim(图片循环)/board(rkaiq+V4L2,Phase 8/B7) | — |
| `hal/display/` | 显示后端:sim=SDL2 720×1280 / board=DRM dumb-buffer | — |
| `modules/access/` | **验证状态机 auth_fsm + access_service**(日志/开门唯一出口) | modules/access/README.md |
| `modules/vision/` | 视觉服务:特征槽位句柄 + sim mock / rockiva 占位 | — |
| `modules/enroll/` | 录入编排(请求→抓取→查重→入库→回执) | — |
| `modules/capture/` | 取流状态服务(EV_CAPTURE_STATE) | — |
| `modules/liveness/` | 活体占位(接口预留) | — |
| `modules/net/` | web 上位机(civetweb)/OTA(流式+sha256,方案 docs/tech/OTA_PLAN.md)/NTP/mDNS | — |
| `ui/` | LVGL 七页面+widgets+theme+多语言+生成字体 | ui/README.md |
| `configs/device.json` | 设备配置(出厂值;用户改动落 DB 覆盖) | config/README.md |
| `tests/` | ctest 用例(宿主 gcc;test_i18n 键覆盖/裸中文=0/字形覆盖) | — |
| `third_party/` | lvgl 8.3 / cjson / civetweb 1.16 / stb_image / lv_drivers(drm) / 字体生成工具说明 | 各 LICENSE |

## 验证状态机快速入口

- 规格与边界用例:`modules/access/README.md` + `tests/test_auth_fsm.c`
- 模拟器全流程走查步骤:`modules/access/README.md` 底部

## 待硬件确认清单(动态,详见 docs/DEVLOG.md)

继电器引脚/类型 · 指纹与读卡协议(UART) · 摄像头 rkaiq+V4L2 链路(B6/B7) · 触摸 GT9xx(待 B5 固件)
