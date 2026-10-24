# services/capture — 取流健康巡检(WAIT→FLOW→DOWN)

**不是帧泵**——真帧流在 `modules/camera/camera_board.c` 的 V4L2 线程
(DQBUF→RGA→主循环信箱)。本服务每 1s 巡检帧序号/心跳:

- `WAIT`:V4L2 未出流(启动期/相机未就绪);
- `FLOW`:帧序号持续前进;
- `DOWN`:序号停走(传感器死机/EAGAIN 永挂)→ 发布 `EV_CAPTURE_STATE`
  (access_service 喂 FSM cam_ready:断流时人脸 1:1 立即 reason=9,
  UI bridge 触发降级提示)。

决策记录:断流**不做软件重推流**(白幕链路保留,2026-10 用户拍板)。
测试:`test_capture_health`(状态机纯逻辑,宿主)。
