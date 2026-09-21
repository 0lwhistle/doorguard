/*
 * dg_jpeg.c — JPEG 编解码实现(接口见 dg_jpeg.h)
 *
 * libjpeg 的两个非默认用法都在这里消化:
 *   ① 内存源/内存目标(IJG 默认是 stdio,头像不落盘);
 *   ② 错误不 exit:默认 error_exit 会直接结束进程,必须换成 setjmp 跳出,
 *      否则一张损坏的头像图能把整个门禁带崩。
 * 线程约定:无静态状态,可多线程并发(每调用一份局部 jerr/cinfo)。
 */
#include "dg_jpeg.h"

#include <stdio.h>      /* jpeglib.h 的 stdio 目标管理器原型用到 FILE,须先包含 */
#include <jpeglib.h>
#include <setjmp.h>
#include <stdbool.h>
#include <string.h>

/* ---- 错误接管:error_exit 默认 exit(EXIT_FAILURE),换成 longjmp ---- */

struct dg_jpeg_err {
    struct jpeg_error_mgr pub;      /* 必须放首位:libjpeg 按基类指针访问 */
    jmp_buf jmp;
};

static void dg_jpeg_error_exit(j_common_ptr cinfo)
{
    struct dg_jpeg_err *e = (struct dg_jpeg_err *)cinfo->err;
    longjmp(e->jmp, 1);
}

/* ---- 内存目的地(编码):libjpeg 要三个回调,容量固定、不可扩 ---- */

struct dg_mem_dest {
    struct jpeg_destination_mgr pub;
    uint8_t *buf;
    size_t cap;
    size_t used;                        /* term 时回填的实际字节数 */
    bool overflow;                      /* 缓冲不足:经错误路径长跳返回 */
};

static void mem_dest_init(j_compress_ptr cinfo)
{
    /* 必须在这里挂上初始缓冲:挂着 NULL/0 的话 libjpeg 第一个字节就写空指针 */
    struct dg_mem_dest *d = (struct dg_mem_dest *)cinfo->dest;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = d->cap;
}

static boolean mem_dest_empty(j_compress_ptr cinfo)
{
    /* 缓冲满:不能继续走(emit_byte 会写到 buf 末尾之外),经标准错误路径
     * 长跳出来;overflow 标记让 setjmp 处返回 NO_MEMORY 而非笼统 INTERNAL */
    struct dg_mem_dest *d = (struct dg_mem_dest *)cinfo->dest;
    d->overflow = true;
    (*cinfo->err->error_exit)((j_common_ptr)cinfo);
    return TRUE;                        /* 不可达:error_exit 必长跳 */
}

static void mem_dest_term(j_compress_ptr cinfo)
{
    struct dg_mem_dest *d = (struct dg_mem_dest *)cinfo->dest;
    d->used = d->cap - d->pub.free_in_buffer;
}

/* ---- 内存源(解码):数据已全量在手,填满即返回"伪 EOI" ---- */

struct dg_mem_src {
    struct jpeg_source_mgr pub;
};

static void mem_src_init(j_decompress_ptr cinfo) { (void)cinfo; }

static boolean mem_src_fill(j_decompress_ptr cinfo)
{
    /* 全量数据一次挂上,理论上不会进来;进来了插入 EOI 让库体面收尾 */
    static const uint8_t eoi[2] = { 0xFF, JPEG_EOI };
    cinfo->src->next_input_byte = eoi;
    cinfo->src->bytes_in_buffer = 2;
    return TRUE;
}

static void mem_src_skip(j_decompress_ptr cinfo, long num)
{
    if (num > 0) {
        const size_t n = (size_t)num > cinfo->src->bytes_in_buffer
                             ? cinfo->src->bytes_in_buffer : (size_t)num;
        cinfo->src->next_input_byte += n;
        cinfo->src->bytes_in_buffer -= n;
    }
}

static void mem_src_term(j_decompress_ptr cinfo) { (void)cinfo; }

/* ---- 编码 ---- */

int dg_jpeg_encode_rgb(const uint8_t *rgb888, int w, int h, int quality,
                       uint8_t *out, size_t cap, size_t *out_len)
{
    if (!rgb888 || !out || !out_len || w <= 0 || h <= 0 ||
        quality < 1 || quality > 95)
        return DG_ERR_PARAM;

    struct dg_jpeg_err jerr;
    struct jpeg_compress_struct cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = dg_jpeg_error_exit;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_compress(&cinfo);
        return DG_ERR_INTERNAL;
    }

    struct dg_mem_dest dest;
    memset(&dest, 0, sizeof(dest));
    dest.buf = out;
    dest.cap = cap;
    dest.pub.init_destination = mem_dest_init;
    dest.pub.empty_output_buffer = mem_dest_empty;
    dest.pub.term_destination = mem_dest_term;

    jpeg_create_compress(&cinfo);
    cinfo.dest = &dest.pub;
    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    if (setjmp(jerr.jmp)) {
        /* dest 已挂上后出错:按溢出与否区分 NO_MEMORY / INTERNAL */
        const bool ovf = cinfo.dest && ((struct dg_mem_dest *)cinfo.dest)->overflow;
        jpeg_destroy_compress(&cinfo);
        return ovf ? DG_ERR_NO_MEMORY : DG_ERR_INTERNAL;
    }
    jpeg_start_compress(&cinfo, TRUE);

    /* 逐行喂(头像 ≤160 行,单行缓冲即可,不整图拷贝) */
    const size_t stride = (size_t)w * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(uintptr_t)(rgb888 + cinfo.next_scanline * stride);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);

    const size_t used = dest.used;      /* term 已回填实际字节数 */
    jpeg_destroy_compress(&cinfo);
    *out_len = used;
    return DG_OK;
}

/* ---- 解码 ---- */

int dg_jpeg_decode_rgb(const uint8_t *jpeg, size_t len, int scale_denom,
                       uint8_t *out_rgb888, size_t cap, int *w, int *h)
{
    if (!jpeg || !out_rgb888 || !w || !h || len < 4 ||
        (scale_denom != 1 && scale_denom != 2 && scale_denom != 4 && scale_denom != 8))
        return DG_ERR_PARAM;

    struct dg_jpeg_err jerr;
    struct jpeg_decompress_struct cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = dg_jpeg_error_exit;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return DG_ERR_INTERNAL;       /* 损坏输入:报错返回,调用方显示占位 */
    }

    struct dg_mem_src src;
    memset(&src, 0, sizeof(src));
    src.pub.init_source = mem_src_init;
    src.pub.fill_input_buffer = mem_src_fill;
    src.pub.skip_input_data = mem_src_skip;
    src.pub.resync_to_restart = jpeg_resync_to_restart;
    src.pub.term_source = mem_src_term;
    src.pub.bytes_in_buffer = len;
    src.pub.next_input_byte = (const JOCTET *)jpeg;

    jpeg_create_decompress(&cinfo);
    cinfo.src = &src.pub;
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    cinfo.scale_denom = scale_denom;
    jpeg_start_decompress(&cinfo);

    const int dw = (int)cinfo.output_width;
    const int dh = (int)cinfo.output_height;
    if ((size_t)dw * (size_t)dh * 3 > cap) {
        jpeg_destroy_decompress(&cinfo);
        *w = dw;
        *h = dh;
        return DG_ERR_NO_MEMORY;
    }

    const size_t stride = (size_t)dw * 3;
    while (cinfo.output_scanline < (unsigned)dh) {
        JSAMPROW row = (JSAMPROW)(out_rgb888 + cinfo.output_scanline * stride);
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    /* 截断/脏数据:libjpeg 只发警告照样解(伪 EOI 体面收尾),剩余行是灰块
     * ——头像库都是我们自己编码的,出现任何警告都按损坏处理,调用方显示占位 */
    const bool warned = jerr.pub.num_warnings > 0;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    if (warned)
        return DG_ERR_INTERNAL;
    *w = dw;
    *h = dh;
    return DG_OK;
}
