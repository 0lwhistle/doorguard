/*
 * rknn_spoof_test.c — 反欺骗(MiniFASNet)链路对拍工具(板上运行,非产品代码)
 *
 * 用法:rknn_spoof_test <modelA.rknn> <rawA.raw> <modelB.rknn> <rawB.raw>
 *   raw = 80×80×3 uint8 **BGR 原域 [0,255]**(模型不做归一化,官方 to_tensor
 *   无 /255);两个模型各吃自己 scale 的 crop(2.7 / 4.0),不要混。
 *
 * 判读:打印各模型 3 类概率([fake, real, other],softmax 已烤进模型)。
 * real 均分 = (probA[1]+probB[1])/2,与 tools/convert_antispoof/check_onnx.py
 * 导出的 raw 在 WSL 侧算出的分数比对,偏差应 <0.01(F16 vs F32):
 *   image_T1(真)→ real ≈ 0.994;image_F1(假)→ real ≈ 0.072。
 * 不一致 = 转换或输入域出错(先怀疑通道序/归一化)。
 */
#include "npu_model.h"

#include <stdio.h>
#include <stdlib.h>

#define SZ (80 * 80 * 3)

static int run_one(npu_model_t *m, const char *path, float prob[3])
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return -1;
    }
    uint8_t *buf = malloc(SZ);
    if (fread(buf, 1, SZ, fp) != SZ) {
        fprintf(stderr, "%s: 大小不符(应为 %d B)\n", path, SZ);
        fclose(fp);
        free(buf);
        return -1;
    }
    fclose(fp);

    /* F16 模型未烤归一化之外的预处理:输入域即原域 U8,类型 U8 由 rknn
     * 运行时按图内转换(none)处理;输出 F32 概率 */
    if (npu_model_run(m, buf, SZ, DG_NPU_TYPE_U8) != DG_OK) {
        free(buf);
        return -1;
    }
    free(buf);

    uint32_t n = 0;
    if (npu_model_output_f32(m, 0, prob, 3, &n) != DG_OK || n != 3) {
        fprintf(stderr, "输出 %u 元素,期望 3\n", n);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "用法: %s <modelA.rknn> <rawA.raw> <modelB.rknn> <rawB.raw>\n",
                argv[0]);
        return 1;
    }

    npu_model_t *ma = npu_model_load(argv[1]);
    npu_model_t *mb = npu_model_load(argv[3]);
    if (!ma || !mb)
        return 1;

    float pa[3], pb[3];
    if (run_one(ma, argv[2], pa) != 0 || run_one(mb, argv[4], pb) != 0)
        return 1;

    printf("A %s: [%.4f %.4f %.4f]\n", argv[1], pa[0], pa[1], pa[2]);
    printf("B %s: [%.4f %.4f %.4f]\n", argv[3], pb[0], pb[1], pb[2]);
    const float real = (pa[1] + pb[1]) / 2.0f;
    const int label = (pa[0] + pb[0]) > (pa[1] + pb[1])
                          ? ((pa[0] + pb[0]) > (pa[2] + pb[2]) ? 0 : 2)
                          : ((pa[1] + pb[1]) > (pa[2] + pb[2]) ? 1 : 2);
    printf("label=%d real均分=%.4f → %s\n", label, real,
           label == 1 ? "REAL" : "FAKE");
    npu_model_release(ma);
    npu_model_release(mb);
    return 0;
}
