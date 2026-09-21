/*
 * test_jpeg.c — dg_jpeg 编解码测试(宿主)
 *
 * 覆盖:往返保真(q80 渐变图的误差上界)、缩放解码尺寸(1/2、1/4)、
 * 参数非法、容量不足(显式报错不截断)、损坏输入(报错不崩)。
 * 头像链路的可靠性就压在这里:DB 里那张图必须能原样解回来。
 */
#include "dg_test.h"
#include "modules/jpeg/dg_jpeg.h"
#include "proto/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 160
#define H 160

static void fill_gradient(uint8_t *rgb, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = &rgb[(y * (size_t)w + x) * 3];
            p[0] = (uint8_t)(x * 255 / (w - 1));
            p[1] = (uint8_t)(y * 255 / (h - 1));
            p[2] = (uint8_t)((x + y) & 0xFF);
        }
}

int main(void)
{
    static uint8_t src[W * H * 3];
    static uint8_t jpeg[DG_AVATAR_JPEG_MAX];
    static uint8_t out[W * H * 3];
    fill_gradient(src, W, H);

    /* ---- 编码:成功、头两个字节是 SOI、尺寸远小于上限 ---- */
    size_t jlen = 0;
    DG_CHECK(dg_jpeg_encode_rgb(src, W, H, 80, jpeg, sizeof(jpeg), &jlen) == DG_OK);
    DG_CHECK(jlen > 100 && jlen < sizeof(jpeg));
    DG_CHECK(jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    printf("  160x160 q80 -> %zu B\n", jlen);

    /* ---- 原尺寸解码:渐变图 q80 平均逐通道误差应有界 ---- */
    int w = 0, h = 0;
    DG_CHECK(dg_jpeg_decode_rgb(jpeg, jlen, 1, out, sizeof(out), &w, &h) == DG_OK);
    DG_CHECK(w == W && h == H);
    long sum = 0;
    for (size_t i = 0; i < (size_t)W * H * 3; i++) {
        long d = (long)out[i] - src[i];
        sum += d < 0 ? -d : d;
    }
    const double mad = (double)sum / ((double)W * H * 3);
    printf("  roundtrip mean abs diff = %.2f\n", mad);
    DG_CHECK(mad < 6.0);

    /* ---- 缩放解码:160/2=80、160/4=40(列表缩略图路径) ---- */
    DG_CHECK(dg_jpeg_decode_rgb(jpeg, jlen, 2, out, sizeof(out), &w, &h) == DG_OK);
    DG_CHECK(w == 80 && h == 80);
    DG_CHECK(dg_jpeg_decode_rgb(jpeg, jlen, 4, out, sizeof(out), &w, &h) == DG_OK);
    DG_CHECK(w == 40 && h == 40);

    /* ---- 参数与容量错误路径 ---- */
    DG_CHECK(dg_jpeg_encode_rgb(NULL, W, H, 80, jpeg, sizeof(jpeg), &jlen)
             == DG_ERR_PARAM);
    DG_CHECK(dg_jpeg_encode_rgb(src, W, H, 0, jpeg, sizeof(jpeg), &jlen)
             == DG_ERR_PARAM);                        /* quality 越界 */
    size_t tiny_len = 0;
    DG_CHECK(dg_jpeg_encode_rgb(src, W, H, 80, jpeg, 64, &tiny_len)
             == DG_ERR_NO_MEMORY);                    /* 容量不足不截断交付 */
    DG_CHECK(dg_jpeg_decode_rgb(jpeg, jlen, 3, out, sizeof(out), &w, &h)
             == DG_ERR_PARAM);                        /* 非法缩放倍率 */
    uint8_t small[40 * 40 * 3];
    DG_CHECK(dg_jpeg_decode_rgb(jpeg, jlen, 1, small, sizeof(small), &w, &h)
             == DG_ERR_NO_MEMORY);                    /* 容量不足 */

    /* ---- 损坏输入:截断的 JPEG 报错返回,进程不崩 ---- */
    DG_CHECK(dg_jpeg_decode_rgb(jpeg, jlen / 3, 1, out, sizeof(out), &w, &h)
             != DG_OK);
    const uint8_t garbage[16] = { 0 };
    DG_CHECK(dg_jpeg_decode_rgb(garbage, sizeof(garbage), 1, out, sizeof(out),
                                &w, &h) != DG_OK);

    DG_TEST_EXIT();
}
