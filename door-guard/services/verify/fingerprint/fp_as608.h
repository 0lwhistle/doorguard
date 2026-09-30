/*
 * fp_as608.h — AS608 指纹模组协议层(纯函数,不持线程不碰 IO)
 *
 * 帧协议唯一事实源:docs/tech/FINGERPRINT_PROTOCOL.md(官方 51 例程
 * FPM10A.c 逐字节冻结);本层只做组包/解析/校验和/确认码语义,
 * 指令序列编排与业务线程在 fp_provider。
 *
 * 纪律:码值以协议文档 §2 为准——【已冻结】指令的 golden 断言在
 * tests/test_fp_proto.c,改码值先过测试。
 */
#ifndef DG_FP_AS608_H
#define DG_FP_AS608_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 帧常量 ---- */
#define FP_A608_HDR_LEN       6    /* EF 01 FF FF FF FF */
#define FP_A608_TYPE_CMD      0x01
#define FP_A608_TYPE_DATA     0x02
#define FP_A608_TYPE_ACK      0x07
#define FP_A608_TYPE_END      0x08
#define FP_A608_PAYLOAD_MAX   600  /* UpChar 数据包 512B 特征 + 余量 */
#define FP_A608_FRAME_MAX     (FP_A608_HDR_LEN + 1 + 2 + FP_A608_PAYLOAD_MAX + 2)

/* CharBuffer 编号(采集/特征缓冲) */
#define FP_A608_BUF1          1
#define FP_A608_BUF2          2

/* ---- 确认码(协议文档 §3) ---- */
enum {
    FP_ACK_OK          = 0x00,
    FP_ACK_RECV_ERR    = 0x01,   /* 收包错误(校验和/格式) */
    FP_ACK_NO_FINGER   = 0x02,   /* 传感器无手指/采集不到 */
    FP_ACK_ENROLL_FAIL = 0x03,   /* 录入失败(指纹太干) */
    FP_ACK_DRY_IMAGE   = 0x04,   /* 图像太干/太淡不成像 */
    FP_ACK_MERGE_FAIL  = 0x05,   /* 合成失败(两次特征差异大) */
    FP_ACK_NOT_FOUND   = 0x09,   /* 1:N 未搜索到 */
    FP_ACK_LIB_FULL    = 0x0A,   /* 模板库已满 */
    FP_ACK_FLASH_ERR   = 0x11,   /* 写 flash 错误 */
    FP_ACK_NO_BUF_IMG  = 0x15,   /* 缓冲区无有效原图(流程顺序错) */
};

/** 确认码语义名(日志用);未知名返回 "UNK_xx" */
const char *fp_as608_confirm_name(uint16_t code);

/* ---- 组包 ---- */

/**
 * 通用指令包:out = 包头 + 0x01 + 长度 + cmd + params + 校验和。
 * @return 帧总字节数;缓冲不足返回 0(调用方断言容量,正常运行不触发)
 */
size_t fp_as608_build_cmd(uint8_t *out, size_t cap, uint8_t cmd,
                          const uint8_t *params, size_t nparams);

/* 逐指令便捷封装(参数式见协议文档 §2;返回同 build_cmd,0=容量不足) */
size_t fp_as608_get_image(uint8_t *out, size_t cap);
size_t fp_as608_img2tz(uint8_t *out, size_t cap, uint8_t buf_id);
size_t fp_as608_match(uint8_t *out, size_t cap);
size_t fp_as608_search(uint8_t *out, size_t cap, uint8_t buf_id,
                       uint16_t start_page, uint16_t page_num);
size_t fp_as608_reg_model(uint8_t *out, size_t cap);
size_t fp_as608_store(uint8_t *out, size_t cap, uint8_t buf_id, uint16_t page_id);
size_t fp_as608_load_char(uint8_t *out, size_t cap, uint8_t buf_id, uint16_t page_id);
size_t fp_as608_delet_char(uint8_t *out, size_t cap, uint16_t page_id, uint16_t count);
size_t fp_as608_empty(uint8_t *out, size_t cap);
size_t fp_as608_valid_num(uint8_t *out, size_t cap);
size_t fp_as608_verify_psw(uint8_t *out, size_t cap, uint32_t password);

/* ---- 解析(流式;容忍一帧跨多次到达/粘包/前后噪声字节) ---- */

typedef struct {
    uint8_t  type;                       /**< 标识码(0x07 应答 / 0x02 数据 / 0x08 结束) */
    uint8_t  payload[FP_A608_PAYLOAD_MAX]; /**< 标识码后的载荷,不含校验和:
                                              应答 = [确认码2B, (结果参数)];数据 = 特征 */
    uint16_t payload_len;                /**< payload 有效字节数(= 帧长字段 - 2) */
} fp_frame_t;

typedef struct {
    int      state;                      /**< 内部状态机,调用方置零后 init */
    uint8_t  hdr_idx;
    uint8_t  type;
    uint16_t len_raw;                    /**< 原始帧长字段(校验和要用它,勿折算) */
    uint16_t need;                       /**< 标识码后实际待读字节数(含校验和) */
    uint16_t got;
} fp_parser_t;

void fp_as608_parser_init(fp_parser_t *p);

/**
 * 喂字节:1 = 完整帧在 *out(校验和已验);0 = 还需更多;-1 = 协议错
 * (包头/校验和/长度超限,内部已自动复位,调用方直接丢弃续喂)。
 * *consumed = 实际吞掉的字节数;一次喂入含多帧(如 UpChar 应答+数据包连发)
 * 时每次只出一帧,调用方必须把 buf+consumed 起的剩余字节再次喂入。
 */
int fp_as608_parse(fp_parser_t *p, const uint8_t *buf, size_t len,
                   fp_frame_t *out, size_t *consumed);

/* ---- 应答取参(调用方保证 frame 来自 ACK;越界访问由调用方容量保证) ---- */

/** 指令应答的确认码(应答 payload 前 2 字节,大端) */
uint16_t fp_as608_ack_confirm(const fp_frame_t *f);
/** Search 应答:命中页号与得分(仅确认码 0x00 时有效) */
void fp_as608_search_result(const fp_frame_t *f, uint16_t *page_id, uint16_t *score);

#ifdef __cplusplus
}
#endif

#endif /* DG_FP_AS608_H */
