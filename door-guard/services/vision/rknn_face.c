/*
 * rknn_face.c — 自组 rknn 人脸后处理实现(SCRFD 解码 / NMS / 5 点对齐 / 比对)
 *
 * 纯 C、零依赖(只用 libm):这些逻辑的正确性可以直接在宿主上验证,
 * 不必等上板——上板要验的只是"硬件路径接得对不对",不是"数学对不对"。
 * 这个分界是本文件存在的理由:把最容易出 subtle bug 的部分留在能测的地方。
 */
#include "rknn_face.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---- SCRFD 解码 ----------------------------------------------------------
 * anchor 编号 = (y * grid_w + x) * n_anchors + a:位置行优先,同一位置内
 * anchor 连续。这与 rknn_model_zoo / insightface 的导出布局一致;
 * 若换模型发现框整体错位,先怀疑这里(用单点高分的合成输入即可定位)。
 */
int rknn_scrfd_decode(const rknn_scrfd_level_t *lv, int n_lv, float score_thresh,
                      rknn_face_t *out, int max_out)
{
    if (!lv || n_lv <= 0 || !out || max_out <= 0)
        return -1;

    int cnt = 0;
    for (int L = 0; L < n_lv; L++) {
        const rknn_scrfd_level_t *l = &lv[L];
        if (!l->score || !l->bbox || !l->kps)
            continue;
        if (l->grid_w <= 0 || l->grid_h <= 0 || l->n_anchors <= 0 || l->stride <= 0)
            continue;

        const float st = (float)l->stride;
        for (int y = 0; y < l->grid_h; y++) {
            for (int x = 0; x < l->grid_w; x++) {
                /* anchor 中心 = 网格坐标 × stride(无 0.5 偏移:SCRFD 用整数格点) */
                const float cx = (float)(x * l->stride);
                const float cy = (float)(y * l->stride);
                for (int a = 0; a < l->n_anchors; a++) {
                    const int idx = (y * l->grid_w + x) * l->n_anchors + a;
                    const float s = l->score[idx];
                    if (!(s >= score_thresh))   /* 同时挡掉 NaN */
                        continue;

                    const float *b = l->bbox + (size_t)idx * 4;
                    rknn_face_t f;
                    f.x1 = cx - b[0] * st;
                    f.y1 = cy - b[1] * st;
                    f.x2 = cx + b[2] * st;
                    f.y2 = cy + b[3] * st;
                    f.score = s;

                    const float *k = l->kps + (size_t)idx * 10;
                    for (int j = 0; j < RKNN_FACE_KPS; j++) {
                        f.kps[j][0] = cx + k[j * 2] * st;
                        f.kps[j][1] = cy + k[j * 2 + 1] * st;
                    }

                    if (cnt < max_out)
                        out[cnt] = f;
                    cnt++;      /* 超容量的也计数:调用方据此知道被截断 */
                }
            }
        }
    }
    return cnt;
}

/* ---- RetinaFace 解码 -----------------------------------------------------
 * 与 rknn_model_zoo 的 Python 参考实现逐条对齐(zoo 那边的锚框是运行时按
 * image_size 现算的,这里同样现算,不落 4200×4 的大表):
 *
 *   min_sizes = [[16,32],[64,128],[256,512]] , steps = [8,16,32]
 *   feature_maps = ceil(size/step)           → 320 时 40/20/10
 *   遍历顺序:i 为行(y)、j 为列(x),同一位置内两个 min_size 连续
 *   锚框 = [cx, cy, w, h] 全部归一化到 [0,1],中心带 +0.5 格偏移
 *
 *   框解码(variance=[0.1,0.2]):
 *     cx = pcx + loc[0]*0.1*pw   cy = pcy + loc[1]*0.1*ph
 *     w  = pw * exp(loc[2]*0.2)  h  = ph * exp(loc[3]*0.2)
 *     x1 = cx - w/2 …(再 ×size 回到像素)
 *   关键点解码:同式但只乘 0.1、不取 exp(每个点 x,y 分开算)
 *
 * 与 SCRFD 的差别(别混用):SCRFD 是 anchor-free 的距离回归、中心无 0.5 偏移、
 * 距离以 stride 为单位;RetinaFace 是 anchor-based、中心有 0.5 偏移、以归一化宽高为单位。
 */
static const int k_rf_min_sizes[3][2] = { { 16, 32 }, { 64, 128 }, { 256, 512 } };
static const int k_rf_steps[3] = { 8, 16, 32 };

int rknn_retinaface_anchor_count(int size)
{
    if (size <= 0)
        return 0;
    int total = 0;
    for (int k = 0; k < 3; k++) {
        const int f = (size + k_rf_steps[k] - 1) / k_rf_steps[k];   /* ceil */
        total += f * f * 2;                                        /* ×2 个 min_size */
    }
    return total;
}

int rknn_retinaface_decode(const float *loc, const float *conf, const float *landm,
                           int n, int size, float score_thresh,
                           rknn_face_t *out, int max_out)
{
    if (!loc || !conf || !landm || !out || max_out <= 0 || size <= 0)
        return -1;
    /* 锚框数对不上说明解码方案与模型不是一套:响亮报错,不给错框 */
    if (n != rknn_retinaface_anchor_count(size))
        return -2;

    const float fs = (float)size;
    int idx = 0, cnt = 0;

    for (int k = 0; k < 3; k++) {
        const int step = k_rf_steps[k];
        const int f = (size + step - 1) / step;
        for (int i = 0; i < f; i++) {              /* 行 → y */
            for (int j = 0; j < f; j++) {          /* 列 → x */
                for (int a = 0; a < 2; a++) {
                    const int ai = idx++;
                    const float s = conf[ai * 2 + 1];      /* [.,1] = 人脸类 */
                    if (!(s >= score_thresh))              /* 同时挡 NaN */
                        continue;

                    const float pw  = (float)k_rf_min_sizes[k][a] / fs;
                    const float ph  = pw;                  /* 方形锚框 */
                    const float pcx = ((float)j + 0.5f) * (float)step / fs;
                    const float pcy = ((float)i + 0.5f) * (float)step / fs;

                    const float *L = loc + (size_t)ai * 4;
                    const float cx = pcx + L[0] * 0.1f * pw;
                    const float cy = pcy + L[1] * 0.1f * ph;
                    const float bw = pw * expf(L[2] * 0.2f);
                    const float bh = ph * expf(L[3] * 0.2f);

                    rknn_face_t fo;
                    fo.x1 = (cx - bw * 0.5f) * fs;
                    fo.y1 = (cy - bh * 0.5f) * fs;
                    fo.x2 = (cx + bw * 0.5f) * fs;
                    fo.y2 = (cy + bh * 0.5f) * fs;
                    fo.score = s;

                    const float *lm = landm + (size_t)ai * 10;
                    for (int p = 0; p < RKNN_FACE_KPS; p++) {
                        fo.kps[p][0] = (pcx + lm[p * 2]     * 0.1f * pw) * fs;
                        fo.kps[p][1] = (pcy + lm[p * 2 + 1] * 0.1f * ph) * fs;
                    }

                    if (cnt < max_out)
                        out[cnt] = fo;
                    cnt++;
                }
            }
        }
    }
    return cnt;
}

/* ---- NMS ---------------------------------------------------------------- */

float rknn_iou(const rknn_face_t *a, const rknn_face_t *b)
{
    const float ix1 = a->x1 > b->x1 ? a->x1 : b->x1;
    const float iy1 = a->y1 > b->y1 ? a->y1 : b->y1;
    const float ix2 = a->x2 < b->x2 ? a->x2 : b->x2;
    const float iy2 = a->y2 < b->y2 ? a->y2 : b->y2;
    const float iw = ix2 - ix1, ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f)
        return 0.0f;
    const float inter = iw * ih;
    const float ua = (a->x2 - a->x1) * (a->y2 - a->y1) +
                     (b->x2 - b->x1) * (b->y2 - b->y1) - inter;
    return ua > 0.0f ? inter / ua : 0.0f;
}

static int cmp_score_desc(const void *pa, const void *pb)
{
    const rknn_face_t *a = (const rknn_face_t *)pa;
    const rknn_face_t *b = (const rknn_face_t *)pb;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    return 0;
}

int rknn_nms(rknn_face_t *faces, int n, float iou_thresh)
{
    if (!faces || n <= 0)
        return 0;
    if (n == 1)
        return 1;

    qsort(faces, (size_t)n, sizeof(*faces), cmp_score_desc);

    /* 抑制标记用 score<0(空出来一位,不必另开数组) */
    for (int i = 0; i < n; i++) {
        if (faces[i].score < 0.0f)
            continue;
        for (int j = i + 1; j < n; j++) {
            if (faces[j].score < 0.0f)
                continue;
            if (rknn_iou(&faces[i], &faces[j]) > iou_thresh)
                faces[j].score = -1.0f;
        }
    }

    int keep = 0;
    for (int i = 0; i < n; i++)
        if (faces[i].score >= 0.0f)
            faces[keep++] = faces[i];
    return keep;
}

/* ---- 5 点对齐 ------------------------------------------------------------
 * ArcFace(InsightFace)的 112×112 参考五点。顺序与 SCRFD 输出一致:
 * 左眼、右眼、鼻尖、左嘴角、右嘴角。
 */
static const float k_arcface_src[RKNN_FACE_KPS][2] = {
    { 38.2946f, 51.6963f },
    { 73.5318f, 51.5014f },
    { 56.0252f, 71.7366f },
    { 41.5493f, 92.3655f },
    { 70.7299f, 92.2041f },
};

/* 解 4×4 线性方程组(列主元高斯消元)。规模固定且极小,不引第三方库 */
static int solve4(float A[4][4], float b[4], float x[4])
{
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (fabsf(A[r][c]) > fabsf(A[piv][c]))
                piv = r;
        if (fabsf(A[piv][c]) < 1e-9f)
            return -1;                      /* 退化(点全重合) */
        if (piv != c) {
            for (int k = 0; k < 4; k++) {
                const float t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t;
            }
            const float t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        for (int r = c + 1; r < 4; r++) {
            const float f = A[r][c] / A[c][c];
            if (f == 0.0f)
                continue;
            for (int k = c; k < 4; k++)
                A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 3; r >= 0; r--) {
        float s = b[r];
        for (int k = r + 1; k < 4; k++)
            s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return 0;
}

int rknn_align_plan(const float src_kps[RKNN_FACE_KPS][2], float m[6])
{
    if (!src_kps || !m)
        return -1;

    /* 相似变换 4 参数 (a,b,tx,ty):
     *   X = a*x - b*y + tx ,  Y = b*x + a*y + ty
     * 最小二乘正规方程(对 5 点求和,推导见实现注释上文):
     *   [ Σ(x²+y²)      0      Σx    Σy ] [a ]   [ Σ(xX+yY) ]
     *   [     0     Σ(x²+y²)  -Σy    Σx ] [b ] = [ Σ(xY-yX) ]
     *   [    Σx       -Σy       N     0 ] [tx]   [    ΣX    ]
     *   [    Σy        Σx       0     N ] [ty]   [    ΣY    ]
     */
    float Sxx_yy = 0, Sx = 0, Sy = 0, SxX = 0, SyY = 0, SxY = 0, SyX = 0, SX = 0, SY = 0;
    const float N = (float)RKNN_FACE_KPS;
    for (int i = 0; i < RKNN_FACE_KPS; i++) {
        const float x = src_kps[i][0], y = src_kps[i][1];
        const float X = k_arcface_src[i][0], Y = k_arcface_src[i][1];
        Sxx_yy += x * x + y * y;
        Sx += x;  Sy += y;
        SxX += x * X;  SyY += y * Y;
        SxY += x * Y;  SyX += y * X;
        SX += X;  SY += Y;
    }

    float A[4][4] = {
        { Sxx_yy,     0.0f,  Sx,  Sy },
        {    0.0f, Sxx_yy,  -Sy,  Sx },
        {      Sx,    -Sy,    N, 0.0f },
        {      Sy,     Sx, 0.0f,   N },
    };
    const float b[4] = { SxX + SyY, SxY - SyX, SX, SY };
    float x[4];
    if (solve4(A, (float *)b, x) != 0)
        return -1;

    m[0] = x[0];        /* a  */
    m[1] = -x[1];       /* -b */
    m[2] = x[2];        /* tx */
    m[3] = x[1];        /* b  */
    m[4] = x[0];        /* a  */
    m[5] = x[3];        /* ty */
    return 0;
}

void rknn_align_warp(const uint8_t *src, int sw, int sh, const float m[6],
                     uint8_t *dst, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    const float det = m[0] * m[4] - m[1] * m[3];
    if (fabsf(det) < 1e-12f) {
        memset(dst, 0, (size_t)dw * dh * 3);
        return;
    }
    const float inv = 1.0f / det;

    for (int v = 0; v < dh; v++) {
        for (int u = 0; u < dw; u++) {
            /* 逆映射:目标 (u,v) → 源坐标,再双线性采样 */
            const float dx = (float)u - m[2];
            const float dy = (float)v - m[5];
            const float sx = ( m[4] * dx - m[1] * dy) * inv;
            const float sy = (-m[3] * dx + m[0] * dy) * inv;

            uint8_t *o = dst + ((size_t)v * dw + u) * 3;
            if (sx < 0.0f || sy < 0.0f || sx > (float)(sw - 1) || sy > (float)(sh - 1)) {
                o[0] = o[1] = o[2] = 0;
                continue;
            }
            const int x0 = (int)sx, y0 = (int)sy;
            const int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            const int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
            const float fx = sx - (float)x0;
            const float fy = sy - (float)y0;

            for (int c = 0; c < 3; c++) {
                const float v00 = src[((size_t)y0 * sw + x0) * 3 + c];
                const float v01 = src[((size_t)y0 * sw + x1) * 3 + c];
                const float v10 = src[((size_t)y1 * sw + x0) * 3 + c];
                const float v11 = src[((size_t)y1 * sw + x1) * 3 + c];
                const float top = v00 + (v01 - v00) * fx;
                const float bot = v10 + (v11 - v10) * fx;
                float val = top + (bot - top) * fy;
                if (val < 0.0f) val = 0.0f;
                if (val > 255.0f) val = 255.0f;
                o[c] = (uint8_t)(val + 0.5f);
            }
        }
    }
}

/* ---- 特征 --------------------------------------------------------------- */

void rknn_l2_normalize(float *v, int n)
{
    if (!v || n <= 0)
        return;
    float s = 0.0f;
    for (int i = 0; i < n; i++)
        s += v[i] * v[i];
    if (s <= 0.0f)
        return;                             /* 零向量不动,避免除零 */
    const float inv = 1.0f / sqrtf(s);
    for (int i = 0; i < n; i++)
        v[i] *= inv;
}

float rknn_cosine(const float *a, const float *b, int n)
{
    if (!a || !b || n <= 0)
        return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < n; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    const float d = sqrtf(na) * sqrtf(nb);
    return d > 0.0f ? dot / d : 0.0f;
}

void rknn_rgb_norm_f32(const uint8_t *rgb, int n_pixels, float *out)
{
    if (!rgb || !out || n_pixels <= 0)
        return;
    for (int i = 0; i < n_pixels * 3; i++)
        out[i] = ((float)rgb[i] - 127.5f) / 127.5f;
}
