#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 对拍 door-guard modules/sqlite/crypto.c 的 PBKDF2 实现
 * 用法: hb <hex-salt-32chars>  → 输出 123456 的 hash hex */
int dg_pbkdf2_sha256(const char *pwd, const unsigned char *salt, unsigned int salt_len,
                     unsigned int iters, unsigned char *out);

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    unsigned char salt[16];
    for (int i = 0; i < 16; i++)
        if (sscanf(argv[1] + i * 2, "%2hhx", &salt[i]) != 1) return 2;
    unsigned char out[32];
    if (dg_pbkdf2_sha256("123456", salt, 16, 10000, out) != 0) return 1;
    for (int i = 0; i < 32; i++) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
