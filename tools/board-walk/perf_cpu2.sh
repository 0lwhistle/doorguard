#!/bin/sh
# perf_cpu.sh v2 — /proc/pid/stat utime+stime 增量 / 10s(USER_HZ=100),单核百分比
P=$(pidof door-guard)
[ -n "$P" ] || { echo NO_PROC; exit 1; }
A=$(awk '{print $14+$15}' /proc/$P/stat)
sleep 10
P=$(pidof door-guard)
[ -n "$P" ] || { echo DEAD_MID; exit 1; }
B=$(awk '{print $14+$15}' /proc/$P/stat)
echo "CPU_10S_TICKS=$((B-A)) PERCENT=$(( (B-A) / 10 ))"
