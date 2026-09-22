# config — 配置体系(内置默认 → default.json → cur_config.json)

## 解析优先级(后者覆盖前者)

```
代码内置默认(cfg.c defaults_apply)
      ↓ 覆盖
configs/default.json(出厂模板,随固件分发;板上 /etc/door-guard/default.json)
      ↓ 覆盖
cur_config.json(现用配置,稀疏覆盖;板上 /userdata/doorguard/cur_config.json,
                PC 模拟器 sim/data/cur_config.json)
```

为什么现用值放 /userdata 的独立文件:json 随固件升级会被覆盖,用户设置必须
升级不丢(/userdata 持久分区,A/B 换 rootfs 不受影响)。DB device_config 仅在
**首次启动迁移时读一次**,此后冻结——新配置一律进 json,不再进 DB(v2 决议)。
任何一层缺键 → 用上一层值;类型错/越界 → WARN + 回退上一层值,**绝不崩**。

## 业务键(cfg.c META 表;json 路径即此处的点分路径)

| cfg 字段 / cfg_set 键 | json 路径 | 范围 | 默认 |
|---|---|---|---|
| door_open_ms | access.door_open_ms | 1000~10000 | 3000 |
| pwd_fail_lock_n | access.pwd_fail_lock_n | 1~10 | 5 |
| pwd_fail_lock_s | access.pwd_fail_lock_s | 10~3600 | 60 |
| face_dup_threshold | face.face_dup_threshold | 0.50~1.00 | 0.90 |
| face_match_threshold | face.match_threshold | 0.30~1.00 | 0.42 |
| min_face_px | face.min_face_px | 40~400 | 80 |
| blur_min | face.blur_min | 0~50000 | 50 |
| det_score_min | face.det_score_min | 0.30~1.00 | 0.70 |
| liveness_enable | face.liveness_enable | 0/1 | 0(B8 活体算法落地后开) |
| standby_timeout_s | ui.standby_timeout_s | 15~60 | 30 |
| menu_timeout_s | ui.menu_timeout_s | 5~120 | 15 |
| language | ui.language | zh-CN/en-US | zh-CN |
| web_port | network.web_port | 1024~65535 | 8080(含 OTA 上传端点) |
| ntp_server | network.ntp_server | 字符串 | ntp.aliyun.com |
| ota_url | network.ota_url | 字符串 | 空 |

视觉后端参数(json-only:换模型/换框架属部署动作,不进 cfg_set/迁移;
逐键说明与换模型流程见 `services/vision/README.md`):

| cfg 字段 | json 路径 | 默认 | 说明 |
|---|---|---|---|
| face_backend | face.backend | 空 | 后端名;空=第一个注册的(PC=sim / 板上=rknn,rockiva 备用) |
| face_model_dir | face.model_dir | /usr/lib | 模型目录;env `DG_IVA_MODEL_DIR` 优先于它 |
| face_model_tag | face.model_tag | 空 | 特征口径标识;空=后端自带默认。**换模型必须改** |

继电器引脚(json-only 硬件参数):`access.relay_gpio_chip`(默认
/dev/gpiochip0)、`access.relay_gpio_line`(默认 0)。

default.json 不再收录 HAL 硬件参数(相机分辨率/旋转等走环境变量,见
`modules/camera/README.md`);本模块忽略未知键。

## 使用示例

```c
#include "cfg.h"

/* main 装配:storage_init 之后(双参:出厂模板 + 现用配置) */
cfg_load("/etc/door-guard/default.json", "/userdata/doorguard/cur_config.json");

/* 各服务读取(只读快照) */
const dg_cfg_t *c = cfg_get();
door_open_for(c->door_open_ms);

/* UI 设置页改动(校验 → 写 cur_config.json → 刷快照) */
cfg_set_int("standby_timeout_s", 45);
cfg_set_str("language", "en-US");       /* 经 EV_UI_HINT(DG_HINT_LANG_RELOAD)
                                           通知 UI 整页重建(navigator_reload) */
```

## 测试

`tests/test_cfg.c`:缺失文件/坏 json/缺键/类型错/越界 → 不崩回退默认;
双层覆盖优先级(cur 覆盖 def);非法值回落;cfg_set 拒绝越界、持久化
跨"重启"生效、未知键显式拒绝。
