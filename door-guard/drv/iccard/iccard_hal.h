/*
 * iccard_hal.h — IC 读卡器设备节点薄封装(ICCARD_PROTOCOL.md §2~§6 的应用侧)
 *
 * 分层:drv/iccard 只做 /dev/dg_iccard0 的 open/poll/read/flush,无线程无业务;
 * 持线程、防重窗、降级在 services/verify/ic/card_provider。帧结构与卡号字符串
 * 化规则见协议文档 §3/§5(驱动作者按 §2~§6 实现,本层与应用层按同文消费)。
 * sim 后端(pipe 注入)仅宿主测试/模拟器编译,板上不进固件。
 */
#ifndef DG_ICCARD_HAL_H
#define DG_ICCARD_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 帧契约(ICCARD_PROTOCOL §3,与驱动侧逐字节一致) ---- */

#define DG_ICCARD_MAGIC    0x30434349UL   /* 'I','C','C','0' 小端排列 */
#define DG_ICCARD_UID_MAX  16
#define DG_ICCARD_FRAME_SZ 24             /* packed 结构恒 24B(_Static_assert 锁) */

/** 卡型(仅诊断不参与业务;取值由驱动作者按实际芯片确认) */
typedef enum {
    DG_ICCARD_TYPE_UNKNOWN = 0,
    DG_ICCARD_TYPE_MF_CLASSIC = 1,
    DG_ICCARD_TYPE_MF_ULTRALIGHT = 2,
    DG_ICCARD_TYPE_NTAG = 3,
    DG_ICCARD_TYPE_DESFIRE = 4,
    DG_ICCARD_TYPE_FELICA = 5,
    DG_ICCARD_TYPE_OTHER = 0xFF,
} dg_iccard_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;                   /**< 恒 DG_ICCARD_MAGIC,不符整帧丢弃 */
    uint8_t  uid_len;                 /**< 合法 4~16,越界视为坏帧 */
    uint8_t  card_type;               /**< dg_iccard_type_t,仅诊断 */
    uint16_t seq;                     /**< 驱动内自 1 递增,丢帧/重复诊断用 */
    uint8_t  uid[DG_ICCARD_UID_MAX];  /**< UID 原样字节,不做字节序变换 */
} dg_iccard_frame_t;

_Static_assert(sizeof(dg_iccard_frame_t) == DG_ICCARD_FRAME_SZ,
               "iccard 帧必须 24B(ICCARD_PROTOCOL §3,与驱动二进制契约)");

/* ioctl(ICCARD_PROTOCOL §4;驱动侧同名定义,改这里必须双向确认协议文档) */
#define DG_ICCARD_IOC_MAGIC  'I'
#define DG_ICCARD_IOC_FLUSH  _IO(DG_ICCARD_IOC_MAGIC, 0x01)

typedef struct {
    uint32_t rx_frames;               /**< 完整帧累计 */
    uint32_t rx_overruns;             /**< 环形缓冲满被丢弃的帧数 */
    uint32_t bad_frames;              /**< magic/uid_len 非法被丢弃的帧数 */
    uint32_t irq_count;               /**< 芯片中断累计(轮询实现=寻卡周期数) */
} dg_iccard_stats_t;

/* ---- 节点操作 ---- */

/**
 * 打开读卡器节点(独占,第二读者 -EBUSY 由驱动保证)。
 * @param dev_path 设备节点(配置 iccard.dev_path);"sim" = 宿主模拟后端
 * @return fd(≥0)/ 负数错误码;fd 供 poll/read/flush/close 全族使用
 */
int iccard_hal_open(const char *dev_path);

/** 等待可读:1=有完整帧 / 0=超时 / 负数=错误(节点失联等,进降级) */
int iccard_hal_poll(int fd, int timeout_ms);

/**
 * 读一整帧(阻塞模式由驱动保证一次 read 一帧;本函数对短读循环补齐)。
 * @return DG_OK / DG_ERR_IO(通信错)/ DG_ERR_PARAM(缓冲<24B)
 */
int iccard_hal_read(int fd, dg_iccard_frame_t *out);

/** 清空驱动内帧缓冲(录入/验证换会话时调用,ICCARD_PROTOCOL §4) */
int iccard_hal_flush(int fd);

/** 关闭并释放独占(阻塞中的 poll/read 被驱动唤醒;close 前置停标志) */
void iccard_hal_close(int fd);

/* ---- sim 后端(仅宿主测试/PC 模拟器编译,板上不进固件) ---- */

/** 注入一次寻卡(uid_len 4~15;16B 超卡号串承载力,见 iccard_uid_to_hex) */
int iccard_sim_inject(const uint8_t *uid, uint8_t uid_len);

/* ---- 卡号字符串化(ICCARD_PROTOCOL §5,全链路唯一口径) ----
 * uid 按字节顺序大写 HEX 无分隔符:4B 04 A3 B2 C1 → "04A3B2C1";
 * 展示掩码 = "********" + 末 4 字符(界面与日志一律掩码)。
 * 实现为纯函数便于宿主单测(test_iccard_proto)。 */

/** 帧合法性:HEX 化前调用;magic 错/uid_len 越界 = 坏帧丢弃 */
bool iccard_frame_valid(const dg_iccard_frame_t *f);

/** uid → 卡号字符串;out 须 ≥DG_IC_LEN;uid_len 非法返回 DG_ERR_PARAM */
int iccard_uid_to_hex(const uint8_t *uid, uint8_t uid_len, char *out);

/** 卡号字符串合法性(ICCARD_PROTOCOL §5,录入/web 绑定入口共用):
 *  8~30 字符、偶数长、全大写 HEX(4~15B UID)。16B UID 超 DG_IC_LEN
 *  承载力(见 iccard_uid_to_hex),不在合法域内——输入侧一律先归一
 *  大写再校验,本函数不做归一 */
bool iccard_no_valid(const char *card_no);

/** 卡号 → 展示掩码("********"+末4);out 须 ≥10 字节 */
void iccard_mask(const char *card_no, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DG_ICCARD_HAL_H */
