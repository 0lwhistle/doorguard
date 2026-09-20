/*
 * ota_service.c — OTA 应用侧实现(方案见 docs/tech/OTA_PLAN.md)
 *
 * 状态机:IDLE → RECEIVING → VERIFIED / FAILED
 * 并发:单实例(BUSY);sha256 用 OpenSSL EVP 流式,不整体驻内存。
 */
#include "ota_service.h"
#include "dg_log.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "[OTA]";

/* 暂存放持久区(/tmp 是 tmpfs,重启即失——切换流程依赖跨重启读不到它没关系,
 * 但安装动作发生在运行中的系统里,放持久区更稳);bash 监听/复核见
 * board/rootfs-overlay/etc/init.d/S60doorguard */
#define OTA_DIR       "/var/lib/door-guard"
#define OTA_STAGING   OTA_DIR "/ota_staging.part"
#define OTA_STAGED    OTA_DIR "/ota_staged.bin"
#define OTA_STAGED_SHA OTA_DIR "/ota_staged.bin.sha256"
#define OTA_STAGED_VER OTA_DIR "/ota_staged.ver"
#define OTA_MAX_SIZE  (64u * 1024u * 1024u)   /* 包上限 64MB(方案 A 分区预留) */

typedef struct {
    bool active;
    ota_manifest_t manifest;
    size_t received;
    EVP_MD_CTX *md;
} ota_ctx_t;

static ota_ctx_t s_ctx;

static int read_hex(const char *hex, uint8_t *out, size_t len)
{
    if (strlen(hex) != len * 2)
        return DG_ERR_PARAM;
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1)
            return DG_ERR_PARAM;
        out[i] = (uint8_t)v;
    }
    return DG_OK;
}

int ota_begin(const ota_manifest_t *m, uint32_t offset, bool *resumed)
{
    if (!m || !resumed)
        return DG_ERR_PARAM;
    if (s_ctx.active)
        return DG_ERR_BUSY;
    if (m->size == 0 || m->size > OTA_MAX_SIZE) {
        DG_LOGW(TAG, "包大小 %u 超限[%d,%u],开始前拒绝", m->size, 0, OTA_MAX_SIZE);
        return DG_ERR_PARAM;                /* 超目标大小:开始前拒绝 */
    }

    size_t staged = 0;
    FILE *probe = fopen(OTA_STAGING, "rb");
    if (probe) {
        fseek(probe, 0, SEEK_END);
        staged = (size_t)ftell(probe);
        fclose(probe);
    }

    *resumed = false;
    if (offset > 0) {
        /* 续传:偏移必须与已暂存严格一致 */
        if (offset != staged)
            return DG_ERR_STATE;
        *resumed = true;
    } else if (staged > 0) {
        remove(OTA_STAGING);                /* 全新上传:清残留 */
        staged = 0;
    }
    if (staged > m->size)
        return DG_ERR_STATE;

    mkdir(OTA_DIR, 0755);               /* 已存在则忽略 */
    s_ctx.manifest = *m;
    s_ctx.received = staged;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md)
        return DG_ERR_NO_MEMORY;
    EVP_DigestInit_ex(md, EVP_sha256(), NULL);
    if (staged > 0) {
        /* 续传:重放已收部分进摘要 */
        FILE *f = fopen(OTA_STAGING, "rb");
        if (!f) {
            EVP_MD_CTX_free(md);
            return DG_ERR_IO;
        }
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            EVP_DigestUpdate(md, buf, n);
        fclose(f);
    }
    s_ctx.md = md;
    s_ctx.active = true;
    DG_LOGI(TAG, "开始接收 v=%s size=%u offset=%zu resumed=%d",
            m->version, m->size, staged, *resumed);
    return DG_OK;
}

int ota_write_chunk(const uint8_t *data, size_t len, size_t *received)
{
    if (!s_ctx.active)
        return DG_ERR_STATE;
    if (!data && len > 0)
        return DG_ERR_PARAM;
    if (s_ctx.received + len > s_ctx.manifest.size)
        return DG_ERR_PARAM;                /* 超声明大小,写途中拒绝 */

    FILE *f = fopen(OTA_STAGING, s_ctx.received ? "ab" : "wb");
    if (!f)
        return DG_ERR_IO;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len)
        return DG_ERR_IO;

    EVP_DigestUpdate(s_ctx.md, data, len);
    s_ctx.received += len;
    if (received)
        *received = s_ctx.received;
    return DG_OK;
}

int ota_finish(char *stage_path, size_t path_cap)
{
    if (!s_ctx.active)
        return DG_ERR_STATE;
    if (s_ctx.received != s_ctx.manifest.size) {
        DG_LOGW(TAG, "大小不符 received=%zu 声明=%u", s_ctx.received,
                s_ctx.manifest.size);
        return DG_ERR_IO;                   /* 不完整:保留 .part 供续传 */
    }

    uint8_t digest[32];
    unsigned dlen = 0;
    EVP_DigestFinal_ex(s_ctx.md, digest, &dlen);
    EVP_MD_CTX_free(s_ctx.md);
    s_ctx.md = NULL;

    uint8_t expect[32];
    int rc = read_hex(s_ctx.manifest.sha256, expect, 32);
    if (rc != DG_OK) {
        s_ctx.active = false;
        return rc;
    }
    s_ctx.active = false;

    if (memcmp(digest, expect, 32) != 0) {
        DG_LOGW(TAG, "sha256 不符,拒收");
        remove(OTA_STAGING);                /* 校验失败:丢弃,不保留坏包 */
        return DG_ERR_IO;
    }

    remove(OTA_STAGED);
    if (rename(OTA_STAGING, OTA_STAGED) != 0)
        return DG_ERR_IO;

    /* sidecar:bash 监听器做二次复核与槽位记录用(写失败不影响闭环,
     * 应用侧 sha256 已验证通过) */
    FILE *sf = fopen(OTA_STAGED_SHA, "w");
    if (sf) {
        fprintf(sf, "%s  ota_staged.bin\n", s_ctx.manifest.sha256);
        fclose(sf);
    }
    FILE *vf = fopen(OTA_STAGED_VER, "w");
    if (vf) {
        fprintf(vf, "%s", s_ctx.manifest.version);
        fclose(vf);
    }
    if (stage_path)
        snprintf(stage_path, path_cap, "%s", OTA_STAGED);
    DG_LOGI(TAG, "校验闭环通过 v=%s → %s(不刷分区,方案见 OTA_PLAN.md)",
            s_ctx.manifest.version, OTA_STAGED);
    return DG_OK;
}

void ota_abort(void)
{
    if (s_ctx.md) {
        EVP_MD_CTX_free(s_ctx.md);
        s_ctx.md = NULL;
    }
    s_ctx.active = false;
}

size_t ota_staged_bytes(void)
{
    return s_ctx.active ? s_ctx.received : 0;
}
