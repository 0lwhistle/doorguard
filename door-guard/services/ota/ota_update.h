/*
 * ota_update.h — MQTT OTA 升级编排(公告评估 → 下载 → 校验 → 暂存交接)
 *
 * 职责边界:本服务只到「校验闭环的暂存文件」为止——写入非活动槽、符号
 * 链接切换、秒退回滚由板端 S60 ota_watch 统一消费(docs/tech/OTA_PLAN.md),
 * 与 web 手动上传同一交接面。MQTT 是控制面(公告/查询),固件本体走 HTTP
 * 直链下载(公告携带 url,MB 级载荷不过 broker)。
 *
 * 平台契约(主题前缀 <p> = cfg mqtt.topic_prefix):
 *   <p>/ota/version  平台→设备,retain 公告最新版:
 *     {"version":"1.2.0","date":"2026-10-05","url":"http://.../dg_app.bin",
 *      "sha256":"<64hex>","size":1234567,"notes":"..."}
 *   <p>/ota/query    设备→平台:手动「检查更新」请求,平台收到后重发上面的
 *                    retain 公告即完成应答
 *   <p>/ota/state    设备→平台:下载/暂存状态可见性上报(宁丢不堵)
 *
 * 版本比较:ota_version_newer() 取「点分数字前缀」逐段比(可带 v/V 前缀
 * 与任意后缀);任一侧解析失败退化为「字符串不同即视为有新版」。
 */
#ifndef DG_OTA_UPDATE_H
#define DG_OTA_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_UPD_IDLE = 0,       /**< 无事(无公告或公告版本不比当前新) */
    OTA_UPD_QUERYING,       /**< 已发 ota/query 等平台应答(>10s 无响应按 IDLE 呈现) */
    OTA_UPD_AVAILABLE,      /**< 有新版待处理(自动关:等用户「立即更新」) */
    OTA_UPD_DOWNLOADING,    /**< 下载中(permille 进度) */
    OTA_UPD_STAGED,         /**< 校验闭环已暂存,S60 接手装槽重启(终态) */
    OTA_UPD_FAILED,         /**< 下载/校验失败(err 携带原因;公告仍有效可重试) */
} ota_upd_state_t;

typedef struct {
    ota_upd_state_t state;
    int32_t  err;             /**< FAILED 时 dg_err_t,其余 DG_OK */
    uint32_t permille;        /**< DOWNLOADING 进度 0~1000 */
    char     version[32];     /**< 可用/在装版本(IDLE/QUERYING 为空) */
    char     date[16];        /**< 公告携带的发布日期 */
    char     notes[128];      /**< 公告携带的更新说明 */
} ota_upd_status_t;

/** 启动(幂等):注册 mqtt 订阅 ota/version、复位状态机。mqtt 未启用时
 *  空转(与 mqtt_service 同款语义),服务层降级不阻塞整机 */
int ota_update_start(void);

/** 停止:撤订阅、等待在途下载线程收尾(幂等) */
void ota_update_stop(void);

/** 当前状态快照(任意线程;QUERYING 超过 10s 自动按 IDLE 呈现=查询无响应) */
void ota_update_status(ota_upd_status_t *out);

/** 手动「检查更新」:发 <p>/ota/query;mqtt 未启用/未连接 → DG_ERR_NETWORK */
int ota_update_check(void);

/** 手动「立即更新」:开始下载公告版本(AVAILABLE → DOWNLOADING)。
 *  非 AVAILABLE → DG_ERR_STATE;下载已在途 → DG_ERR_BUSY */
int ota_update_apply(void);

/* ---- 注入口(测试/未来 TLS 传输替换;NULL = 恢复内置 HTTP) ---- */

typedef struct {
    /** 打开源流:url 直链;*content_len 回报包大小(未知填 0,下载器会以
     *  公告 size 为准)。返回 dg_err_t。 */
    int (*open)(void *ud, const char *url, uint32_t *content_len);
    /** 读一块(阻塞语义,在下载线程调用);流尽 got=0。返回 dg_err_t */
    int (*read)(void *ud, uint8_t *buf, size_t cap, size_t *got);
    void (*close)(void *ud);
} ota_transport_t;

void ota_update_transport_set(const ota_transport_t *t, void *ud);

/** 当前固件版本(= DG_FW_VERSION,git describe;测试与展示用) */
const char *ota_update_current_version(void);

/* ---- 纯函数(宿主单测) ---- */

/** 版本比较:cand 是否比 cur 新。规则见头注;cur 为空(unknown)且 cand
 *  可解析 → 视为新 */
bool ota_version_newer(const char *cur, const char *cand);

/** 公告 JSON 处理入口(mqtt 订阅回调同款签名;也公开给测试直驱)。
 *  内部:解析 → 与当前版本比较 → 新版则进 AVAILABLE(自动开则立即下载) */
void ota_update_on_announce(const char *payload);

/* ---- 内置 HTTP 传输(http 直链;src = ota_http 上下文,调用方 calloc/
 * free——与注入传输的同款生命周期,见 ota_update.c) ---- */

void *ota_http_src_new(void);
void  ota_http_src_free(void *src);
int   ota_http_open(void *src, const char *url, uint32_t *content_len);
int   ota_http_read(void *src, uint8_t *buf, size_t cap, size_t *got);
void  ota_http_close(void *src);

#endif /* DG_OTA_UPDATE_H */
