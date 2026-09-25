/* useradd.c — 用应用同款 storage 路径重建走查用户(禁止手工 INSERT)
 * 用法: useradd <db> <uid> <name> <pwd> <role> <auth_flags>
 * 编译见排查手册 §7.3;密码经 db_user_set_password(PBKDF2+随机盐) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "storage.h"
#include "types.h"

int main(int argc, char **argv)
{
    if (argc < 6) {
        printf("usage: %s <db> <uid> <name> <pwd> <role> <auth_flags>\n", argv[0]);
        return 2;
    }
    if (storage_init(argv[1], "/tmp/dg_uadd.key") != DG_OK) {
        printf("storage_init FAIL\n");
        return 1;
    }
    user_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.user_id, sizeof(rec.user_id), "%s", argv[2]);
    snprintf(rec.user_name, sizeof(rec.user_name), "%s", argv[3]);
    rec.id = 0;                          /* 新增:主键忽略 */
    rec.ic_card[0] = '\0';
    rec.role = atoi(argv[5]);
    rec.auth_flags = (unsigned)atoi(argv[6]);
    rec.created_at = rec.updated_at = time(NULL);

    int rc = db_user_set_password(&rec, argv[4]);
    printf("db_user_set_password rc=%d\n", rc);
    if (rc == DG_OK)
        rc = db_user_add(&rec);
    printf("db_user_add(%s) rc=%d\n", argv[2], rc);
    storage_deinit();
    return rc == DG_OK ? 0 : 1;
}
