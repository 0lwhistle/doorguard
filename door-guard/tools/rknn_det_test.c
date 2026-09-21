/*
 * rknn_det_test.c — 检测链路离线对拍工具(板上运行,非产品代码)
 *
 * 用法:rknn_det_test <model.rknn> <raw_rgb_320x320x3> [阈值]
 *
 * 把一张**已按同法 letterbox 好的**原始 RGB 直接喂进 NPU 并走完整解码
 * (rknn_retinaface_decode + NMS),打印检出框。用于在**没有人站镜头前**时
 * 验证"推理+解码"这条链路是否正确——拿已知人脸位置的标准图对拍,
 * 比让人站着试靠谱,也能把 RGA 与解码两件事分开定位。
 *
 * 与 vision_rknn.c 用同一套库调用,唯一差别是输入来自文件而非 RGA。
 */
#include "npu_model.h"
#include "rknn_face.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CAND 256

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <model.rknn> <raw_rgb_320x320x3> [阈值]\n", argv[0]);
        return 2;
    }
    const float thresh = (argc > 3) ? (float)atof(argv[3]) : 0.5f;

    npu_model_t *m = npu_model_load(argv[1]);
    if (!m)
        return 1;

    npu_attr_t in;
    if (npu_model_input_attr(m, 0, &in) != DG_OK) {
        npu_model_release(m);
        return 1;
    }
    printf("模型输入 %dx%dx%d,期望 %zu B\n", in.width, in.height, in.channels, in.nbytes);

    FILE *fp = fopen(argv[2], "rb");
    if (!fp) {
        perror("打开 raw 失败");
        npu_model_release(m);
        return 1;
    }
    uint8_t *buf = malloc(in.nbytes);
    size_t got = fread(buf, 1, in.nbytes, fp);
    fclose(fp);
    if (got != in.nbytes) {
        fprintf(stderr, "raw 大小不符:读到 %zu 期望 %zu\n", got, in.nbytes);
        free(buf);
        npu_model_release(m);
        return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = npu_model_run(m, buf, in.nbytes, DG_NPU_TYPE_U8);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("推理 %s,%.1f ms\n", rc == DG_OK ? "成功" : "失败",
           (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    if (rc != DG_OK) {
        free(buf);
        npu_model_release(m);
        return 1;
    }
    free(buf);

    /* 三个输出:框 / 分 / 关键点(尺寸按模型查出来) */
    npu_attr_t o;
    npu_model_output_attr(m, 0, &o);
    const int n = (o.n_dims >= 2) ? (int)o.dims[1] : 0;
    float *loc = malloc(sizeof(float) * n * 4);
    float *conf = malloc(sizeof(float) * n * 2);
    float *landm = malloc(sizeof(float) * n * 10);
    rknn_face_t *cand = malloc(sizeof(rknn_face_t) * MAX_CAND);
    if (!loc || !conf || !landm || !cand) {
        fprintf(stderr, "分配失败\n");
        return 1;
    }
    int ok = npu_model_output_f32(m, 0, loc, n * 4, NULL) == DG_OK &&
             npu_model_output_f32(m, 1, conf, n * 2, NULL) == DG_OK &&
             npu_model_output_f32(m, 2, landm, n * 10, NULL) == DG_OK;
    npu_model_release(m);
    if (!ok) {
        fprintf(stderr, "取输出失败\n");
        return 1;
    }

    printf("锚框 %d,解码阈值 %.2f\n", n, thresh);
    int cnt = rknn_retinaface_decode(loc, conf, landm, n, in.width, thresh, cand, MAX_CAND);
    if (cnt < 0) {
        printf("解码失败(%d)——锚框数与输入不匹配?\n", cnt);
        return 1;
    }
    printf("解码候选 %d 个\n", cnt);
    if (cnt > MAX_CAND)
        cnt = MAX_CAND;
    int keep = rknn_nms(cand, cnt, 0.4f);
    printf("NMS 后 %d 个脸:\n", keep);
    for (int i = 0; i < keep; i++) {
        printf("  #%d 框 (%.0f, %.0f)-(%.0f, %.0f)  %.0fx%.0f  分数 %.4f\n",
               i, cand[i].x1, cand[i].y1, cand[i].x2, cand[i].y2,
               cand[i].x2 - cand[i].x1, cand[i].y2 - cand[i].y1, cand[i].score);
        printf("     关键点 ");
        for (int k = 0; k < RKNN_FACE_KPS; k++)
            printf("(%.0f,%.0f) ", cand[i].kps[k][0], cand[i].kps[k][1]);
        printf("\n");
    }
    return 0;
}
