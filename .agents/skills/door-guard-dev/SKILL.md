---
name: door-guard-dev
description: RK3576 K7 人脸识别门禁项目(door-guard)开发技能,含完整业务规格与工作流。凡在本仓库做任何开发都必须先用本技能:LVGL 界面(主页验证/待机/菜单/弹窗)、用户与门禁记录 SQLite、认证业务(1:N/1:1 人脸、指纹、密码、IC 卡)、网络功能(web 上位机/websocket/视频推流/OTA A/B 升级/NTP/mDNS)、HAL 驱动、PC 模拟器、测试与文档。用户提到门禁、door-guard、K7、识别、用户管理、OTA、上位机等即触发,即便没说"门禁"二字。
---

# door-guard 开发技能(RK3576 K7 人脸识别门禁)

## 0. 开工三步(每次必做,顺序固定)

1. `tail -n 30 docs/DEVLOG.md` —— 恢复上下文:上次做了什么、没做完什么(日志再长也只读最后 30 行)
2. 读 `PROJECT_PLAN.md` 第一节"项目快照"表 —— 当前阶段与待办
3. 按本次任务只读需要的 reference(渐进加载,不要一次全读):

| 本次任务 | 必读 |
|---|---|
| 用户/特征/日志存储、加密、任何 SQLite 改动 | references/spec-database.md |
| 主页验证流程、弹窗、超时、状态标志位 | references/spec-auth-business.md + spec-ui.md |
| 页面/待机/菜单/主题/多语言 | references/spec-ui.md |
| web 上位机、OTA、NTP、mDNS | references/spec-network.md |
| 新建模块、移植 ESP32 组件、构建体系 | references/architecture.md |
| 硬件实测结论(WiFi 坏、摄像头在 cam2、串口参数等) | docs/DEV_HANDBOOK.md §2/§4 |

## 1. 环境速查

```bash
source env/env.sh     # 仓库任意位置;dg-* 脚本进 PATH,自动探测工具链
dg-build              # 交叉编译 door-guard(-c 全新配置)
dg-deploy -r <IP>     # scp 推板并运行(export DOORGUARD_IP=<IP> 后免 IP)
dg-serial             # 串口控制台(1500000 8N1,不是 115200)
```

- 工具链:`~/dg-toolchain`(gcc-arm-10.3 + 与固件同源 sysroot),细节见 `docs/tech/TOOLCHAIN.md`
- 板端:SSH root 登录(串口 `passwd` 设密);固件 20260917-B4;WiFi 不可用,用以太网
- 代码流转只经 git(Gitea 192.168.2.150:3002,SSH 端口 222),禁止跨机复制目录
- VM 只做固件/内核/rootfs 全量编译(SDK `./build.sh`);WSL 不碰 SDK

## 2. 硬性工程纪律

- **模块化**:模块间只经 `proto/` 的消息/事件总线通信,禁止跨层直调;新建模块先对照 references/architecture.md 明确职责再动手
- **边界优先**:实现任何功能前先列边界情况(空输入/超时/重复/权限不足/并发/掉电恢复),每个错误显式处理并返回失败码,禁止静默吞掉
- **命名**:模块前缀式 `module_action()` / `module_type_t`;注释解释"为什么",不复述代码
- **完成的定义**:一个功能模块 = 代码 + 注释 + 测试案例(`door-guard/tests/`,WSL 宿主 gcc 可跑)+ 模块 README + 使用示例,缺一不算完成
- **UI**:所有 label 一律 `_("原文")` 包裹;翻译文件 `ui/lang/<语言>.json`(键=原文,值=译文);按钮一律图标+label;蓝白主题,色值 token 见 spec-ui.md
- **配置**:业务参数进 `configs/device.json` 或 DB device_config 表,代码零魔数
- 推板前 dg-build 无警告;测试不过不推板

## 3. 收尾纪律(每次会话结束前)

1. 在 `docs/DEVLOG.md` 顶部追加本次条目,**≤30 行**:做了什么 / 没做完什么 / 下一步
2. 有新的硬件实测结论或环境变化 → 同步 `docs/DEV_HANDBOOK.md`
3. git commit + push

## 4. 业务硬规则速记(细则在 references,冲突时以 references 为准)

- 用户上限 **2000**;`user_id` 唯一;人脸特征/指纹特征/IC 卡号跨用户唯一(重复 → 添加失败,错误码区分是哪种重复);密码可重复(验证是 ID+密码),且**新用户必须设置密码,否则禁止添加**
- 权限三级:普通(0)/ 管理员(1,**可多个**)/ 黑名单(2);黑名单走任何验证路径都失败
- 普通模式默认 1:N 人脸,只检索"开启人脸验证且非黑名单"的用户;**1.5s** 未命中或黑名单 → 红框 + 红色失败弹窗
- 管理员验证模式(点"菜单"进入)只认管理员;**5s** 内无人脸且未点"验证" → 自动回普通模式
- 点"验证":弹窗输入用户 ID → 按该用户开启的方式选 1:1 人脸 / 指纹 / 密码 / IC 卡;每步 **5s** 无操作自动回普通模式
- 弹窗期间:摄像头推流与脸框照常显示,但 **1:N 匹配必须挂起**(标志位管理,见 spec-auth-business.md)
- 每次验证动作(任何方式、成功或失败)都写 access_logs,供菜单查询页与上位机查询

## 5. ESP32 模板复用

用户提供 ESP32 工程模板(tasker / event_bus / holder / lvgl 模块化思想等)。移植约定:

- 只取**概念与接口形态**,不复制 ESP-IDF 依赖;FreeRTOS API 换成 pthread/Linux 等价物
- 移植代码放 `door-guard/third_party/` 或 `proto/`,每个组件附 README:原模块出处、改动点、使用示例
- 模板到位后,在 references/architecture.md 补"模板组件 ↔ door-guard 模块"映射表
