/*
 * test_iccard_proto.c — IC 卡帧契约与卡号字符串化单测(ICCARD_PROTOCOL §3/§5)
 *
 * 覆盖:帧校验(magic 错/uid_len 越界)、HEX 转换(4B/15B/越界拒收)、
 * 展示掩码(末 4 恒有/短串全掩)。sim 后端 pipe 语义一并冒烟
 * (open→inject→poll→read→flush,真实 fd 路径零改动)。
 */
#include "dg_test.h"
#include "iccard_hal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static dg_iccard_frame_t mk_frame(const uint8_t *uid, uint8_t len)
{
    dg_iccard_frame_t f;
    memset(&f, 0, sizeof(f));
    f.magic = DG_ICCARD_MAGIC;
    f.uid_len = len;
    f.card_type = DG_ICCARD_TYPE_MF_CLASSIC;
    memcpy(f.uid, uid, len < DG_ICCARD_UID_MAX ? len : DG_ICCARD_UID_MAX);
    return f;
}

static void t_frame_valid(void)
{
    printf("[P1] 帧校验(magic/uid_len 契约)\n");
    uint8_t uid4[4] = { 0x04, 0xA3, 0xB2, 0xC1 };
    dg_iccard_frame_t f = mk_frame(uid4, 4);
    DG_CHECK(iccard_frame_valid(&f));

    f.magic = 0xDEADBEEF;
    DG_CHECK(!iccard_frame_valid(&f));        /* magic 错:整帧丢弃 */

    f = mk_frame(uid4, 4);
    f.uid_len = 3;                            /* 下界外 */
    DG_CHECK(!iccard_frame_valid(&f));
    f.uid_len = DG_ICCARD_UID_MAX + 1;        /* 上界外 */
    DG_CHECK(!iccard_frame_valid(&f));
    DG_CHECK(!iccard_frame_valid(NULL));
}

static void t_hex(void)
{
    printf("[P2] uid → HEX 字符串(按字节序大写,无分隔符)\n");
    char no[DG_IC_LEN];
    uint8_t uid4[4] = { 0x04, 0xA3, 0xB2, 0xC1 };
    DG_CHECK(iccard_uid_to_hex(uid4, 4, no) == DG_OK);
    DG_CHECK(strcmp(no, "04A3B2C1") == 0);

    uint8_t uid7[7] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
    DG_CHECK(iccard_uid_to_hex(uid7, 7, no) == DG_OK);
    DG_CHECK(strcmp(no, "01020304050607") == 0);

    uint8_t uid15[15];
    for (int i = 0; i < 15; i++)
        uid15[i] = (uint8_t)(0xAB ^ i);
    DG_CHECK(iccard_uid_to_hex(uid15, 15, no) == DG_OK);
    DG_CHECK(strnlen(no, sizeof(no)) == 30);  /* 15B = 30 字符,卡号串上限内 */

    uint8_t uid16[16] = { 0 };
    DG_CHECK(iccard_uid_to_hex(uid16, 16, no) == DG_ERR_PARAM);  /* 32 字符放不下 */

    uint8_t uid3[3] = { 1, 2, 3 };
    DG_CHECK(iccard_uid_to_hex(uid3, 3, no) == DG_ERR_PARAM);
    DG_CHECK(iccard_uid_to_hex(NULL, 4, no) == DG_ERR_PARAM);
}

static void t_mask(void)
{
    printf("[P3] 展示掩码(********+末4)\n");
    char out[16];
    iccard_mask("04A3B2C1", out, sizeof(out));
    DG_CHECK(strcmp(out, "********B2C1") == 0);
    iccard_mask("01020304050607", out, sizeof(out));
    DG_CHECK(strcmp(out, "********0607") == 0);
    iccard_mask("", out, sizeof(out));        /* 脏数据:全掩不出末4 */
    DG_CHECK(strcmp(out, "********") == 0);
    iccard_mask("ABC", out, sizeof(out));
    DG_CHECK(strcmp(out, "********") == 0);
}

static void t_sim_pipe(void)
{
    printf("[P4] sim 后端 pipe 语义(open/inject/poll/read/flush)\n");
    int fd = iccard_hal_open("sim");
    DG_CHECK(fd >= 0);

    DG_CHECK(iccard_hal_poll(fd, 20) == 0);   /* 无帧:超时 */

    uint8_t uid4[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t uid7[7] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x0F, 0x01 };
    DG_CHECK(iccard_sim_inject(uid4, 4) == 0);
    DG_CHECK(iccard_sim_inject(uid7, 7) == 0);  /* 两帧背靠背(环形缓冲场景) */

    DG_CHECK(iccard_hal_poll(fd, 200) == 1);
    dg_iccard_frame_t f;
    DG_CHECK(iccard_hal_read(fd, &f) == DG_OK);
    DG_CHECK(iccard_frame_valid(&f));
    DG_CHECK(memcmp(f.uid, uid4, 4) == 0);
    DG_CHECK(f.seq == 1);                     /* 驱动内 seq 自 1 递增 */

    DG_CHECK(iccard_hal_poll(fd, 200) == 1);
    DG_CHECK(iccard_hal_read(fd, &f) == DG_OK);
    DG_CHECK(memcmp(f.uid, uid7, 7) == 0);
    DG_CHECK(f.seq == 2);

    /* FLUSH:sim 无 ioctl 走排水等价路径 */
    DG_CHECK(iccard_sim_inject(uid4, 4) == 0);
    DG_CHECK(iccard_hal_flush(fd) == DG_OK);
    DG_CHECK(iccard_hal_poll(fd, 50) == 0);   /* 已清空 */

    iccard_hal_close(fd);
}

int main(void)
{
    t_frame_valid();
    t_hex();
    t_mask();
    t_sim_pipe();
    DG_TEST_EXIT();
}
