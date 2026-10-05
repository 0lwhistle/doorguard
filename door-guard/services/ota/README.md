# ota — OTA 升级(services/ota)

> 2026-10-05 起 = 两件套:`ota_service`(收包校验流水线,2026-09-22)+
> `ota_update`(MQTT 公告/下载编排,2026-10-05)。交接终点一致:校验闭环的
> 暂存文件;装槽/切换/回滚由板端 S60 `ota_watch` 统一消费
> (docs/tech/OTA_PLAN.md)。

## 两条升级入口,一个交接面

| 入口 | 触发 | 传输 | 说明 |
|---|---|---|---|
| web 上传 | 上位机手动 | HTTP POST /api/ota/upload(流式+断点续传) | 主机侧工具 `dg-ota-upload` |
| MQTT 公告 | 平台推送/用户「检查更新」 | MQTT 控制面 + HTTP 直链下载 | 本目录 `ota_update` |

两者最终都写 `<DG_OTA_DIR>/ota_staged.bin`(+ .sha256/.ver sidecar),
S60 轮询到即装**非活动槽** → 符号链接原子切换 → 重启;新版本 15s 内连续
3 次秒退自动回滚。同一时刻 ota_service 只有一个会话,冲突方收 BUSY。

## MQTT OTA 流程(ota_update)

```
平台 --retain--> <p>/ota/version {"version","date","url","sha256","size","notes"}
设备:公告解析 → ota_version_newer(与 DG_FW_VERSION 比)→
  自动更新开(cfg ota_auto_update)→ 立即下载
  自动更新关 → EV_NET_OTA_UPDATE(AVAILABLE)→ 关于设备页亮「立即更新」
用户「检查更新」→ 设备发 <p>/ota/query → 平台重发 retain 公告
下载(worker 线程):HTTP 直链(ota_http,Content-Length 必须、跟随 3 次
  重定向、https/chunked 显式拒绝)→ ota_service 流水线(sha256 校验)
  → ota_finish 暂存 → EV(STAGED)→ S60 接手装槽重启
进度/终态:EV_NET_OTA_UPDATE(UI)+ <p>/ota/state(平台,宁丢不堵)
```

- **版本比较**:取点分数字前缀逐段比(v/V 前缀与第 3 段后缀忽略)。
  **发布请打 tag 或用纯 x.y.z**——git describe 形如 `1.2.3-12-gabc` 的
  「12 个提交增量」不参与比较,同 tag 重发不会触发升级。
- **公告字段**:version/sha256(64hex)/size 必填;date/notes 展示用;
  url 空则回落 cfg `ota_url`。
- **失败语义**:下载/校验失败 → FAILED(公告保留,同版本重公告或用户
  重按「立即更新」可重试);sha 不符坏包不落 staged。

## 关于设备页(消费端)

菜单 → 设备管理 → 关于设备:设备名称(cfg `device.name`)/固件版本
(DG_FW_VERSION = git describe,构建期注入)/构建日期(DG_BUILD_DATE)+
最新版本/发布日期/更新说明/状态行;「检查更新」(MQTT 在线才可点)、
「自动更新」开关(cfg `ota_auto_update`,json-only 持久化)、「立即更新」
(仅 AVAILABLE 且自动关时出现)。

## 配置

| 键 | 默认 | 说明 |
|---|---|---|
| `network.ota_url` | 空 | 公告缺 url 时的下载回落地址 |
| `network.ota_auto_update` | 0 | 检到新版本自动下载升级(关于设备页可切) |
| `device.name` | Doorguard | 关于设备页展示 |

## 测试

`tests/test_ota_update.c`(宿主,传输注入零网络依赖):版本比较规则表、
公告解析丢弃、旧公告忽略、无通道查询、手动全流(暂存落位+事件序列)、
sha 不符拒收、大小对拍、缺 url、慢源在途 BUSY。`tests/test_ota.c` 覆盖
ota_service 流水线本身(续传/超限/进度事件)。

```bash
./build-tests/test_ota_update && ./build-tests/test_ota
```

## 使用示例(业务侧只碰 ota_update.h)

```c
#include "ota/ota_update.h"

ota_update_start();                     /* 装配期;mqtt 未启用则空转 */
ota_update_check();                     /* 手动检查(需 mqtt 在线) */
ota_update_apply();                     /* 用户按「立即更新」 */
ota_upd_status_t st; ota_update_status(&st);   /* 状态快照 */
/* 事件:EV_NET_OTA_UPDATE(ev_ota_update_t)——UI/其他消费方订阅 */
```
