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

/** 当前暂存字节数(续传对拍) */
size_t ota_staged_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_OTA_SERVICE_H */
