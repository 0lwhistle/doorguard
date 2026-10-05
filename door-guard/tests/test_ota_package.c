/*
 * test_ota_package.c — .ota 全量升级包解析/提取测试(宿主;DG_OTA_DIR 临时目录)
 *
 * 覆盖:头部序列化/解析往返 / magic 坏 / 头长不符 / size=0 / 完整包解析
 * (摘要相符)/ 摘要不符拒 / 截断包拒 / extract → ota_staged.bin 字节一致
 * + sidecar 落位 / 版本字段截断保护。
 */
#include "dg_test.h"
#include "ota/ota_package.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAYLOAD_SZ 5000u

static char s_dir[64];
static char s_pkg[96];
static uint8_t s_payload[PAYLOAD_SZ];

static void make_sha(char *out, size_t cap, const uint8_t *data, size_t len)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdl = 0;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    EVP_DigestUpdate(c, data, len);
    EVP_DigestFinal_ex(c, md, &mdl);
    EVP_MD_CTX_free(c);
    for (unsigned i = 0; i < mdl && (size_t)(i * 2 + 1) < cap; i++)
        snprintf(out + i * 2, 3, "%02x", md[i]);
    out[mdl * 2] = '\0';
}

/* 按 ota_package_build_header + 载荷拼一个完整 .ota */
static void build_pkg(const char *version, const char *date,
                      const uint8_t *payload, size_t len, const char *out_path)
{
    char sha[65];
    make_sha(sha, sizeof(sha), payload, len);
    uint8_t hdr[OTA_PKG_HEADER_LEN];
    DG_CHECK(ota_package_build_header(hdr, version, date, (uint32_t)len, sha) ==
             DG_OK);
    FILE *f = fopen(out_path, "wb");
    DG_CHECK(f != NULL);
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(payload, 1, len, f);
    fclose(f);
}

/* 篡改包文件里偏移 header+idx 的一个载荷字节(制造摘要不符) */
static void corrupt_payload_byte(const char *path, size_t idx)
{
    FILE *f = fopen(path, "r+b");
    if (!f)
        exit(2);
    fseek(f, (long)(OTA_PKG_HEADER_LEN + idx), SEEK_SET);
    const uint8_t b = 0x5A;
    fwrite(&b, 1, 1, f);
    fclose(f);
}

int main(void)
{
    snprintf(s_dir, sizeof(s_dir), "/tmp/dg_pkg_%d", (int)getpid());
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", s_dir);
    DG_CHECK(system(cmd) >= 0);
    snprintf(s_pkg, sizeof(s_pkg), "%s/test.ota", s_dir);

    for (size_t i = 0; i < PAYLOAD_SZ; i++)
        s_payload[i] = (uint8_t)(i * 17 + 3);

    /* [P1] 头部往返:build → parse 字段一致 */
    printf("[P1] 头部往返\n");
    uint8_t hdr[OTA_PKG_HEADER_LEN];
    DG_CHECK(ota_package_build_header(hdr, "1.2.3", "2026-10-05", PAYLOAD_SZ,
                                      "00112233445566778899aabbccddeeff"
                                      "00112233445566778899aabbccddeeff") == DG_OK);
    ota_pkg_meta_t m;
    DG_CHECK(ota_package_parse_header(hdr, &m) == DG_OK);
    DG_CHECK(strcmp(m.version, "1.2.3") == 0);
    DG_CHECK(strcmp(m.date, "2026-10-05") == 0);
    DG_CHECK(m.payload_size == PAYLOAD_SZ);
    DG_CHECK(strcmp(m.sha256_hex,
                    "00112233445566778899aabbccddeeff"
                    "00112233445566778899aabbccddeeff") == 0);

    /* [P2] 头部非法:magic 坏 / 头长不符 / size=0 / sha 形态坏 */
    printf("[P2] 头部校验\n");
    hdr[0] = 'X';
    DG_CHECK(ota_package_parse_header(hdr, &m) == DG_ERR_PARAM);
    DG_CHECK(ota_package_build_header(hdr, "1.0", NULL, PAYLOAD_SZ,
                                      "zz112233445566778899aabbccddeeff"
                                      "00112233445566778899aabbccddeeff") ==
             DG_ERR_PARAM);
    DG_CHECK(ota_package_build_header(hdr, "1.0", NULL, PAYLOAD_SZ,
                                      "00112233445566778899aabbccddeeff"
                                      "00112233445566778899aabbccddeeff") == DG_OK);
    const uint32_t saved_hl = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                              ((uint32_t)hdr[10] << 8) | hdr[11];
    hdr[11] = 95;                             /* header_len 篡改 */
    DG_CHECK(ota_package_parse_header(hdr, &m) == DG_ERR_PARAM);
    hdr[11] = 96;
    DG_CHECK(ota_package_parse_header(hdr, &m) == DG_OK);
    (void)saved_hl;

    /* [P3] 完整包解析:摘要相符 → OK */
    printf("[P3] 完整包解析\n");
    build_pkg("9.8.7", "2026-10-05", s_payload, PAYLOAD_SZ, s_pkg);
    DG_CHECK(ota_package_parse_file(s_pkg, &m) == DG_OK);
    DG_CHECK(strcmp(m.version, "9.8.7") == 0);
    DG_CHECK(m.payload_size == PAYLOAD_SZ);

    /* [P4] 摘要不符:打包后篡改文件里的一个载荷字节 → MISMATCH */
    printf("[P4] 摘要不符拒收\n");
    build_pkg("9.8.7", "2026-10-05", s_payload, PAYLOAD_SZ, s_pkg);
    corrupt_payload_byte(s_pkg, 0);
    DG_CHECK(ota_package_parse_file(s_pkg, &m) == DG_ERR_MISMATCH);

    /* [P5] 截断包:总长不符 → PARAM */
    printf("[P5] 截断包拒收\n");
    build_pkg("9.8.7", "2026-10-05", s_payload, PAYLOAD_SZ, s_pkg);
    snprintf(cmd, sizeof(cmd), "truncate -s %d %s",
             (int)(OTA_PKG_HEADER_LEN + PAYLOAD_SZ - 100), s_pkg);
    DG_CHECK(system(cmd) >= 0);
    DG_CHECK(ota_package_parse_file(s_pkg, &m) == DG_ERR_PARAM);

    /* [P6] 非 .ota 文件:全 0 */
    printf("[P6] 非 .ota 拒收\n");
    snprintf(cmd, sizeof(cmd),
             "head -c 200 /dev/zero > %s && "
             "dd if=/dev/urandom of=%s bs=1 seek=200 count=100 2>/dev/null",
             s_pkg, s_pkg);
    DG_CHECK(system(cmd) >= 0);
    DG_CHECK(ota_package_parse_file(s_pkg, &m) == DG_ERR_PARAM);

    /* [P7] 不存在的文件 → PARAM(open_checked 统一按包无效) */
    printf("[P7] 缺文件\n");
    snprintf(s_pkg, sizeof(s_pkg), "%s/missing.ota", s_dir);
    DG_CHECK(ota_package_parse_file(s_pkg, &m) == DG_ERR_PARAM);

    /* [P8] extract:好包 → ota_staged.bin 字节一致 + sidecar 落位 */
    printf("[P8] 提取到暂存交接面\n");
    build_pkg("1.2.3", "2026-10-05", s_payload, PAYLOAD_SZ, s_pkg);
    char ver[OTA_PKG_VER_MAX] = "";
    DG_CHECK(ota_package_extract(s_pkg, s_dir, ver, sizeof(ver)) == DG_OK);
    DG_CHECK(strcmp(ver, "1.2.3") == 0);
    char path[128];
    snprintf(path, sizeof(path), "%s/ota_staged.bin", s_dir);
    FILE *f = fopen(path, "rb");
    DG_CHECK(f != NULL);
    uint8_t back[PAYLOAD_SZ];
    DG_CHECK(f && fread(back, 1, PAYLOAD_SZ, f) == PAYLOAD_SZ);
    if (f)
        fclose(f);
    DG_CHECK(memcmp(back, s_payload, PAYLOAD_SZ) == 0);
    snprintf(path, sizeof(path), "%s/ota_staged.bin.sha256", s_dir);
    DG_CHECK(access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/ota_staged.bin.ver", s_dir);
    DG_CHECK(access(path, F_OK) == 0);

    /* [P9] extract 坏包:失败且不留半成品(.part 已清,旧 staged 不动) */
    printf("[P9] 坏包提取不留半成品\n");
    corrupt_payload_byte(s_pkg, 1);
    DG_CHECK(ota_package_extract(s_pkg, s_dir, ver, sizeof(ver)) ==
             DG_ERR_MISMATCH);
    snprintf(path, sizeof(path), "%s/ota_staging.part", s_dir);
    DG_CHECK(access(path, F_OK) != 0);
    snprintf(path, sizeof(path), "%s/ota_staged.bin", s_dir);
    f = fopen(path, "rb");
    DG_CHECK(f != NULL);
    DG_CHECK(f && fread(back, 1, PAYLOAD_SZ, f) == PAYLOAD_SZ);
    if (f)
        fclose(f);
    DG_CHECK(back[1] == s_payload[1]);        /* 原 staged 未被破坏 */

    /* [P10] 版本字段超长截断(32B 边界) */
    printf("[P10] 超长版本截断\n");
    char longver[64];
    memset(longver, 'v', sizeof(longver) - 1);
    longver[sizeof(longver) - 1] = '\0';
    build_pkg(longver, "2026-10-05", s_payload, PAYLOAD_SZ, s_pkg);
    DG_CHECK(ota_package_parse_file(s_pkg, &m) == DG_OK);
    DG_CHECK(strlen(m.version) == OTA_PKG_VER_MAX - 1);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", s_dir);
    (void)system(cmd);
    DG_TEST_EXIT();
}
