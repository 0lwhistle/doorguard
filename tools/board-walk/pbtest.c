/* 板上 PBKDF2 对拍:RFC 7914 §11 固定向量
 * (2026-11-21 审查修复:原第 2 段含真实用户盐/候选口令/库期望哈希,公开仓不可留,
 *  等效泄露板库凭据材料——真实值对拍移本地层执行) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int dg_pbkdf2_sha256(const char *pwd, const unsigned char *salt, unsigned int salt_len,
                     unsigned int iters, unsigned char out[32]);

static void phex(const unsigned char *p)
{
    for (int i = 0; i < 32; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    unsigned char out[32];
    (void)argc; (void)argv;

    /* RFC 7914 §11 固定向量 */
    if (dg_pbkdf2_sha256("password", (const unsigned char *)"salt", 4, 1, out) == 0)
        phex(out);
    printf("expect 120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b\n");
    return 0;
}
