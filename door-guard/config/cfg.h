/*
 * cfg.h — 设备配置:device.json(出厂默认)+ DB device_config(用户设置)
 *
 * 解析优先级(后者覆盖前者):
 *   代码内置默认 → configs/device.json → DB device_config(spec-database §5)
 * 理由:json 随固件分发,是"出厂值";菜单/上位机改动的值必须持久且升级不丢,
 * 所以 DB 为最终事实。任何键缺失→默认;类型错/越界→回退默认并 WARN(不崩)。
 *
 * 快照只读:各服务经 cfg_get() 取值;设置页改动走 cfg_set()(写 DB 并刷新),
 * 不允许直接改快照。
 */
#ifndef DG_CFG_H
#define DG_CFG_H

#include <stdbool.h>
#include <stddef.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 门禁业务(spec-auth-business §5) */
    int  door_open_ms;         /**< 开门时长,1000~10000,默认 3000 */
    int  pwd_fail_lock_n;      /**< 密码连错锁定次数,1~10,默认 5 */
    int  pwd_fail_lock_s;      /**< 密码锁定时长,10~3600,默认 60 */
    double face_dup_threshold; /**< 入库人脸查重相似度阈值,0.50~1.00,默认 0.90 */
    double face_match_threshold; /**< 1:N/1:1 命中阈值,0.30~1.00,默认 0.42(ROCKIVA 相似度) */
    int  liveness_enable;      /**< 动作活体开关,0/1,默认 0(B8 算法落地后开启) */
    /* UI(spec-ui) */
    int  standby_timeout_s;    /**< 待机超时,15~60(spec 上限),默认 30 */
    char language[16];         /**< zh-CN / en-US,默认 zh-CN */
    /* 网络(spec-network) */
    int  web_port;             /**< web 上位机端口,1024~65535,默认 8080 */
    int  ota_port;             /**< OTA 监听端口,1024~65535,默认 9000 */
    char ntp_server[64];       /**< 默认 ntp.aliyun.com(国内部署实测可用) */
    char ota_url[128];         /**< OTA 升级包源地址,可空 */
    /* 硬件参数(json-only,不经 DB;引脚待硬件确认) */
    int  relay_gpio_line;      /**< 开门继电器 GPIO 行号,默认 0 */
    char relay_gpio_chip[32];  /**< GPIO 控制器(sysfs 模式下仅记录) */
} dg_cfg_t;

/**
 * 加载配置(json 路径 + DB 覆盖)。storage_init 必须先于本调用。
 * json 文件不存在/坏 json 不算错误:全部走默认并 WARN(出厂文件损坏时设备仍可用)。
 */
int cfg_load(const char *json_path);

/** 只读快照(加载后稳定;cfg_set 成功后更新) */
const dg_cfg_t *cfg_get(void);

/** 设置并持久化(写 DB device_config,校验通过后刷新快照);key 见 cfg_keys.h 语义 */
int cfg_set_int(const char *db_key, int value);
int cfg_set_str(const char *db_key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* DG_CFG_H */
