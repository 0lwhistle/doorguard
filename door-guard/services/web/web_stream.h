/*
 * web_stream.h — web 监控推流(2026-10-14,spec-network §1:WS 周期推 JPEG 快照)
 *
 * 职责:把相机最新帧按固定帧率(10fps)降采样 + JPEG 编码,发布到进程内
 * 双缓冲;传输(mg_ws_send)在 web_server 的 loop 线程做,本模块绝不碰连接。
 * 编码线程自己拉 camera_latest()(pull,与 UI 轮询同款语义),不进事件总线、
 * 不占 NV12 监听槽(vision 独占)——相机侧只按位或打开 XRGB 转换
 * (camera_stream_preview_set)。**平时不跑**(用户拍板):web 页面点开始才
 * 订阅;无人订阅时线程睡在条件变量上(零唤醒),取帧开关同步关闭;
 * 断连/判死的退订路径见 web_server 的在线检测(周期 Ping+超时判死)。
 *
 * 用户拍板(2026-10-14):总开关 web.video_enabled 默认开;帧率固定 10fps
 * 不做档位;设备端不做"远程预览中"提示。
 */
#ifndef DG_WEB_STREAM_H
#define DG_WEB_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 发布缓冲容量(640 长边 q95 最坏情况的余量;发送侧按此分配拷贝缓冲) */
#define DG_WEB_STREAM_JPEG_CAP (128 * 1024)

/** 起编码线程(幂等)。配置 web.video_enabled=0 时空转启动,订阅请求被拒 */
int web_stream_start(void);

/** 停编码线程(join);幂等 */
void web_stream_stop(void);

/** 推流可用(配置开 && 线程在跑);false 时 video_on 应显式拒绝 */
bool web_stream_available(void);

/**
 * 订阅计数 +1(loop 线程或任意线程调用;线程安全)。
 * 0→1 时打开相机取帧开关;计数上限 4(web 单会话策略下实际 ≤1)。
 * @return DG_OK / DG_ERR_UNSUPPORTED(配置关闭或服务未起)/ DG_ERR_BUSY
 */
int web_stream_viewer_add(void);

/** 订阅计数 -1:连接断开/video_off 时必须配对调用(归零即停编码) */
void web_stream_viewer_remove(void);

/** 当前是否有订阅者(日志/诊断用) */
bool web_stream_active(void);

/**
 * 取最新已编码 JPEG(loop 线程的周期定时器调用;测试直调)。
 * @param seq 出参,帧序号(自增,未变=无新帧);可空
 * @return true=拷出有效帧 / false=尚无帧
 */
bool web_stream_snapshot(uint8_t *dst, size_t cap, size_t *len, uint32_t *seq);

#ifdef __cplusplus
}
#endif

#endif /* DG_WEB_STREAM_H */
