/*
 * iccard_hal.c — IC 读卡器设备节点封装实现(契约见 iccard_hal.h / ICCARD_PROTOCOL.md)
 *
 * 驱动未上线时 open 失败是常态(应用走降级,整机不受影响,协议 §10 失败隔离);
 * "sim" 设备名走 pipe 模拟后端(见 iccard_hal_sim.c,仅宿主编译)。
 */
#include "iccard_hal.h"
#include "dg_log.h"
#include "types.h"                    /* DG_IC_LEN(卡号串上限与 proto 一致) */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *TAG = "[ICCARD]";

/* sim 后端("sim" 设备名):pipe 读端即句柄,poll/read/close 全走原生路径,
 * HAL 主体零分派,注入端见 iccard_hal_sim.c(仅宿主编译) */
int iccard_sim_open(void);

/* 板上构建没有 sim 源文件:弱符号桩顶住链接,真配 "sim" 名返回不支持 */
__attribute__((weak)) int iccard_sim_open(void)
{
    return DG_ERR_UNSUPPORTED;
}

int iccard_hal_open(const char *dev_path)
{
    if (!dev_path || !dev_path[0])
        return DG_ERR_PARAM;
    if (!strcmp(dev_path, "sim"))
        return iccard_sim_open();

    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        /* 驱动未上线时本函数每 2s 被退避重试打一次:限频到 ~30s 一条,
         * 否则降级期日志全被本行淹没(2026-10-03 板上 10min 300+ 条) */
        static int n_log;
        if (n_log++ % 15 == 0)
            DG_LOGW(TAG, "open %s: %s(读卡器未就绪?走降级重试;此日志 30s 限频)",
                    dev_path, strerror(errno));
        return (errno == EBUSY) ? DG_ERR_BUSY : DG_ERR_IO;
    }
    return fd;
}

int iccard_hal_poll(int fd, int timeout_ms)
{
    if (fd < 0)
        return DG_ERR_PARAM;

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : DG_ERR_IO;
    if (pr == 0)
        return 0;
    if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))
        return DG_ERR_IO;
    return (pfd.revents & POLLIN) ? 1 : 0;
}

int iccard_hal_read(int fd, dg_iccard_frame_t *out)
{
    if (fd < 0 || !out)
        return DG_ERR_PARAM;

    /* 驱动契约:阻塞 read 一次返回一整帧;此处对短读补齐属防御性
     * (半帧只有驱动 bug 才会产生,补齐比丢帧好诊断) */
    uint8_t *p = (uint8_t *)out;
    size_t got = 0;
    while (got < DG_ICCARD_FRAME_SZ) {
        ssize_t n = read(fd, p + got, DG_ICCARD_FRAME_SZ - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return DG_ERR_IO;
        }
        if (n == 0)
            return DG_ERR_IO;              /* EOF = 节点失联 */
        got += (size_t)n;
    }
    return DG_OK;
}

int iccard_hal_flush(int fd)
{
    if (fd < 0)
        return DG_ERR_PARAM;
    if (ioctl(fd, DG_ICCARD_IOC_FLUSH) == 0)
        return DG_OK;
    if (errno != ENOTTY) {
        DG_LOGW(TAG, "FLUSH ioctl: %s", strerror(errno));
        return DG_ERR_IO;
    }
    /* sim 后端(pipe)没有 ioctl:非阻塞排水到 EAGAIN 等价清缓冲 */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return DG_ERR_IO;
    uint8_t drain[DG_ICCARD_FRAME_SZ];
    while (read(fd, drain, sizeof(drain)) > 0) {
    }
    fcntl(fd, F_SETFL, fl);
    return (errno == EAGAIN) ? DG_OK : DG_ERR_IO;
}

void iccard_hal_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* ---- 纯函数:帧校验与卡号字符串化 ---- */

bool iccard_frame_valid(const dg_iccard_frame_t *f)
{
    return f && f->magic == DG_ICCARD_MAGIC && f->uid_len >= 4 &&
           f->uid_len <= DG_ICCARD_UID_MAX;
}

int iccard_uid_to_hex(const uint8_t *uid, uint8_t uid_len, char *out)
{
    /* DG_IC_LEN=32 含 '\0':16B UID(32 字符)放不下——超 15B(30 字符)
     * 按 param 错拒收,调用方当坏帧丢弃。协议 §5 允许 4~16B,但现行
     * DB/事件/用户记录的卡号串口径是 DG_IC_LEN;扩上限要先动 types.h */
    if (!uid || !out || uid_len < 4 || uid_len * 2 > DG_IC_LEN - 1)
        return DG_ERR_PARAM;
    static const char HEX[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < uid_len; i++) {
        out[i * 2] = HEX[uid[i] >> 4];
        out[i * 2 + 1] = HEX[uid[i] & 0x0F];
    }
    out[uid_len * 2] = '\0';
    return DG_OK;
}

void iccard_mask(const char *card_no, char *out, size_t cap)
{
    if (!out || cap < 10) {
        if (out && cap)
            out[0] = '\0';
        return;
    }
    /* 协议 §5:HEX 卡号最短 8 字符,末 4 恒有;异常短串(脏数据)原样掩全 */
    size_t n = card_no ? strnlen(card_no, DG_IC_LEN) : 0;
    if (n < 8) {
        snprintf(out, cap, "********");
        return;
    }
    snprintf(out, cap, "********%s", card_no + n - 4);
}
