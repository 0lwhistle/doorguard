# liveness — 动作活体(单目 RGB 动作指令式)

## 职责

按 `PROJECT_PLAN.md §二` 的产品定义,活体是"动作指令式"防伪:随机下发
2~3 个动作(眨眼 / 张嘴 / 摇头等),用户在镜头前完成,全过才允许开门。
本模块只做**判定**,不做 UI 引导(引导动画/文案在 UI 层,经事件总线)。

## 数据流

```
vision_rockiva ──liveness_service_on_face(106 关键点, 质量分)──▶ 本模块(逐帧记账)
                                                                │
vision 命中发布前 ◀──liveness_service_pass()────────────────────┘ 门禁
```

为什么关键点直调而不走事件总线:每帧 106 点、帧率级频率,总线是"命令/通知"
通道(§架构:大数据走直连或环形缓冲)。命中门禁是一次性查询,同样适合直调。
模块间其余通信一律走 `proto/events.h`。

## B7 现状(B7_FACE_HANDOFF.md §2.3)

- `liveness_service_on_face()`:空实现,只记最近一次关键点/质量与时刻
  (`liveness_service_last_face_age_ms()` 供联调观测),返回 `DG_OK`;
- `liveness_service_pass()`:**恒 true**(算法未实现,若返回 false 会让
  门禁永远不开门);`cfg liveness_enable=1` 时打一次告警,明示"已启用但
  未实现,本次放行",不做静默失效;
- 视觉侧已开 106 点(`faceLandmarkEnable=2`)并按帧回灌,门禁已接线。

## B8 计划

1. 动作状态机:E 版本用几何量——眨眼=EAR 边沿、点头=纵向位移比、
   摇头/转头=yaw 往返(106 点取眼/鼻/嘴轮廓点);
2. 随机 2~3 指令序列(`cfg liveness_actions_min/max`)、每步 5s 超时
   (`cfg liveness_timeout_ms`)、全过置 pass,失败/超时置 fail;
3. UI 动作引导页(指令文案 + 进度)+ 事件上行;`cfg liveness_enable` 打开。

## 测试

`tests/` 依赖:门禁为纯判定接口,可用 `dg_face_pt_t` 序列在宿主 gcc 下
直接驱动(B8 补 `test_liveness.c`)。
