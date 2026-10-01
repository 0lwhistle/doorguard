/*
 * iccard_hal_sim.c — IC 读卡器模拟后端(仅宿主测试/PC 模拟器编译)
 *
 * 实现 = pipe 一对:读端就是 iccard_hal 的句柄(poll/read/flush 原生语义,
 * HAL 主体零分派),写端留给 iccard_sim_inject 注入符合 §3 帧格式的 UID。
 * 宿主测试没有真驱动,service→FSM→UI 全链路靠它打通(ICCARD_PROTOCOL §9)。
 */
#include "iccard_hal.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int s_wr = -1;                     /* 注入端(进程内唯一读卡器) */

int iccard_sim_open(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    fcntl(fds[0], F_SETFL, O_CLOEXEC);
    fcntl(fds[1], F_SETFL, O_CLOEXEC);
    if (s_wr >= 0)
        close(s_wr);                      /* 重复 open 换新会话:旧注入端作废 */
    s_wr = fds[1];
    return fds[0];
}

/** 注入一次寻卡(uid_len 4~16);宿主测试用它冒充驱动吐帧 */
int iccard_sim_inject(const uint8_t *uid, uint8_t uid_len)
{
    if (s_wr < 0 || !uid || uid_len < 4 || uid_len > DG_ICCARD_UID_MAX)
        return -1;
    dg_iccard_frame_t f;
    memset(&f, 0, sizeof(f));
    f.magic = DG_ICCARD_MAGIC;
    f.uid_len = uid_len;
    f.card_type = DG_ICCARD_TYPE_MF_CLASSIC;
    static uint16_t seq;
    f.seq = ++seq;
    memcpy(f.uid, uid, uid_len);
    return write(s_wr, &f, sizeof(f)) == (ssize_t)sizeof(f) ? 0 : -1;
}
