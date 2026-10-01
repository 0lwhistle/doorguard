/*
 * fp_as608.c — AS608 协议层实现(纯函数)
 * 帧结构与码值:docs/tech/FINGERPRINT_PROTOCOL.md v1(官方例程冻结)。
 */
#include "fp_as608.h"

#include <stdio.h>
#include <string.h>

static const uint8_t FP_HDR[FP_A608_HDR_LEN] = {
    0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF
};

/* 校验和:标识码起(含类型、长度字节、载荷)逐字节和,取低 16 位 */
static uint16_t checksum_of(const uint8_t *type_and_rest, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint16_t)(sum + type_and_rest[i]);
    return sum;                          /* 自然截断 = 取低 16 位 */
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

size_t fp_as608_build_cmd(uint8_t *out, size_t cap, uint8_t cmd,
                          const uint8_t *params, size_t nparams)
{
    /* 帧长字段 = 指令 1B + 参数 + 校验和 2B */
    size_t len_field = 1 + nparams + 2;
    size_t total = FP_A608_HDR_LEN + 1 + 2 + len_field;
    if (!out || cap < total || nparams > FP_A608_PAYLOAD_MAX)
        return 0;

    memcpy(out, FP_HDR, FP_A608_HDR_LEN);
    out[FP_A608_HDR_LEN] = FP_A608_TYPE_CMD;
    put_be16(out + FP_A608_HDR_LEN + 1, (uint16_t)len_field);
    out[FP_A608_HDR_LEN + 3] = cmd;
    if (nparams)
        memcpy(out + FP_A608_HDR_LEN + 4, params, nparams);
    /* 校验和覆盖:类型 + 长度 2B + 指令 + 参数 */
    uint16_t sum = checksum_of(out + FP_A608_HDR_LEN, 3 + 1 + nparams);
    put_be16(out + total - 2, sum);
    return total;
}

/* 逐指令封装:统一走 build_cmd,单参数指令在此拼参数 */
size_t fp_as608_get_image(uint8_t *out, size_t cap)
{
    return fp_as608_build_cmd(out, cap, 0x01, NULL, 0);
}

size_t fp_as608_img2tz(uint8_t *out, size_t cap, uint8_t buf_id)
{
    return fp_as608_build_cmd(out, cap, 0x02, &buf_id, 1);
}

size_t fp_as608_match(uint8_t *out, size_t cap)
{
    return fp_as608_build_cmd(out, cap, 0x03, NULL, 0);
}

size_t fp_as608_search(uint8_t *out, size_t cap, uint8_t buf_id,
                       uint16_t start_page, uint16_t page_num)
{
    uint8_t p[5] = { buf_id, 0, 0, 0, 0 };
    put_be16(p + 1, start_page);
    put_be16(p + 3, page_num);
    return fp_as608_build_cmd(out, cap, 0x04, p, sizeof(p));
}

size_t fp_as608_reg_model(uint8_t *out, size_t cap)
{
    return fp_as608_build_cmd(out, cap, 0x05, NULL, 0);
}

size_t fp_as608_store(uint8_t *out, size_t cap, uint8_t buf_id, uint16_t page_id)
{
    uint8_t p[3] = { buf_id, 0, 0 };
    put_be16(p + 1, page_id);
    return fp_as608_build_cmd(out, cap, 0x06, p, sizeof(p));
}

size_t fp_as608_load_char(uint8_t *out, size_t cap, uint8_t buf_id, uint16_t page_id)
{
    uint8_t p[3] = { buf_id, 0, 0 };
    put_be16(p + 1, page_id);
    return fp_as608_build_cmd(out, cap, 0x07, p, sizeof(p));
}

size_t fp_as608_delet_char(uint8_t *out, size_t cap, uint16_t page_id, uint16_t count)
{
    uint8_t p[4] = { 0, 0, 0, 0 };
    put_be16(p, page_id);
    put_be16(p + 2, count);
    return fp_as608_build_cmd(out, cap, 0x0C, p, sizeof(p));
}

size_t fp_as608_empty(uint8_t *out, size_t cap)
{
    return fp_as608_build_cmd(out, cap, 0x0D, NULL, 0);
}

size_t fp_as608_valid_num(uint8_t *out, size_t cap)
{
    return fp_as608_build_cmd(out, cap, 0x1D, NULL, 0);
}

size_t fp_as608_verify_psw(uint8_t *out, size_t cap, uint32_t password)
{
    uint8_t p[4] = {
        (uint8_t)(password >> 24), (uint8_t)(password >> 16),
        (uint8_t)(password >> 8),  (uint8_t)(password & 0xFF),
    };
    return fp_as608_build_cmd(out, cap, 0x13, p, sizeof(p));
}

const char *fp_as608_confirm_name(uint16_t code)
{
    switch (code) {
    case FP_ACK_OK:          return "OK";
    case FP_ACK_RECV_ERR:    return "RECV_ERR";
    case FP_ACK_NO_FINGER:   return "NO_FINGER";
    case FP_ACK_ENROLL_FAIL: return "ENROLL_FAIL";
    case FP_ACK_DRY_IMAGE:   return "DRY_IMAGE";
    case FP_ACK_MERGE_FAIL:  return "MERGE_FAIL";
    case FP_ACK_NOT_FOUND:   return "NOT_FOUND";
    case FP_ACK_LIB_FULL:    return "LIB_FULL";
    case FP_ACK_FLASH_ERR:   return "FLASH_ERR";
    case FP_ACK_NO_BUF_IMG:  return "NO_BUF_IMG";
    default: break;
    }
    static char buf[10];
    snprintf(buf, sizeof(buf), "UNK_%02X", code & 0xFF);
    return buf;
}

/* ---- 解析状态机 ---- */
enum {
    PS_HEADER = 0,   /* 匹配 6B 包头 */
    PS_TYPE,         /* 标识码 1B */
    PS_LEN,          /* 长度 2B(大端) */
    PS_PAYLOAD,      /* 载荷(含 2B 校验和;按类型折算,见 after_len_of) */
};

/* 标识码后的实际字节数(含校验和)。AS608 应答包的长度字段口径与指令包
 * 不同:指令包 = 指令+参数+校验和;应答包 = 确认码+参数+校验和再减 1
 * (确认码 2B 只按 1B 计)——官方例程 FPM10A_Receive_Data(12/16) 少读
 * 一字节正是此坑。结束包 = 校验和 + 1(长度 0x0003、校验和 2B)。 */
static int after_len_of(uint8_t type, uint16_t len_field)
{
    switch (type) {
    case FP_A608_TYPE_ACK:  return (int)len_field + 1;
    case FP_A608_TYPE_END:  return (int)len_field - 1;
    default:                return (int)len_field;   /* 0x01 指令 / 0x02 数据 */
    }
}

void fp_as608_parser_init(fp_parser_t *p)
{
    if (p) {
        p->state = PS_HEADER;
        p->hdr_idx = 0;
        p->type = 0;
        p->len_raw = 0;
        p->need = 0;
        p->got = 0;
    }
}

int fp_as608_parse(fp_parser_t *p, const uint8_t *buf, size_t len,
                   fp_frame_t *out, size_t *consumed)
{
    if (!p || !buf || !out)
        return -1;
    if (consumed)
        *consumed = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (consumed)
            (*consumed)++;

        switch (p->state) {
        case PS_HEADER:
            if (b == FP_HDR[p->hdr_idx]) {
                p->hdr_idx++;
                if (p->hdr_idx == FP_A608_HDR_LEN) {
                    p->state = PS_TYPE;
                    p->hdr_idx = 0;
                }
            } else {
                /* 失配回退:如同 "EF 01 FF FF FF EF 01 ..." 里前缀复用,
                 * 从头再对(把当前字节作为新帧首字节重试一次) */
                p->hdr_idx = (b == FP_HDR[0]) ? 1 : 0;
            }
            break;

        case PS_TYPE:
            p->type = b;
            p->state = PS_LEN;
            p->hdr_idx = 0;
            break;

        case PS_LEN:
            p->hdr_idx++;
            if (p->hdr_idx == 1)
                p->len_raw = (uint16_t)(b << 8);
            else
                p->len_raw = (uint16_t)(p->len_raw | b);
            if (p->hdr_idx == 2) {
                int after = after_len_of(p->type, p->len_raw);
                if (after < 2 || after - 2 > FP_A608_PAYLOAD_MAX) {
                    fp_as608_parser_init(p);
                    return -1;
                }
                p->state = PS_PAYLOAD;
                p->need = (uint16_t)after;
                p->got = 0;
                p->hdr_idx = 0;
            }
            break;

        case PS_PAYLOAD:
            /* 全部载荷字节入缓冲(含末 2 字节校验和:expect 要从这里读)。
             * 此前只存前 need-2 字节,expect 读到的是未初始化栈——帧被误判
             * 协议错,真 bug 被 test_fp_proto 失效的假 main 掩盖(2026-10-01 修) */
            out->payload[p->got] = b;
            p->got++;
            if (p->got == p->need) {
                uint16_t expect = get_be16(&out->payload[p->need - 2]);
                /* 校验和覆盖:类型 + 原始长度字段 2B + 载荷(不含校验和)。
                 * ACK/END 的字节数折算只影响读取,不改校验和口径 */
                uint16_t actual = (uint16_t)(p->type + (p->len_raw >> 8) + (p->len_raw & 0xFF));
                for (uint16_t k = 0; k < (uint16_t)(p->need - 2); k++)
                    actual = (uint16_t)(actual + out->payload[k]);
                if (actual != expect) {
                    fp_as608_parser_init(p);
                    return -1;
                }
                out->type = p->type;
                out->payload_len = (uint16_t)(p->need - 2);
                fp_as608_parser_init(p);
                return 1;
            }
            break;

        default:
            fp_as608_parser_init(p);
            return -1;
        }
    }
    return 0;
}

uint16_t fp_as608_ack_confirm(const fp_frame_t *f)
{
    if (!f || f->type != FP_A608_TYPE_ACK || f->payload_len < 2)
        return 0xFFFF;                   /* 非应答帧,调用方误用 */
    return get_be16(f->payload);
}

void fp_as608_search_result(const fp_frame_t *f, uint16_t *page_id, uint16_t *score)
{
    if (page_id)
        *page_id = 0;
    if (score)
        *score = 0;
    if (!f || f->type != FP_A608_TYPE_ACK || f->payload_len < 6)
        return;
    if (page_id)
        *page_id = get_be16(f->payload + 2);
    if (score)
        *score = get_be16(f->payload + 4);
}
