/* 板上 PBKDF2 对拍:1=RFC 固定向量,2=真实 salt 算 1230 */
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

    /* 001 的真实 salt → 1230 的 hash(库期望 9de62d00...816c9) */
    unsigned char salt[16];
    const char *hex = "d7b0680aaab3b4856091d2e3b4b2a04f";
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%2x", &v);
        salt[i] = (unsigned char)v;
    }
    if (dg_pbkdf2_sha256("1230", salt, 16, 10000, out) == 0)
        phex(out);
    printf("expect 9de62d005fa588fe10bac289d2138f29ee38d3391035b3a80d7545953b7816c9\n");
    return 0;
}
