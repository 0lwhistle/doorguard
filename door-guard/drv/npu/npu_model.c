/*
 * npu_model.c — NPU 推理库实现(RKNN 运行时薄封装)
 *
 * 本文件是全仓唯一 include <rknn_api.h> 的地方:上层只认 npu_model.h 的
 * 类型与函数,换推理运行时(别的 NPU 栈)只改这一个 .c,接口不变
 * —— 与 drv 层其它驱动(gpio/uart)同一纪律。
 *
 * 错误码:统一用 proto/err.h,不把 rknn 的负值泄漏出去(调用方不该认识 RKNN_ERR_*)。
 */
#include "npu_model.h"
#include "dg_log.h"

#include <rknn_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "[NPU]";

/* rknn context 与"最近一次"输出;一个句柄一个线程用(见头注) */
struct npu_model {
    rknn_context ctx;
    char path[256];
    rknn_input_output_num io;
    rknn_tensor_attr in0;      /* 缓存输入 0 的属性:run() 填 rknn_input 用 */
    rknn_output *outs;         /* 非 NULL = 持有未还的输出 */
};

/* 版本字符串:rknn_query 要 context,所以第一次 load 成功后缓存。
 * 两个版本各 256 字节,定长截断避免 -Wformat-truncation(项目零告警纪律) */
static char s_ver[560] = "-";

const char *npu_hal_version(void)
{
    return s_ver;
}

static dg_npu_type_t map_type(rknn_tensor_type t)
{
    switch (t) {
    case RKNN_TENSOR_UINT8:  return DG_NPU_TYPE_U8;
    case RKNN_TENSOR_INT8:   return DG_NPU_TYPE_I8;
    case RKNN_TENSOR_FLOAT16:return DG_NPU_TYPE_F16;
    case RKNN_TENSOR_FLOAT32:return DG_NPU_TYPE_F32;
    case RKNN_TENSOR_INT32:  return DG_NPU_TYPE_I32;
    default:                 return DG_NPU_TYPE_OTHER;
    }
}

static dg_npu_fmt_t map_fmt(rknn_tensor_format f)
{
    switch (f) {
    case RKNN_TENSOR_NCHW: return DG_NPU_FMT_NCHW;
    case RKNN_TENSOR_NHWC: return DG_NPU_FMT_NHWC;
    default:               return DG_NPU_FMT_OTHER;
    }
}

/* 维度顺序随布局:NHWC=[n,h,w,c] / NCHW=[n,c,h,w];非 4 维(dims 不描述的
 * 向量输出)只保证 elems/nbytes 可用,宽高尽力而为 */
static void fill_attr(const rknn_tensor_attr *a, npu_attr_t *o)
{
    memset(o, 0, sizeof(*o));
    o->n_dims = a->n_dims;
    for (uint32_t i = 0; i < a->n_dims && i < 4; i++)
        o->dims[i] = a->dims[i];
    o->type  = map_type(a->type);
    o->fmt   = map_fmt(a->fmt);
    o->elems = a->n_elems;
    o->nbytes = a->size;
    o->quant = (a->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    o->zp    = a->zp;
    o->scale = a->scale;

    if (a->n_dims == 4) {
        if (o->fmt == DG_NPU_FMT_NCHW) {
            o->channels = (int)a->dims[1];
            o->height   = (int)a->dims[2];
            o->width    = (int)a->dims[3];
        } else {
            o->height   = (int)a->dims[1];
            o->width    = (int)a->dims[2];
            o->channels = (int)a->dims[3];
        }
    } else if (a->n_dims >= 1) {
        o->width    = (int)a->dims[a->n_dims - 1];
        o->height   = (a->n_dims >= 2) ? (int)a->dims[a->n_dims - 2] : 1;
        o->channels = 1;
    }
}

npu_model_t *npu_model_load(const char *path)
{
    if (!path || !path[0])
        return NULL;

    npu_model_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    snprintf(m->path, sizeof(m->path), "%s", path);

    /* size=0 + model 传路径:rknn 自行读文件 */
    int rc = rknn_init(&m->ctx, (void *)path, 0, 0, NULL);
    if (rc != RKNN_SUCC) {
        DG_LOGE(TAG, "rknn_init 失败(%d):%s(文件缺失/非 rknn 模型/驱动不匹配?)",
                rc, path);
        free(m);
        return NULL;
    }

    if (rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &m->io, sizeof(m->io)) != RKNN_SUCC) {
        DG_LOGE(TAG, "查询输入输出数失败:%s", path);
        rknn_destroy(m->ctx);
        free(m);
        return NULL;
    }

    memset(&m->in0, 0, sizeof(m->in0));
    m->in0.index = 0;
    if (rknn_query(m->ctx, RKNN_QUERY_INPUT_ATTR, &m->in0, sizeof(m->in0)) != RKNN_SUCC) {
        DG_LOGE(TAG, "查询输入属性失败:%s", path);
        rknn_destroy(m->ctx);
        free(m);
        return NULL;
    }

    if (strcmp(s_ver, "-") == 0) {
        rknn_sdk_version v;
        memset(&v, 0, sizeof(v));
        if (rknn_query(m->ctx, RKNN_QUERY_SDK_VERSION, &v, sizeof(v)) == RKNN_SUCC)
            snprintf(s_ver, sizeof(s_ver), "api=%.255s drv=%.255s",
                     v.api_version, v.drv_version);
    }

    DG_LOGI(TAG, "模型就绪 %s(in=%u out=%u,输入 %ux%ux%u %s)", path,
            m->io.n_input, m->io.n_output,
            (unsigned)m->in0.dims[2], (unsigned)m->in0.dims[1],
            (unsigned)m->in0.dims[3],
            (m->in0.fmt == RKNN_TENSOR_NHWC) ? "NHWC" : "NCHW");
    return m;
}

void npu_model_release(npu_model_t *m)
{
    if (!m)
        return;
    if (m->outs) {
        rknn_outputs_release(m->ctx, m->io.n_output, m->outs);
        free(m->outs);
        m->outs = NULL;
    }
    if (m->ctx)
        rknn_destroy(m->ctx);
    free(m);
}

uint32_t npu_model_input_num(const npu_model_t *m)  { return m ? m->io.n_input : 0; }
uint32_t npu_model_output_num(const npu_model_t *m) { return m ? m->io.n_output : 0; }
const char *npu_model_path(const npu_model_t *m)    { return m ? m->path : "-"; }

static int query_io(const npu_model_t *m, uint32_t idx, bool is_input, npu_attr_t *out)
{
    if (!m || !out)
        return DG_ERR_PARAM;
    uint32_t n = is_input ? m->io.n_input : m->io.n_output;
    if (idx >= n)
        return DG_ERR_PARAM;

    rknn_tensor_attr a;
    memset(&a, 0, sizeof(a));
    a.index = idx;
    int rc = rknn_query(m->ctx,
                        is_input ? RKNN_QUERY_INPUT_ATTR : RKNN_QUERY_OUTPUT_ATTR,
                        &a, sizeof(a));
    if (rc != RKNN_SUCC) {
        DG_LOGE(TAG, "查询%s属性失败(idx=%u,rc=%d):%s",
                is_input ? "输入" : "输出", idx, rc, m->path);
        return DG_ERR_IO;
    }
    fill_attr(&a, out);
    return DG_OK;
}

int npu_model_input_attr(const npu_model_t *m, uint32_t idx, npu_attr_t *out)
{
    return query_io(m, idx, true, out);
}

int npu_model_output_attr(const npu_model_t *m, uint32_t idx, npu_attr_t *out)
{
    return query_io(m, idx, false, out);
}

int npu_model_run(npu_model_t *m, const void *input, size_t in_bytes)
{
    if (!m)
        return DG_ERR_NOT_INIT;
    if (!input || in_bytes == 0)
        return DG_ERR_PARAM;
    /* 尺寸不符立即报错:喂错尺寸的缓冲是静默出错图的经典来法 */
    if (m->in0.size && in_bytes != m->in0.size) {
        DG_LOGE(TAG, "输入 %zu B 与模型期望 %u B 不符(按 npu_model_input_attr 准备)",
                in_bytes, m->in0.size);
        return DG_ERR_PARAM;
    }

    /* 上一轮输出先还,否则每帧泄漏一块 */
    if (m->outs) {
        rknn_outputs_release(m->ctx, m->io.n_output, m->outs);
        free(m->outs);
        m->outs = NULL;
    }

    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index        = 0;
    in.buf          = (void *)input;
    in.size         = (uint32_t)in_bytes;
    in.type         = m->in0.type;
    in.fmt          = m->in0.fmt;
    in.pass_through = 0;

    int rc = rknn_inputs_set(m->ctx, 1, &in);
    if (rc != RKNN_SUCC) {
        DG_LOGE(TAG, "rknn_inputs_set 失败(%d):%s", rc, m->path);
        return DG_ERR_IO;
    }

    rc = rknn_run(m->ctx, NULL);
    if (rc != RKNN_SUCC) {
        DG_LOGE(TAG, "rknn_run 失败(%d):%s", rc, m->path);
        return DG_ERR_IO;
    }

    m->outs = calloc(m->io.n_output ? m->io.n_output : 1, sizeof(rknn_output));
    if (!m->outs)
        return DG_ERR_NO_MEMORY;
    for (uint32_t i = 0; i < m->io.n_output; i++) {
        m->outs[i].index       = i;
        m->outs[i].want_float  = 1;   /* 驱动负责反量化,上层只拿 float */
        m->outs[i].is_prealloc = 0;
    }
    rc = rknn_outputs_get(m->ctx, m->io.n_output, m->outs, NULL);
    if (rc != RKNN_SUCC) {
        DG_LOGE(TAG, "rknn_outputs_get 失败(%d):%s", rc, m->path);
        free(m->outs);
        m->outs = NULL;
        return DG_ERR_IO;
    }
    return DG_OK;
}

int npu_model_output_f32(const npu_model_t *m, uint32_t idx,
                         float *dst, uint32_t cap_elems, uint32_t *n_out)
{
    if (!m || !dst)
        return DG_ERR_PARAM;
    if (!m->outs || idx >= m->io.n_output)
        return DG_ERR_NOT_INIT;

    /* want_float=1 → 缓冲是 float32,size 是字节数 */
    uint32_t elems = m->outs[idx].size / (uint32_t)sizeof(float);
    if (elems > cap_elems) {
        DG_LOGE(TAG, "输出 %u 元素超出 dst 容量 %u(不截断):%s",
                elems, cap_elems, m->path);
        return DG_ERR_PARAM;
    }
    memcpy(dst, m->outs[idx].buf, m->outs[idx].size);
    if (n_out)
        *n_out = elems;
    return DG_OK;
}
