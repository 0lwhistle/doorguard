/*
 * wav.c — WAV 头解析实现:RIFF 容器按块遍历(fmt / data),不定块序
 * (某些导出器把 data 放 fmt 前),LIST 等杂块跳过。
 */
#include "wav.h"

#include <stdbool.h>
#include <string.h>

static bool tag_is(const uint8_t *p, const char *tag)
{
    return p && memcmp(p, tag, 4) == 0;
}

/* 小端 32/16 位读取(避免对齐/字节序假设) */
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int wav_probe(const uint8_t *buf, size_t len, wav_info_t *out)
{
    if (!buf || !out || len < 12)
        return DG_ERR_PARAM;
    if (!tag_is(buf, "RIFF") || !tag_is(buf + 8, "WAVE"))
        return DG_ERR_IO;

    bool have_fmt = false, have_data = false;
    wav_info_t info = { 0 };

    /* 块遍历起点 = 偏移 12(RIFF+WAVE 之后);每块头 8B:tag+size(小端) */
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *blk = buf + off;
        uint32_t sz = rd_u32(blk + 4);
        const size_t body = off + 8;
        if (body + sz > len)
            break;                          /* 头声明超过文件实长:按损坏 */
        if (tag_is(blk, "fmt ") && sz >= 16) {
            uint16_t fmt = rd_u16(buf + body);
            info.channels = rd_u16(buf + body + 2);
            info.rate = rd_u32(buf + body + 4);
            info.bits = rd_u16(buf + body + 14);
            if (fmt != 1 || info.bits != 16 ||
                (info.channels != 1 && info.channels != 2))
                return DG_ERR_UNSUPPORTED;
            have_fmt = true;
        } else if (tag_is(blk, "data")) {
            info.data_off = body;
            info.data_len = sz;
            have_data = true;
        }
        /* 奇数长度块按 RIFF 规范补偶对齐 */
        off = body + sz + (sz & 1);
    }

    if (!have_fmt || !have_data)
        return DG_ERR_IO;
    if (info.data_off + info.data_len > len)
        return DG_ERR_IO;                   /* data 块被截断 */
    if (info.data_len == 0 || info.data_len % (2 * info.channels) != 0)
        return DG_ERR_IO;                   /* 空数据/非整帧 */
    *out = info;
    return DG_OK;
}
