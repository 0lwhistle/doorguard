#!/bin/bash
# perf_switch.sh — 切页响应测量:记录 tap 前帧 md5,注入点击,轮询导图比对帧变化耗时
# 用法:perf_switch.sh <x> <y> <标签>  (须在主页/菜单等可切页状态)
PID=$(pidof door-guard)
[ -z "$PID" ] && { echo "NO_PROCESS"; exit 1; }
X=$1; Y=$2; TAG=$3

shotmd5() {
    rm -f /tmp/dg_walk/dg_screen.raw
    touch /tmp/dg_shot
    for i in $(seq 1 40); do
        [ -f /tmp/dg_walk/dg_screen.raw ] && break
        sleep 0.03
    done
    md5sum /tmp/dg_walk/dg_screen.raw 2>/dev/null | cut -d' ' -f1
}

M0=$(shotmd5)
T0=$(date +%s%N)
printf "P $X $Y\nR\n" >> /tmp/dg_touch
# 轮询导图直到帧变化(粒度约 70ms:导图+md5 一次 ~70ms)
for i in $(seq 1 100); do
    M=$(shotmd5)
    if [ -n "$M" ] && [ "$M" != "$M0" ]; then
        T1=$(date +%s%N)
        echo "SWITCH_MS=$(( (T1 - T0) / 1000000 ))"
        echo "TAG=$TAG"
        exit 0
    fi
done
echo "SWITCH_TIMEOUT TAG=$TAG"
