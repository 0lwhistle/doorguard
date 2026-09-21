/*
 * rknn_rec_test.c — ArcFace 特征链路验证工具(板上运行,非产品代码)
 *
 * 用法:rknn_rec_test <model.rknn> <mode> <a.raw> <b.raw> <c.raw>
 *   mode = u8  : raw 为 112×112×3 uint8 RGB(假设模型已烤入归一化)
 *   mode = f32 : raw 为 112×112×3 float32(已按 (x-127.5)/127.5 归一化)
 *
 * 对每个输入取 512 维 embedding 并 L2 归一化,打印两两余弦。
 * 判读(用"同一张脸的正常/变暗版本"+"纯色画布"三张图):
 *   cos(a,b) 应显著高(同人);cos(a,c) 应低;若 cos(a,b) 也低,
 *   说明输入归一化假设错了(换 mode 再试/找转换脚本核对 mean/std)。
 */
#include "npu_model.h"
#include "rknn_face.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIM 512

static int run_one(npu_model_t *m, const char *path, dg_npu_type_t type,
                   size_t bytes, float out[DIM])
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return -1;
    }
    uint8_t *buf = malloc(bytes);
    if (fread(buf, 1, bytes, fp) != bytes) {
        fprintf(stderr, "%s: 大小不符\n", path);
        fclose(fp);
        free(buf);
        return -1;
    }
    fclose(fp);

    if (npu_model_run(m, buf, bytes, type) != DG_OK) {
        free(buf);
        return -1;
    }
    free(buf);

    uint32_t n = 0;
    float raw[DIM];
    if (npu_model_output_f32(m, 0, raw, DIM, &n) != DG_OK || n != DIM) {
        fprintf(stderr, "输出 %u 元素,期望 %d\n", n, DIM);
        return -1;
    }
    memcpy(out, raw, sizeof(raw));
    rknn_l2_normalize(out, DIM);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "用法: %s <model.rknn> <u8|f32> <a.raw> <b.raw> <c.raw>\n", argv[0]);
        return 2;
    }
    const dg_npu_type_t type = (strcmp(argv[2], "f32") == 0) ? DG_NPU_TYPE_F32
                                                             : DG_NPU_TYPE_U8;
    npu_model_t *m = npu_model_load(argv[1]);
    if (!m)
        return 1;
    npu_attr_t in;
    if (npu_model_input_attr(m, 0, &in) != DG_OK || in.channels != 3) {
        fprintf(stderr, "输入属性异常\n");
        return 1;
    }
    const size_t bytes = (type == DG_NPU_TYPE_F32) ? (size_t)in.elems * 4
                                                   : (size_t)in.elems;
    printf("输入 %dx%dx%d,mode=%s(%zu B/张)\n", in.width, in.height, in.channels,
           argv[2], bytes);

    float e[3][DIM];
    const char *name[3] = { "a(正常)", "b(变暗)", "c(纯色)" };
    for (int i = 0; i < 3; i++) {
        if (run_one(m, argv[3 + i], type, bytes, e[i]) != 0)
            return 1;
        /* 未归一化的模长有诊断价值:正常应在十几~几十;饱和/全零说明输入约定错 */
        float sq = 0;
        for (int k = 0; k < DIM; k++)
            sq += e[i][k] * e[i][k];
        printf("%s: 归一化后模长 1.0(原始平方和 %.1f)\n", name[i], sq);
    }
    printf("cos(a,b) = %.4f   (同人不同亮度,应高)\n", rknn_cosine(e[0], e[1], DIM));
    printf("cos(a,c) = %.4f   (脸 vs 纯色,应低)\n", rknn_cosine(e[0], e[2], DIM));
    printf("cos(b,c) = %.4f\n", rknn_cosine(e[1], e[2], DIM));
    npu_model_release(m);
    return 0;
}
