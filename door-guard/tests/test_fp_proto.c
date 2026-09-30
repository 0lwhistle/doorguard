/*
 * test_fp_proto.c — AS608 协议层单测(services/verify/fingerprint/fp_as608)
 *
 * golden vector 直接取自官方 51 例程 FPM10A.c 的逐字节指令常量
 * (docs/tech/FINGERPRINT_PROTOCOL.md §2 权威出处):改协议码值必须先过这里。
 * 解析侧覆盖:整帧/跨块/粘包/校验和错/噪声字节/多帧连发重喂。
 */
#include "dg_test.h"
#include "fp_as608.h"

#include <stdio.h>
#include <string.h>

/* 官方例程原文 golden:包头 + FPM10A_Get_Img 等(帧内注释处与 FPM10A.c 对齐) */
static const uint8_t GOLD_GET_IMAGE[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x01, 0x00, 0x05 };
static const uint8_t GOLD_IMG2TZ_B1[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x02, 0x01, 0x00, 0x08 };
static const uint8_t GOLD_IMG2TZ_B2[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x02, 0x02, 0x00, 0x09 };
static const uint8_t GOLD_SEARCH_ALL[] = {          /* FPM10A_Search:0~999,B1 */
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x08,
    0x04, 0x01, 0x00, 0x00, 0x03, 0xE7, 0x00, 0xF8 };
static const uint8_t GOLD_REG_MODEL[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x05, 0x00, 0x09 };
static const uint8_t GOLD_EMPTY[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x0D, 0x00, 0x11 };
static const uint8_t GOLD_VALID_NUM[] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x1D, 0x00, 0x21 };
static const uint8_t GOLD_VERIFY_PSW[] = {          /* FPM10A_Get_Device 出厂口令 */
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x07,
    0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B };
static const uint8_t GOLD_STORE_P11[] = {           /* FPM10A_Cmd_Save_Finger(11) */
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x06,
    0x06, 0x01, 0x00, 0x0B, 0x00, 0x19 };

static void expect_bytes(const char *what, const uint8_t *got, size_t got_len,
                         const uint8_t *want, size_t want_len)
{
    printf("[%s] len=%zu(got %zu)\n", what, want_len, got_len);
    DG_CHECK(got_len == want_len);
    DG_CHECK(memcmp(got, want, want_len) == 0);
}

static void t_build_golden(void)
{
    printf("[P1] 组包 vs 官方例程 golden(改码值必过)\n");
    uint8_t b[FP_A608_FRAME_MAX];

    expect_bytes("GetImage", b, fp_as608_get_image(b, sizeof(b)),
                 GOLD_GET_IMAGE, sizeof(GOLD_GET_IMAGE));
    expect_bytes("Img2Tz_B1", b, fp_as608_img2tz(b, sizeof(b), FP_A608_BUF1),
                 GOLD_IMG2TZ_B1, sizeof(GOLD_IMG2TZ_B1));
    expect_bytes("Img2Tz_B2", b, fp_as608_img2tz(b, sizeof(b), FP_A608_BUF2),
                 GOLD_IMG2TZ_B2, sizeof(GOLD_IMG2TZ_B2));
    expect_bytes("Search全库", b, fp_as608_search(b, sizeof(b), FP_A608_BUF1, 0, 999),
                 GOLD_SEARCH_ALL, sizeof(GOLD_SEARCH_ALL));
    expect_bytes("RegModel", b, fp_as608_reg_model(b, sizeof(b)),
                 GOLD_REG_MODEL, sizeof(GOLD_REG_MODEL));
    expect_bytes("Empty", b, fp_as608_empty(b, sizeof(b)),
                 GOLD_EMPTY, sizeof(GOLD_EMPTY));
    expect_bytes("ValidNum", b, fp_as608_valid_num(b, sizeof(b)),
                 GOLD_VALID_NUM, sizeof(GOLD_VALID_NUM));
    expect_bytes("VerifyPSW", b, fp_as608_verify_psw(b, sizeof(b), 0),
                 GOLD_VERIFY_PSW, sizeof(GOLD_VERIFY_PSW));
    expect_bytes("Store@11", b, fp_as608_store(b, sizeof(b), FP_A608_BUF1, 11),
                 GOLD_STORE_P11, sizeof(GOLD_STORE_P11));
}

static void t_build_derived(void)
{
    printf("[P2] 标准推导指令:帧式自洽(校验和可复算)\n");
    uint8_t b[FP_A608_FRAME_MAX];
    size_t n;

    /* Match:官方例程无此 golden,按帧式断言结构 + 校验和 */
    n = fp_as608_match(b, sizeof(b));
    DG_CHECK(n == 12);
    DG_CHECK(b[9] == 0x03);
    DG_CHECK(((b[10] << 8) | b[11]) == (0x01 + 0x03 + 0x03));   /* =0x07 */

    /* LoadChar B2@5:EF01+01+0006+07+02+0005+sum(01+06+07+02+05=0x15) */
    n = fp_as608_load_char(b, sizeof(b), FP_A608_BUF2, 5);
    DG_CHECK(n == 15);
    DG_CHECK(b[9] == 0x07 && b[10] == FP_A608_BUF2);
    DG_CHECK(b[11] == 0x00 && b[12] == 5);
    DG_CHECK(((b[13] << 8) | b[14]) == 0x15);

    /* DeletChar @7 起 1 枚:len=0007,sum=01+00+07+0C+00+07+00+01=0x17 */
    n = fp_as608_delet_char(b, sizeof(b), 7, 1);
    DG_CHECK(n == 16);
    DG_CHECK(b[9] == 0x0C && b[10] == 0x00 && b[11] == 7 && b[12] == 0x00 && b[13] == 1);
    DG_CHECK(((b[14] << 8) | b[15]) == 0x17);

    /* 容量不足返回 0,不越界 */
    uint8_t tiny[8];
    DG_CHECK(fp_as608_get_image(tiny, sizeof(tiny)) == 0);
    DG_CHECK(fp_as608_search(tiny, sizeof(tiny), 1, 0, 999) == 0);
}

static void t_parse_ack(void)
{
    printf("[P3] 应答解析:整帧/跨块/确认码/Search 取参\n");
    fp_parser_t p;
    fp_frame_t f;

    /* Search 命中 17B:07 0007 | 确认 0000 | 页 0005 | 得分 0032 | sum 0046
     * (sum=07+00+07+00+00+00+05+00+32=0x46;注意应答长度字段口径:确认码
     * 2B 按 1B 计,实际帧比长度字段多 1 字节,见 fp_as608.c after_len_of) */
    uint8_t srch_ack[] = { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                           0x07, 0x00, 0x07, 0x00, 0x00, 0x00, 0x05,
                           0x00, 0x32, 0x00, 0x46 };
    fp_as608_parser_init(&p);
    DG_CHECK(fp_as608_parse(&p, srch_ack, sizeof(srch_ack), &f, NULL) == 1);
    DG_CHECK(f.type == FP_A608_TYPE_ACK);
    DG_CHECK(fp_as608_ack_confirm(&f) == FP_ACK_OK);
    uint16_t page = 0, score = 0;
    fp_as608_search_result(&f, &page, &score);
    DG_CHECK(page == 5 && score == 50);

    /* 跨块:逐字节喂入也能拼出同一帧 */
    fp_as608_parser_init(&p);
    int got = 0;
    for (size_t i = 0; i < sizeof(srch_ack); i++) {
        int r = fp_as608_parse(&p, &srch_ack[i], 1, &f, NULL);
        if (r == 1) {
            got = 1;
            break;
        }
        DG_CHECK(r == 0);
    }
    DG_CHECK(got);
    DG_CHECK(fp_as608_ack_confirm(&f) == FP_ACK_OK && page == 5);

    /* 普通指令应答 13B:确认码 0x02(无手指),sum=07+00+03+00+02=0x0C */
    uint8_t nack[] = { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                       0x07, 0x00, 0x03, 0x00, 0x02, 0x00, 0x0C };
    fp_as608_parser_init(&p);
    DG_CHECK(fp_as608_parse(&p, nack, sizeof(nack), &f, NULL) == 1);
    DG_CHECK(fp_as608_ack_confirm(&f) == FP_ACK_NO_FINGER);
    DG_CHECK(strcmp(fp_as608_confirm_name(FP_ACK_NO_FINGER), "NO_FINGER") == 0);
}

static void t_parse_errors(void)
{
    printf("[P4] 解析防御:噪声前缀/校验和错/长度超限\n");
    fp_parser_t p;
    fp_frame_t f;

    /* 前置噪声字节 + 正帧(粘包场景的"前噪声"形态) */
    uint8_t noisy[] = { 0xAA, 0xBB, 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                        0x07, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0A };
    fp_as608_parser_init(&p);
    DG_CHECK(fp_as608_parse(&p, noisy, sizeof(noisy), &f, NULL) == 1);
    DG_CHECK(fp_as608_ack_confirm(&f) == FP_ACK_OK);

    /* 校验和错(尾字节改坏)→ -1 且解析器自动复位,随后可继续收帧 */
    uint8_t bad[sizeof(noisy)];
    memcpy(bad, noisy, sizeof(noisy));
    bad[sizeof(bad) - 1] ^= 0xFF;
    fp_as608_parser_init(&p);
    DG_CHECK(fp_as608_parse(&p, bad, sizeof(bad), &f, NULL) == -1);
    DG_CHECK(fp_as608_parse(&p, noisy, sizeof(noisy), &f, NULL) == 1);

    /* 长度字段超限(声称 0xFFFF)→ -1 */
    uint8_t biglen[] = { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                         0x07, 0xFF, 0xFF, 0x00, 0x00 };
    fp_as608_parser_init(&p);
    DG_CHECK(fp_as608_parse(&p, biglen, sizeof(biglen), &f, NULL) == -1);
}

static void t_parse_sticky(void)
{
    printf("[P5] 粘包/多帧连发:应答+数据包+结束包,consumed 续喂\n");
    fp_parser_t p;
    fp_frame_t f;

    /* UpChar 三连:指令应答(0x00) + 数据包 4B 特征(0x02) + 结束包(0x08)
     * 数据包:len=数据4+校验和2=0006,sum=02+00+06+DE+AD+BE+EF=0x0340
     * 结束包:len=0003(校验和前口径),sum=08+00+03=0x000B */
    uint8_t stream[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0A,
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x06,
        0xDE, 0xAD, 0xBE, 0xEF, 0x03, 0x40,
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x08, 0x00, 0x03, 0x00, 0x0B,
    };
    fp_as608_parser_init(&p);
    size_t off = 0, consumed = 0;
    int frames = 0;
    uint8_t seen_type[3] = { 0, 0, 0 };
    while (off < sizeof(stream)) {
        int r = fp_as608_parse(&p, stream + off, sizeof(stream) - off, &f, &consumed);
        if (r == 1) {
            seen_type[frames++] = f.type;
            off += consumed;              /* 剩余字节必须续喂(API 契约) */
            if (frames == 3)
                break;
        } else {
            /* 0=输入耗尽(中途断帧) / -1=协议错:都直接终局,防死循环 */
            DG_CHECK(r == 0 && frames == 3);
            break;
        }
    }
    DG_CHECK(frames == 3);
    DG_CHECK(seen_type[0] == FP_A608_TYPE_ACK);
    DG_CHECK(seen_type[1] == FP_A608_TYPE_DATA);
    DG_CHECK(seen_type[2] == FP_A608_TYPE_END);
}

int main(void)
{
    t_build_golden();
    t_build_derived();
    t_parse_ack();
    t_parse_errors();
    t_parse_sticky();
    printf("test_fp_proto: ALL PASS\n");
    return 0;
}
