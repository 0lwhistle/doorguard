/*
 * timeutil.h — 公共时基(components;2026-09-28 C2 收口)
 *
 * 此前 app/modules/services 六处各自 static now_ms(),且时钟语义分裂:
 * 看门狗心跳链(main/netcore)用 REALTIME,耗时测量(liveness/mdns/vision)
 * 用 MONOTONIC——netcore 注释记录过混用事故(换 MONOTONIC 与看门狗相差
 * 整个纪元基数,心跳必判超龄误重启)。本组件把两种语义显式分成两个函数,
 * 调用方按用途选,编译期消灭"再抄一份"与"抄错时钟"两个坑。
 *
 * 零依赖(仅 libc),任何层可 include。
 */
#ifndef DG_TIMEUTIL_H
#define DG_TIMEUTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 墙钟毫秒(CLOCK_REALTIME):看门狗心跳、日志时间戳、绝对时刻比较。
 *  受系统改时/NTP 校正影响——跨它计"经过时长"会跳变,那是 now_mono_ms 的活 */
int64_t now_ms(void);

/** 单调毫秒(CLOCK_MONOTONIC):耗时测量、超时窗口、防抖计时;不受改时影响 */
int64_t now_mono_ms(void);

/** 墙钟秒(unix 秒):落库时间戳(access_logs.ts 类) */
int64_t now_s(void);

#ifdef __cplusplus
}
#endif

#endif /* DG_TIMEUTIL_H */
