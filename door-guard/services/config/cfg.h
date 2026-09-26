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
    double face_dup_threshold; /**< 入库人脸查重相似度阈值,0.50~1.00,默认 0.90 */
    double face_match_threshold; /**< 1:N/1:1 命中阈值,0.30~1.00,默认 0.42(ROCKIVA 相似度) */
    int  liveness_enable;      /**< 动作活体开关,0/1,默认 0(B8 算法落地后开启) */
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
    int  web_port;             /**< web 上位机端口(含 OTA 上传端点),1024~65535,默认 8080 */
    char ntp_server[64];       /**< 默认 ntp.aliyun.com(国内部署实测可用) */
    char ota_url[128];         /**< OTA 升级包源地址,可空 */
    /* 硬件参数(json-only;引脚待硬件确认) */
    int  relay_gpio_line;      /**< 开门继电器 GPIO 行号,默认 0 */
    char relay_gpio_chip[32];  /**< GPIO 控制器(sysfs 模式下仅记录) */
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
int cfg_set_str(const char *key, const char *value);

/** 按项恢复默认:从 cur 删除该键(加载序回落 default);未知键 DG_ERR_PARAM */
int cfg_reset_key(const char *key);

/** 全部恢复默认:cur 清空;同步落盘后返回 */
int cfg_reset_all(void);

/** 同步强制落盘(原子写:临时文件→fsync→rename);无未落盘改动时为空操作 */
int cfg_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_CFG_H */
