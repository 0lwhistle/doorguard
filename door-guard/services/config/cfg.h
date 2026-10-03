/*
 * cfg.h — 设备配置:default.json(出厂模板)+ cur_config.json(现用配置)
 *
 * 架构 v2 M2①(spec 见 docs/architecture-v2-proposal.md §4.1):
 *   加载序:代码内置默认 → default.json(出厂模板,只读) → cur_config.json(现用,稀疏覆盖)
 *   - cur 不存在 → 首启迁移:把 DB device_config 中已知业务键一次性导出生成 cur,
 *     此后 DB 冻结(仅存 web 凭据等非配置数据),cfg_set 只写 cur 文件;
 *   - cfg_set_*:校验 → 内存生效 → 500ms 防抖原子落盘(临时文件→fsync→rename);
 *   - cfg_flush():同步强制落盘(测试与关机路径);cfg_reset_key/reset_all() 按项/全部
 *     恢复默认(从 cur 删键,加载序自然回落 default);
 *   - 任何键缺失→默认;类型错/越界→回退默认并 WARN(不崩)。
 *
 * 快照只读:各服务经 cfg_get() 取值;改动一律走 cfg_set/reset,不允许直接改快照。
 */
#ifndef DG_CFG_H
#define DG_CFG_H

#include <stdint.h>   /* int32_t(质量闸门等整型配置字段) */

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
    double face_dup_threshold; /**< 入库人脸查重余弦阈值,0.30~1.00,默认 0.50
                                    (板标定:本管线同人 0.40~0.60/陌生人 ≤0.3) */
    double face_match_threshold; /**< 1:N/1:1 命中阈值,0.30~1.00,默认 0.42(ROCKIVA 相似度) */
    int  liveness_enable;      /**< 动作活体开关,0/1,默认 0(B8 算法落地后开启) */
    int  antispoof_enable;     /**< 反欺骗单帧判定(MiniFASNet)开关,0/1,默认 0;
                                    模型上板+假体标定后开启。开启后 1:N 命中若判
                                    "疑似假体"→ 不开门,发起多模态二次验证 */
    double antispoof_threshold; /**< 反欺骗"真脸"分数下限,0~1:多帧平滑分低于它
                                     即判疑似假体(降级方案下误拒只多验一道,
                                     可放心调严),默认 0.50,须板上假体标定 */
    /* 视觉后端(json-only,不经 set/迁移:换模型/换后端属部署参数,见 services/vision/README.md) */
    char face_backend[16];     /**< 想要的后端名("rockiva"/"rknn"/"sim"),空=第一个注册的 */
    char face_model_dir[128];  /**< 模型目录,默认 /usr/lib(env DG_IVA_MODEL_DIR 优先) */
    char face_model_tag[64];   /**< 特征口径标识;换模型必须改它(旧特征作废) */

    /* 人脸质量闸门(face_quality.h;0 = 该项不启用,便于板上先只开清晰度标定) */
    int32_t face_min_px;        /**< 人脸框较小边最小像素,40~400,默认 80 */
    double  face_blur_min;      /**< 清晰度下限(Laplacian 方差),默认 50,须实测标定 */
    double  face_det_score_min; /**< 检测分数下限,0.30~1.00,默认 0.70 */
    double  face_det_threshold; /**< 检测器出框阈值,0.30~0.95,默认 0.60。
                                     i8 量化模型空场景有 0.5x 幻检(会唤醒待机/
                                     弹验证失败),0.5 顶不住;正脸实测 0.85+,
                                     余量充足。板上按 2s 检出日志标定 */
    int32_t face_lost_hold_ms;  /**< 人脸消失判定滞回(ms),0~2000,默认 200:
                                     连续无检超过该时长才发 FACE_LOST——单帧
                                     漏检(识别帧占用/分数抖动)不闪框;调小
                                     脸框跟手,调大稳但"框滞后于人" */
    /* UI(spec-ui) */
    int  standby_timeout_s;    /**< 待机超时,15~60(spec 上限),默认 30 */
    int  menu_timeout_s;       /**< 菜单页无操作自动回主页,5~120s,默认 15 */
    char language[16];         /**< zh-CN / en-US,默认 zh-CN */
    /* 网络(spec-network) */
    int  web_port;             /**< web 上位机端口(含 OTA 上传端点),默认 80;PC 模拟器等非 root 绑定失败自动回退 8080 */
    char ntp_server[64];       /**< 默认 ntp.aliyun.com(国内部署实测可用) */
    char ota_url[128];         /**< OTA 升级包源地址,可空 */
    char net_mode[8];          /**< 接口地址来源:"dhcp"(默认)/"static";应用在 modules/net/net_cfg */
    char net_ip[16];           /**< 静态 IP(net_mode=static 时生效),点分十进制 */
    char net_mask[16];         /**< 静态子网掩码,点分十进制 */
    char net_gw[16];           /**< 静态默认网关,空 = 不下发默认路由 */
    /* 硬件参数(json-only;引脚待硬件确认) */
    int  relay_gpio_line;      /**< 开门继电器 GPIO 行号,默认 0 */
    char relay_gpio_chip[32];  /**< GPIO 控制器(sysfs 模式下仅记录) */
    /* 指纹模组 AS608(json-only;FINGERPRINT_AS608.md §8;2026-09-30 板上实测定值) */
    char fp_uart_dev[32];      /**< 模组串口节点,默认 /dev/ttyS8(uart8,板上 status=okay 且空闲) */
    int  fp_baud;              /**< 模组波特率,默认 57600(协议 v1 冻结值) */
    int  fp_wak_gpio;          /**< WAK 触摸 GPIO 全局编号,默认 94(GPIO2_D6) */
    int  fp_wak_active;        /**< WAK"按下"电平:-1=自动(默认,启动采样静息
                                    电平反相定按下,fp_provider wak_calibrate);
                                    0/1=强制。按压极性是 FINGERPRINT_PROTOCOL
                                    §6.④ 开放项,自动档消除该未知数 */
    /* IC 读卡器(json-only;ICCARD_PROTOCOL §8;最终节点名由驱动定,只改配置) */
    char iccard_dev_path[32];  /**< 读卡器节点,默认 /dev/dg_iccard0;"sim"=宿主模拟后端 */
    /* MQTT 通道(json-only;services/mqtt。默认关:broker 地址属部署参数,
     * 硬接好之前不留对外连接面) */
    int  mqtt_enabled;         /**< 0/1,默认 0;开启后连 broker、订阅 cmd、转发验证事件 */
    char mqtt_uri[96];         /**< broker 地址 mqtt://host:1883;空 = 不连 */
    char mqtt_client_id[48];   /**< 空 = doorguard-<主接口IP 末段> 自动生成 */
    char mqtt_topic_prefix[32];/**< 主题前缀,默认 doorguard(cmd/status/event/rsp 挂其下) */
    char mqtt_username[32];    /**< broker 认证用户名,可空 */
    char mqtt_password[32];    /**< broker 认证口令,可空 */
    int  mqtt_allow_remote_open; /**< 远程开门命令开关,0/1,默认 0(安全默认:
                                        网络侧命令直接开锁须经部署方显式授权) */
    /* 语音播报(json-only;modules/audio。MAX98357 = I2S 功放,软件侧就是
     * 一条 ALSA PCM 输出,硬件接入后改 dts 使能声卡即可,应用零改动) */
    int  audio_enabled;        /**< 0/1,默认 1;后端打不开自动降级静默,不报障 */
    char audio_device[32];     /**< ALSA PCM 名,默认 default(硬件接入后可钉 hw:0,0) */
    int  audio_volume;         /**< 软件音量 0~100,默认 80(功放增益由硬件增益脚定) */
    char audio_prompt_dir[96]; /**< 语音 wav 目录,默认 /userdata/doorguard/audio
                                    (success.wav/fail.wav 缺席时降级为内置提示音) */
} dg_cfg_t;

/**
 * 加载配置(出厂模板 + 现用配置)。storage_init 须先于本调用(首启迁移读 DB)。
 * @param default_path 出厂模板 json(缺失/坏 → 内置默认,WARN 不拒启)
 * @param cur_path     现用配置 json(缺失 → 首启迁移生成;坏 → 视为空并 WARN)
 * @return DG_OK / DG_ERR_IO(cur 首启迁移写盘失败,内存值仍可用)
 */
int cfg_load(const char *default_path, const char *cur_path);

/** 只读快照(两次 set 之间稳定;cfg_set/reset 成功后更新) */
const dg_cfg_t *cfg_get(void);

/** 设置并持久化(防抖落盘);key 为业务键名(同 DB 迁移键,见 test_cfg.c) */
int cfg_set_int(const char *key, int value);
int cfg_set_dbl(const char *key, double value);
int cfg_set_str(const char *key, const char *value);

/** 按项恢复默认:从 cur 删除该键(加载序回落 default);未知键 DG_ERR_PARAM */
int cfg_reset_key(const char *key);

/** 全部恢复默认:cur 清空;同步落盘后返回 */
int cfg_reset_all(void);

/** 同步强制落盘(原子写:临时文件→fsync→rename);无未落盘改动时为空操作 */
int cfg_flush(void);

/** 查某业务键的合法域(INT/DBL 项;范围唯一事实源=META 表,web 上位机的
 *  滑条范围/校验从这里取,不再各自维护一份)。STR 键/未知键返回 false */
bool cfg_meta_range(const char *key, double *lo, double *hi);

#ifdef __cplusplus
}
#endif

#endif /* DG_CFG_H */
