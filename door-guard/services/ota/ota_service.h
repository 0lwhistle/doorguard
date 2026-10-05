/*
 * ota_service.h — OTA 应用侧(A/B 分区方案,只写方案不刷固件)
 *
 * 接收:HTTP POST /ota/upload,流式落盘 /tmp/ota_staging.bin(不占内存);
 *      请求头携 manifest 字段(X-OTA-Version/-Size/-SHA256),服务端校验。
 * 校验:大小预检(超 MAX 拒收)+ 完整性 sha256(OpenSSL 流式)。
 * 恢复:X-OTA-Offset 续传(与已暂存字节数不符即 409 重传)。
 * 提交:校验闭环到暂存文件为止——**不写真实分区**(分区方案见
 *      docs/tech/OTA_PLAN.md,uboot env 约定同文档)。
 */
#ifndef DG_OTA_SERVICE_H
#define DG_OTA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    version[32];
    uint32_t size;                        /**< 声明的包大小 */
    char    sha256[65];                   /**< 声明的 sha256 hex(64 字符) */
} ota_manifest_t;

#define OTA_PIPE_CAP (256u * 1024u)     /**< 生产→写线程环形缓冲容量(对 web 限流可见) */

/** 开始接收(校验声明,打开暂存文件;offset>0 续传) */
int ota_begin(const ota_manifest_t *m, uint32_t offset, bool *resumed);

/** 流式写入一块(内部累计;环形满时阻塞等写线程腾位——**事件循环线程禁调**,
 *  先用 ota_can_accept() 限流) */
int ota_write_chunk(const uint8_t *data, size_t len, size_t *received);

/** 非阻塞余量(环形缓冲当前可接纳字节数;非会话期返回 0)。
 *  web 事件循环据此限流喂入,绝不让 ota_write_chunk 阻塞 loop */
size_t ota_can_accept(void);

/** 结束:校验已收大小与 sha256;通过则暂存文件改名为 staged */
int ota_finish(char *stage_path, size_t path_cap);

/** 取消并清理暂存 */
void ota_abort(void);

/* ---- web 升级包槽位模式(2026-10-05) ----
 * 与暂存模式共用同一单会话流水线(互斥 BUSY),差异只在收口:
 * 不做客户端声明摘要比对(.ota 包的载荷摘要在 apply 时由 ota_package
 * 复核),写完改名为 fw_slot.ota 并回报实算 sha256。 */

/** 开始槽位接收(size=整个 .ota 文件字节数;offset 续传语义同 ota_begin) */
int ota_begin_slot(uint32_t size, uint32_t offset, bool *resumed);

/** 结束槽位接收:path 回 fw_slot.ota 全路径,sha_hex 回整文件实算摘要 */
int ota_finish_slot(char *path, size_t path_cap, char *sha_hex, size_t sha_cap);

/** 当前是否有会话在途(暂存或槽位;web 上传/删除/apply 的互斥依据) */
bool ota_session_active(void);

/** fw_slot.ota 全路径(查询/删除用) */
void ota_slot_path(char *path, size_t cap);

/** 暂存目录全路径(ota_package_extract 的目标;web apply 用) */
void ota_staged_dir(char *path, size_t cap);

/** 校验闭环的暂存包是否在位(未装槽时 true;S60 装槽后进程重启自然翻假) */
bool ota_staged_present(void);

/** 当前暂存字节数(续传对拍) */
size_t ota_staged_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_OTA_SERVICE_H */
