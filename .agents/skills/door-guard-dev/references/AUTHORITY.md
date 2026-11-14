# 文档权威层 — doorguard 项目实例

> 本文件是文档权威层在本项目的**管理面配置**：只存路由与验证方式，不存事实内容。
> 协议正文见通用技能 `doc-authority`（~/.agents/skills/doc-authority/）；本文件随 git 入库，
> 改动有 diff 审查。2026-11-14 全仓审计（docs/DOCUMENT-AUDIT-2026-11-14.md，本地）后建立。

## 路由表（事实类型 → 唯一权威源）

| 事实类型 | 唯一权威源 | 其他地方怎么写 |
|---|---|---|
| 业务语义/规则 | 本目录 spec-*.md | 记忆/README 只放指针，不复述 |
| 接口/配置键/默认值 | 代码（services/config/cfg.h+cfg.c、各头文件） | 写键名+「默认值见 cfg.c」，不复制数字 |
| 数量/体积/耗时 | 实跑命令（ctest -N / ls -l / 脚本输出） | 带日期戳+「以实跑为准」 |
| 历史决策（何时/为何/为何回退） | docs/DEVLOG.md + git log（append-only 永不改写） | 引用带 commit 锚点 |
| 操作流程/环境 | docs/DEV_HANDBOOK.md | 记忆放指针 |
| 板上/环境状态 | 现场命令现取（readlink /root/door-guard、md5sum、ping doorguard.local） | 只存「怎么取」；存值必须带快照日期戳 |
| 事件契约 | door-guard/proto/events.h | 删事件必须 retire 旧名（见下） |
| web 路由/端点 | door-guard/services/web/web_server.c 路由表 | 文档与代码对表 |
| UI 页面清单 | door-guard/ui/presenters/*.c 的 navigator_register 调用（上限=navigator.h NAV_MAX_PAGES） | 写「N 页」必须现数 |
| 模型清单/校验 | door-guard/models/README.md + sha256sums.txt | |

## 契约文件哨兵（L2 判定：工作区 diff 命中即提示出示销账记录）

- door-guard/proto/events.h
- door-guard/proto/types.h
- door-guard/services/config/cfg.h
- door-guard/configs/default.json
- door-guard/services/web/web_server.c
- door-guard/services/web/frontend/src/api/

## 验证注册表（claim → 命令 → 期望输出子串 → 上次验证；verify 自动比对期望）

| claim | 命令 | 期望 | 上次验证 |
|---|---|---|---|
| ctest 静态注册 55 | awk '/dg_add_test\(/ {n1++} /dg_add_demo\(/ {n2++} /add_test\(NAME/ && !/\$\{name\}/ {n3++} END {print n1+n2+n3+0}' door-guard/tests/CMakeLists.txt | 55 | 2026-11-14（静态计数 36+16+3,宏体模板行已排除;运行时口径=ctest -N,构建缓存可能过期如 Windows 侧现 52） |
| UI 页面 16 | awk '/navigator_register\(&ops\)/{n++} END{print n+0}' door-guard/ui/presenters/*.c | 16 | 2026-11-14 |
| DG_FEATURE_MAX=2048 | grep "define DG_FEATURE_MAX" door-guard/proto/types.h | 2048 | 2026-11-14 |
| face.face_dup_threshold 默认 0.50 | grep "dup_threshold" door-guard/services/config/cfg.c | 0.50 | 2026-11-14 |
| web_stream 10fps | grep "define STREAM_FPS" door-guard/services/web/web_stream.c \| tr -s " " | STREAM_FPS 10 | 2026-11-14 |
| DOORGUARD_IP 缺省 .95 | grep "DOORGUARD_IP:-" env/env.sh | 192.168.2.95 | 2026-11-14 |

## 灭绝词汇表（现行声明命中即漂移报警；历史区命中属预期，人工判断）

- liveness_service（2026-11-14 retire：cf9ca5c 删除空壳模块，动作活体 bad656d 弃用）
- civetweb（2026-11-14 retire：2026-09-22 退役改 mongoose 7.23）
- /api/stream/mjpeg（2026-11-14 retire：实装为 WS 二进制 JPEG 快照流）
- 192.168.2.96（2026-11-14 retire：DOORGUARD_IP 缺省已 .95）
- GT9xx@0x5D（2026-11-14 retire：「当前屏=GT9xx」旧论断失效,现屏 fts_ts;GT9xx 作为 DTS 共存驱动/早期屏历史提法属合法,不算漂移）
- DG_USE_LVGL9（2026-11-14 retire：开关与 8.3 回退通道已删，B5-3）
- auth_provider.h（2026-11-14 retire：v2 草案已废，判定走 auth_fsm）
- EV_NET_OTA_PROGRESS（2026-11-14 retire：事件已删，进度走 /api/ota/fw 轮询）
- 131KB（2026-11-14 retire：app.js 现 164,021B）
- 28 用例（2026-11-14 retire：测试计数旧口径）
- 34 用例（2026-11-14 retire：测试计数旧口径）

## 历史区白名单（append-only/审计留档，哨兵不查）

- .agents/skills/door-guard-dev/references/AUTHORITY.md
- docs/DEVLOG.md
- docs/DOCUMENT-AUDIT-2026-11-14.md
- docs/lvgl9-migration-LOG.md
- docs/REVIEW-TODO-2026-10-24.md
- docs/review-2026-10-24/
- docs/superpowers/
- docs/prompts/

注意：PROJECT_PLAN.md §4.4 弃用存档块、architecture-v2-proposal 历史登记节、S60doorguard 内「已退役」注释属**活文件内的历史段落**，哨兵会命中——按「被当作现行事实才算漂移」人工判断，勿机械清零。

## 行级豁免（人工判定为合法历史说明，格式：`- 路径前缀:内容子串`；2026-11-14 首轮 triage）

- door-guard/services/web/README.md:civetweb 在 OpenSSL 3 下 WebSocket 握手必崩
- door-guard/services/web/README.md:修法见 `third_party/civetweb/dg_openssl_shim.h`
- door-guard/services/web/web_server.c:civetweb 4-worker 线程池
- door-guard/services/web/web_server.c:civetweb "状态行被吞"
- door-guard/modules/display/README.md:DG_USE_LVGL9` 开关与

## 更新日志（L2/L3 记账，append-only）

- 2026-11-14 L3 全仓审计收口：文档漂移 45 条修复（df6b1be）、PROJECT_PLAN+DEV_HANDBOOK 脱敏入库（6447c32）、AUTHORITY.md 建立、灭绝词汇表录入 11 词。
- 2026-11-14 L2 文档权威层落地:AUTHORITY.md 建立(11 灭绝词/6 注册表行/5 行级豁免);首轮 verify 抓出并修复 9 处 .96 缺省 IP 残留+PROJECT_PLAN liveness 残留销账
