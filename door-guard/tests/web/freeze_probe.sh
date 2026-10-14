#!/bin/bash
# freeze_probe.sh — 在线检测(判死)现场实验:
# 起探针→4s 后 SIGSTOP 冻结(不回 Pong 的半开场景)→8s 后上板取证:
# ESTABLISHED 连接数(判死=连接应已被服务端关掉)、订阅计数、无响应日志。
set -u
HOST=192.168.137.100
cd "$(dirname "$0")"

node ws_video_test.mjs "$HOST" 80 > /tmp/p3.log 2>&1 &
PID=$!
sleep 4
kill -STOP "$PID"
echo frozen_at_4s
sleep 8
ssh -o ConnectTimeout=5 root@"$HOST" '
  pid=$(pidof door-guard)
  est=$(grep -c " 01 " /proc/$pid/net/tcp 2>/dev/null)
  echo "board ESTAB(sockets of whole netns): $est"
  echo "订阅次数: $(grep -ac 订阅监控画面 /var/log/door-guard.log)"
  echo "无响应日志: $(grep -a 无响应 /var/log/door-guard.log | tail -1)"
  echo "停车日志: $(grep -a 无订阅者,推流取帧已停 /var/log/door-guard.log | tail -1)"
'
kill -9 "$PID" 2>/dev/null
echo "--- probe stdout:"
cat /tmp/p3.log
