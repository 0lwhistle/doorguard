# config — 配置体系(默认 → device.json → DB)

## 解析优先级(后者覆盖前者)

```
代码内置默认(cfg.c defaults_apply)
      ↓ 覆盖
configs/device.json(出厂值,随固件分发)
      ↓ 覆盖
DB device_config(用户经菜单/web 的改动,持久)
```

为什么 DB 是最终事实:json 随固件升级会被覆盖,用户的设置必须升级不丢。
任何一层缺键 → 用上一层值;类型错/越界 → WARN + 回退上一层值,**绝不崩**。

## 业务键(spec-database §5 预置键;json 路径 → DB 键)

| DB 键 / cfg 字段 | json 路径 | 范围 | 默认 |
|---|---|---|---|
| door_open_ms | access.door_open_ms | 1000~10000 | 3000 |
| pwd_fail_lock_n | access.pwd_fail_lock_n | 1~10 | 5 |
| pwd_fail_lock_s | access.pwd_fail_lock_s | 10~3600 | 60 |
| face_dup_threshold | face.face_dup_threshold | 0.50~1.00 | 0.90 |
| face_match_threshold | face.match_threshold | 0.30~1.00 | 0.42 |
| liveness_enable | face.liveness_enable | 0/1 | 0(B8 活体算法落地后开) |
| standby_timeout_s | ui.standby_timeout_s | 15~60(spec 上限) | 30 |
| language | ui.language | zh-CN/en-US | zh-CN |
| web_port | network.web_port | 1024~65535 | 8080 |
| ota_port | network.ota_port | 1024~65535 | 9000 |
| ntp_server | network.ntp_server | 字符串 | ntp.aliyun.com |
| ota_url | network.ota_url | 字符串 | 空 |

device.json 中其余键(camera/display/stream/storage 等硬件参数)由 HAL
阶段各自模块读取,本模块忽略未知键。

## 使用示例

```c
#include "cfg.h"

/* main 装配:storage_init 之后 */
cfg_load("/etc/door-guard/device.json");

/* 各服务读取(只读快照) */
const dg_cfg_t *c = cfg_get();
door_open_for(c->door_open_ms);

/* UI 设置页改动(校验 → 写 DB → 刷快照) */
cfg_set_int("standby_timeout_s", 45);
cfg_set_str("language", "en-US");       /* 成功后发 EV_UI_REFRESH_REQUEST */
```

## 测试

`tests/test_cfg.c`:缺失文件/坏 json/缺键/类型错/越界 → 不崩回退默认;
DB 覆盖 json 的优先级;非法 DB 值回落 json;cfg_set 拒绝越界、持久化
跨"重启"生效、未知键显式拒绝。
