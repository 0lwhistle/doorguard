/*
 * npu_probe.c — NPU 模型探针(板上运行,非产品代码)
 *
 * 用法:npu_probe <model.rknn> [model2.rknn ...]
 *
 * 打印每个模型的输入/输出张量规格(尺寸/布局/类型/量化参数)并**零输入试跑一次**,
 * 确认模型在本板 NPU 上真能跑起来(加载成功 ≠ 能推理:驱动版本不匹配要到 run 才暴露)。
 *
 * 为什么要有它:写后处理解码(RetinaFace 的 stride/anchor 网格等)必须先知道
 * 模型真实的输入尺寸与输出张量形状——这些随 .rknn 文件走,不能靠猜、也不该
 * 假设当初 rknn-toolkit2 是怎么配的。换模型第一件事就是跑它。
 */
#include "npu_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *tname(dg_npu_type_t t)
{
    switch (t) {
    case DG_NPU_TYPE_U8:  return "U8";
    case DG_NPU_TYPE_I8:  return "I8";
    case DG_NPU_TYPE_F16: return "F16";
    case DG_NPU_TYPE_F32: return "F32";
    case DG_NPU_TYPE_I32: return "I32";
    default:              return "?";
    }
}

static const char *fname(dg_npu_fmt_t f)
{
    switch (f) {
    case DG_NPU_FMT_NCHW: return "NCHW";
    case DG_NPU_FMT_NHWC: return "NHWC";
    default:              return "?";
    }
}

static void print_dims(const npu_attr_t *a)
{
    printf("dims=[");
    for (uint32_t d = 0; d < a->n_dims; d++)
        printf("%u%s", a->dims[d], (d + 1 < a->n_dims) ? "," : "");
    printf("]");
}

static void probe(const char *path)
{
    printf("=== %s ===\n", path);

    npu_model_t *m = npu_model_load(path);
    if (!m) {
        printf("  加载失败(看上一行 [NPU] 日志)\n\n");
        return;
    }
    printf("  运行时: %s\n", npu_hal_version());

    uint32_t nin = npu_model_input_num(m);
    printf("  输入 %u 个:\n", nin);
    for (uint32_t k = 0; k < nin; k++) {
        npu_attr_t a;
        if (npu_model_input_attr(m, k, &a) != DG_OK)
            continue;
        printf("    [%u] %dx%dx%d %s %s  ", k, a.width, a.height, a.channels,
               tname(a.type), fname(a.fmt));
        print_dims(&a);
        printf("  elems=%u bytes=%zu", a.elems, a.nbytes);
        if (a.quant)
            printf("  quant(scale=%.6f zp=%d)", a.scale, a.zp);
        printf("\n");
    }

    uint32_t nout = npu_model_output_num(m);
    printf("  输出 %u 个:\n", nout);
    for (uint32_t k = 0; k < nout; k++) {
        npu_attr_t a;
        if (npu_model_output_attr(m, k, &a) != DG_OK)
            continue;
        printf("    [%u] %s  ", k, tname(a.type));
        print_dims(&a);
        printf("  elems=%u bytes=%zu\n", a.elems, a.nbytes);
    }

    /* 零输入试跑:证明"能推理",并把首元素打出来(全 0 输入下值无意义,
     * 只用来确认输出非 NaN/非空) */
    npu_attr_t in;
    if (npu_model_input_attr(m, 0, &in) == DG_OK && in.nbytes > 0) {
        uint8_t *buf = calloc(1, in.nbytes);
        if (buf) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int rc = npu_model_run(m, buf, in.nbytes, DG_NPU_TYPE_U8);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (t1.tv_nsec - t0.tv_nsec) / 1e6;

            /* 压测(DG_NPU_BENCH=N):首跑含图编译/预热,稳态才是排期依据 */
            const char *bench = getenv("DG_NPU_BENCH");
            if (rc == DG_OK && bench && atoi(bench) > 0) {
                int n = atoi(bench);
                double best = 1e9;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (int i = 0; i < n; i++) {
                    struct timespec a, b;
                    clock_gettime(CLOCK_MONOTONIC, &a);
                    if (npu_model_run(m, buf, in.nbytes, DG_NPU_TYPE_U8) != DG_OK)
                        break;
                    clock_gettime(CLOCK_MONOTONIC, &b);
                    double t = (b.tv_sec - a.tv_sec) * 1000.0 +
                               (b.tv_nsec - a.tv_nsec) / 1e6;
                    if (t < best)
                        best = t;
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double total = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                               (t1.tv_nsec - t0.tv_nsec) / 1e6;
                printf("  压测 %d 次:均值 %.1f ms / 最快 %.1f ms(上限 %.0f fps)\n",
                       n, total / n, best, 1000.0 / best);
            }
            if (rc == DG_OK) {
                printf("  试跑通过:%.1f ms\n", ms);
                /* 逐输出按真实元素数分配:SCRFD 的 score 头就有 12800 元素,
                 * 写死缓冲会被 npu_model_output_f32 正确地拒掉(不截断) */
                for (uint32_t k = 0; k < nout; k++) {
                    npu_attr_t oa;
                    if (npu_model_output_attr(m, k, &oa) != DG_OK || oa.elems == 0)
                        continue;
                    float *f = malloc(sizeof(float) * oa.elems);
                    uint32_t n = 0;
                    if (f && npu_model_output_f32(m, k, f, oa.elems, &n) == DG_OK)
                        printf("    输出[%u] 前 3 元素 %.4f %.4f %.4f …(共 %u)\n",
                               k, f[0], n > 1 ? f[1] : 0.0f, n > 2 ? f[2] : 0.0f, n);
                    free(f);
                }
            } else {
                printf("  试跑失败(rc=%d)——模型能加载但跑不动,查驱动版本\n", rc);
            }
            free(buf);
        }
    }
    npu_model_release(m);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <model.rknn> [model2.rknn ...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++)
        probe(argv[i]);
    return 0;
}
