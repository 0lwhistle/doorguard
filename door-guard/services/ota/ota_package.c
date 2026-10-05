/*
 * ota_package.c — .ota 全量升级包解析/提取实现
 *
 * 全部流式:parse 用 fseek 定位载荷、按块读 EVP 摘要(8MB 包至多几十 ms);
 * extract 同一读法拷到暂存并顺带算摘要,复核后才 rename 落位——中途断电
 * 只留 .part,现有暂存文件不受影响(与 ota_service 同款安全语义)。
 */
#include "ota_package.h"
#include "dg_log.h"

#include <strings.h>

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "[OTA-PKG]";

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void copy_field(char *dst, size_t cap, const char *src, size_t src_cap)
{
    size_t n = 0;
    while (n < src_cap && src[n])
        n++;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int ota_package_parse_header(const uint8_t hdr[OTA_PKG_HEADER_LEN],
                             ota_pkg_meta_t *out)
{
    if (!hdr || !out)
        return DG_ERR_PARAM;
    if (memcmp(hdr, OTA_PKG_MAGIC, OTA_PKG_MAGIC_LEN) != 0)
        return DG_ERR_PARAM;
    const uint32_t header_len = get_u32be(hdr + 8);
    if (header_len != OTA_PKG_HEADER_LEN)
        return DG_ERR_PARAM;                 /* 只认本版本布局 */
    memset(out, 0, sizeof(*out));
    copy_field(out->version, sizeof(out->version), (const char *)hdr + 16,
               OTA_PKG_VER_MAX);
    copy_field(out->date, sizeof(out->date), (const char *)hdr + 48,
               OTA_PKG_DATE_MAX);
    out->payload_size = get_u32be(hdr + 12);
    if (out->payload_size == 0)
        return DG_ERR_PARAM;
    static const char *const HEX = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out->sha256_hex[i * 2] = HEX[hdr[64 + i] >> 4];
        out->sha256_hex[i * 2 + 1] = HEX[hdr[64 + i] & 0xF];
    }
    out->sha256_hex[64] = '\0';
    return DG_OK;
}

int ota_package_build_header(uint8_t hdr[OTA_PKG_HEADER_LEN],
                             const char *version, const char *date,
                             uint32_t payload_size, const char *sha_hex)
{
    if (!hdr || !version || !sha_hex || strlen(sha_hex) != 64)
        return DG_ERR_PARAM;
    memset(hdr, 0, OTA_PKG_HEADER_LEN);
    memcpy(hdr, OTA_PKG_MAGIC, OTA_PKG_MAGIC_LEN);
    put_u32be(hdr + 8, OTA_PKG_HEADER_LEN);
    put_u32be(hdr + 12, payload_size);
    copy_field((char *)hdr + 16, OTA_PKG_VER_MAX, version, OTA_PKG_VER_MAX);
    if (date)
        copy_field((char *)hdr + 48, OTA_PKG_DATE_MAX, date, OTA_PKG_DATE_MAX);
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(sha_hex + i * 2, "%2x", &v) != 1)
            return DG_ERR_PARAM;
        hdr[64 + i] = (uint8_t)v;
    }
    return DG_OK;
}

/* 打开包文件并完成头部解析 + 总长一致性;返回 FILE*,调用方 fclose */
static FILE *open_checked(const char *path, ota_pkg_meta_t *meta)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        DG_LOGW(TAG, "包文件打不开:%s", path);
        return NULL;
    }
    uint8_t hdr[OTA_PKG_HEADER_LEN];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        ota_package_parse_header(hdr, meta) != DG_OK) {
        DG_LOGW(TAG, "包头部非法(非 .ota 或格式版本不符):%s", path);
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long total = ftell(f);
    if (total < 0 || (uint64_t)total != (uint64_t)OTA_PKG_HEADER_LEN +
                                             meta->payload_size) {
        DG_LOGW(TAG, "包总长 %ld 与头部载荷 %u 不符:%s", total,
                meta->payload_size, path);
        fclose(f);
        return NULL;
    }
    if (fseek(f, OTA_PKG_HEADER_LEN, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

/* 载荷摘要(从当前偏移读 payload_size 字节;同时可选拷贝到 out 流) */
static int digest_payload(FILE *f, uint32_t payload_size,
                          const char *expect_hex, uint8_t *sha_raw_out,
                          FILE *sink)
{
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md)
        return DG_ERR_NO_MEMORY;
    EVP_DigestInit_ex(md, EVP_sha256(), NULL);

    uint8_t buf[16 * 1024];
    uint32_t left = payload_size;
    while (left > 0) {
        const size_t want = left < sizeof(buf) ? left : sizeof(buf);
        const size_t got = fread(buf, 1, want, f);
        if (got != want) {
            EVP_MD_CTX_free(md);
            return DG_ERR_IO;
        }
        EVP_DigestUpdate(md, buf, got);
        if (sink && fwrite(buf, 1, got, sink) != got) {
            EVP_MD_CTX_free(md);
            return DG_ERR_IO;
        }
        left -= (uint32_t)got;
    }
    uint8_t raw[32];
    unsigned int raw_len = 0;
    EVP_DigestFinal_ex(md, raw, &raw_len);
    EVP_MD_CTX_free(md);

    if (sha_raw_out)
        memcpy(sha_raw_out, raw, 32);
    if (expect_hex) {
        char got_hex[65];
        static const char *const HEX = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            got_hex[i * 2] = HEX[raw[i] >> 4];
            got_hex[i * 2 + 1] = HEX[raw[i] & 0xF];
        }
        got_hex[64] = '\0';
        if (strcasecmp(got_hex, expect_hex) != 0) {
            DG_LOGW(TAG, "载荷摘要不符(头 %s / 实算 %s)", expect_hex, got_hex);
            return DG_ERR_MISMATCH;
        }
    }
    return DG_OK;
}

int ota_package_parse_file(const char *path, ota_pkg_meta_t *out)
{
    if (!path || !out)
        return DG_ERR_PARAM;
    ota_pkg_meta_t m;
    FILE *f = open_checked(path, &m);
    if (!f)
        return DG_ERR_PARAM;                 /* 打不开/结构坏统一按包无效 */
    const int rc = digest_payload(f, m.payload_size, m.sha256_hex, NULL, NULL);
    fclose(f);
    if (rc != DG_OK)
        return rc;
    *out = m;
    return DG_OK;
}

int ota_package_extract(const char *pkg_path, const char *staged_dir,
                        char *version_out, size_t ver_cap)
{
    if (!pkg_path || !staged_dir)
        return DG_ERR_PARAM;

    ota_pkg_meta_t m;
    FILE *src = open_checked(pkg_path, &m);
    if (!src)
        return DG_ERR_PARAM;

    char part[256], staged[256], sha_path[256], ver_path[256];
    snprintf(part, sizeof(part), "%s/ota_staging.part", staged_dir);
    snprintf(staged, sizeof(staged), "%s/ota_staged.bin", staged_dir);
    snprintf(sha_path, sizeof(sha_path), "%s/ota_staged.bin.sha256", staged_dir);
    snprintf(ver_path, sizeof(ver_path), "%s/ota_staged.bin.ver", staged_dir);

    FILE *dst = fopen(part, "wb");
    if (!dst) {
        fclose(src);
        DG_LOGE(TAG, "暂存文件打不开:%s", part);
        return DG_ERR_IO;
    }
    uint8_t raw[32];
    const int rc = digest_payload(src, m.payload_size, m.sha256_hex, raw, dst);
    fclose(src);
    fclose(dst);
    if (rc != DG_OK) {
        remove(part);                        /* 坏包不留半成品 */
        return rc;
    }

    if (rename(part, staged) != 0) {
        remove(part);
        DG_LOGE(TAG, "暂存落位失败:%s → %s", part, staged);
        return DG_ERR_IO;
    }
    /* sidecar 与裸包上传同契约(S60 二次复核用;写失败不回滚——上包侧
     * 与 web 直传同一处理口径) */
    FILE *sf = fopen(sha_path, "w");
    if (sf) {
        fprintf(sf, "%s  ota_staged.bin\n", m.sha256_hex);
        fclose(sf);
    }
    FILE *vf = fopen(ver_path, "w");
    if (vf) {
        fprintf(vf, "%s", m.version);
        fclose(vf);
    }
    if (version_out && ver_cap)
        copy_field(version_out, ver_cap, m.version, OTA_PKG_VER_MAX);
    DG_LOGI(TAG, "提取完成 v=%s(%uB)→ %s", m.version, m.payload_size, staged);
    return DG_OK;
}
