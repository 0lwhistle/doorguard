/*
 * face_antispoof.c — 反欺骗纯算法件实现(见头文件:几何/通道/平滑)
 * 模型与判定语义的出处见 tools/convert_antispoof/(对拍脚本与文档)。
 */
#include "face_antispoof.h"

#include <string.h>

void antispoof_scale_box(int box_x, int box_y, int box_w, int box_h,
                         int src_w, int src_h, float scale,
                         int *out_x, int *out_y, int *out_w, int *out_h)
{
    /* 边界防御:退化框/空图直接给 1×1,让上层 RGA/推理显式失败而不是越界 */
    if (!out_x || !out_y || !out_w || !out_h)
        return;
    if (box_w <= 0 || box_h <= 0 || src_w <= 0 || src_h <= 0 || scale <= 0.0f) {
        *out_x = *out_y = *out_w = *out_h = 0;
        return;
    }
    /* 官方实现:三路取小 scale = min((src_h-1)/bh, (src_w-1)/bw, scale)
     * ——注意期望值必须与原始 scale 一同取小,不能分步覆盖 */
    float sc = scale;
    const float lim_h = (float)(src_h - 1) / (float)box_h;
    const float lim_w = (float)(src_w - 1) / (float)box_w;
    if (lim_w < sc)
        sc = lim_w;
    if (lim_h < sc)
        sc = lim_h;

    float new_w = (float)box_w * sc;
    float new_h = (float)box_h * sc;
    const float cx = (float)box_w / 2.0f + (float)box_x;
    const float cy = (float)box_h / 2.0f + (float)box_y;
    float ltx = cx - new_w / 2.0f;
    float lty = cy - new_h / 2.0f;
    float rbx = cx + new_w / 2.0f;
    float rby = cy + new_h / 2.0f;

    /* 整体平移夹回图内(官方语义:不平缩放) */
    if (ltx < 0.0f) {
        rbx -= ltx;
        ltx = 0.0f;
    }
    if (lty < 0.0f) {
        rby -= lty;
        lty = 0.0f;
    }
    if (rbx > (float)(src_w - 1)) {
        ltx -= rbx - (float)(src_w - 1);
        rbx = (float)(src_w - 1);
    }
    if (rby > (float)(src_h - 1)) {
        lty -= rby - (float)(src_h - 1);
        rby = (float)(src_h - 1);
    }

    int x = (int)ltx, y = (int)lty;
    int w = (int)rbx - x, h = (int)rby - y;
    if (w <= 0 || h <= 0) {                     /* 浮点边界:退化为 1×1 */
        w = h = 1;
    }
    /* 偶对齐收缩(YUV420 色度要求;官方无此步,是板上 NV12 路径的约束) */
    x &= ~1;
    y &= ~1;
    w &= ~1;
    h &= ~1;
    if (w <= 0) w = 2;
    if (h <= 0) h = 2;
    /* 收缩后不得越过右/下边界 */
    if (x + w > src_w) x = src_w - w;
    if (y + h > src_h) y = src_h - h;
    *out_x = x;
    *out_y = y;
    *out_w = w;
    *out_h = h;
}

void antispoof_rgb_to_bgr(const uint8_t *rgb, uint32_t n_pixels, uint8_t *bgr)
{
    if (!rgb || !bgr)
        return;
    for (uint32_t i = 0; i < n_pixels; i++) {
        bgr[i * 3 + 0] = rgb[i * 3 + 2];
        bgr[i * 3 + 1] = rgb[i * 3 + 1];
        bgr[i * 3 + 2] = rgb[i * 3 + 0];
    }
}

void antispoof_smooth_reset(antispoof_smooth_t *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
}

float antispoof_smooth_push(antispoof_smooth_t *s, float real_prob)
{
    if (!s)
        return real_prob;
    s->win[s->head] = real_prob;
    s->head = (s->head + 1) % ANTISPOOF_SMOOTH_N;
    if (s->count < ANTISPOOF_SMOOTH_N)
        s->count++;

    /* 插入排序取中位数:窗口 ≤5,常数即最优;win 原始顺序需保留,拷副本排 */
    float tmp[ANTISPOOF_SMOOTH_N];
    memcpy(tmp, s->win, sizeof(float) * (size_t)s->count);
    for (int i = 1; i < s->count; i++) {
        const float key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    return tmp[s->count / 2];
}

bool antispoof_is_spoof(float smooth_real, float threshold)
{
    return smooth_real < threshold;
}
