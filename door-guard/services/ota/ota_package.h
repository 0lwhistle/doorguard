/*
 * ota_package.h — .ota 全量升级包(格式契约 + 解析/提取)
 *
 * 包格式(大端,96B 头 + 载荷;载荷 = door-guard 应用 ELF 本体):
 *   0   : magic "DGOTA1\0\0"(8B:魔数 + 格式版本 1)
 *   8   : u32 header_len(载荷在包内偏移;当前 = 96)
 *   12  : u32 payload_size
 *   16  : char version[32](NUL 结尾,出厂打包时 git describe 注入)
 *   48  : char date[16](YYYY-MM-DD)
 *   64  : u8 sha256[32](载荷摘要原始 32 字节)
 *   96  : payload
 *
 * 定位:web 上位机「固件升级」页与 MQTT OTA 共用的**全量包**入口。
 * web 槽位上传收的是整个 .ota 文件;apply 时本模块复核结构/摘要并把
 * 载荷提取成 ota_service 的暂存交接面(ota_staged.bin + sidecar),
 * 之后的装槽/切换/回滚全部复用 S60 ota_watch,与裸包上传同一出口。
 */
#ifndef DG_OTA_PACKAGE_H
#define DG_OTA_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_PKG_MAGIC       "DGOTA1\0\0"
#define OTA_PKG_MAGIC_LEN   8
#define OTA_PKG_HEADER_LEN  96u
#define OTA_PKG_VER_MAX     32
#define OTA_PKG_DATE_MAX    16

typedef struct {
    char     version[OTA_PKG_VER_MAX];
    char     date[OTA_PKG_DATE_MAX];
    uint32_t payload_size;
    char     sha256_hex[65];        /**< 载荷摘要(hex,大写不敏感比对) */
} ota_pkg_meta_t;

/** 解析并完整校验一个 .ota 文件(magic/头长/总长一致性/载荷 sha256)。
 *  通过返回 DG_OK 并回填 meta;失败返回 DG_ERR_PARAM(结构坏)或
 *  DG_ERR_IO(读不到),DG_ERR_MISMATCH(摘要不符)。文件不存在按 IO。 */
int ota_package_parse_file(const char *path, ota_pkg_meta_t *out);

/** 把包内载荷提取为 ota_service 的暂存交接面:流式拷贝 + 摘要复核 →
 *  staged_dir/ota_staged.bin,并写 .sha256/.ver sidecar(S60 契约)。
 *  version_out 回传包内版本(展示/日志)。本函数会先 parse 再提取;
 *  阻塞秒级,调用方须自行放 worker 线程(勿在事件循环调用)。 */
int ota_package_extract(const char *pkg_path, const char *staged_dir,
                        char *version_out, size_t ver_cap);

/* ---- 头部纯函数(宿主单测/打包工具对拍) ---- */

/** 从 96B 头部缓冲解出 meta(只做结构与形态校验,不验载荷摘要)。
 *  magic/头长非法 → DG_ERR_PARAM。 */
int ota_package_parse_header(const uint8_t hdr[OTA_PKG_HEADER_LEN],
                             ota_pkg_meta_t *out);

/** 序列化头部(打包工具/测试用;sha_hex 为 64 字符 hex,非法拒绝) */
int ota_package_build_header(uint8_t hdr[OTA_PKG_HEADER_LEN],
                             const char *version, const char *date,
                             uint32_t payload_size, const char *sha_hex);

#ifdef __cplusplus
}
#endif

#endif /* DG_OTA_PACKAGE_H */
