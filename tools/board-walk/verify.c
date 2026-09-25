/* 宿主对拍:用应用同款 storage/crypto 代码验证拷回的板上库
 * 用法: verify <db> <uid> <pwd>  → 打印 db_user_get 结果与 db_verify_password 结果 */
#include <stdio.h>
#include <string.h>

#include "storage.h"
#include "crypto.h"
#include "types.h"

static void hexdump(const char *tag, const unsigned char *p, unsigned n)
{
    printf("%s ", tag);
    for (unsigned i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    if (storage_init(argv[1], "/tmp/dg_verify.key") != DG_OK) {
        printf("storage_init FAIL\n");
        return 1;
    }
    user_rec_t rec;
    int rc = db_user_get(argv[2], &rec);
    printf("db_user_get(%s) rc=%d\n", argv[2], rc);
    if (rc == DG_OK) {
        printf("user_name=%s role=%d auth_flags=%u\n", rec.user_name, rec.role, rec.auth_flags);
        hexdump("salt:", rec.pwd_salt, DG_PWD_SALT_LEN);
        hexdump("hash:", rec.pwd_hash, DG_PWD_HASH_LEN);
    }
    rc = db_verify_password(argv[2], argv[3], &rec);
    printf("db_verify_password(%s,%s) rc=%d (0=OK)\n", argv[2], argv[3], rc);
    storage_deinit();
    return 0;
}
