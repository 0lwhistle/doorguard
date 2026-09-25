#!/bin/bash
# perf_cpu.sh — 整机 CPU:utime+stime 差值 / 10s / 100 = 单核百分比(busybox 兼容,纯 shell)
PID=$(pidof door-guard)
[ -z "$PID" ] && { echo "NO_PROCESS"; exit 1; }
f() {
    set -- $(cut -d' ' -f14,15 /proc/$1/stat)
    echo $(( $1 + $2 ))
}
T0=$(f $PID)
sleep 10
T1=$(f $PID)
DIFF=$((T1 - T0))
echo "T0=$T0 T1=$T1 DIFF=$DIFF"
echo "CPU_PCT=$((DIFF / 1000)).$(((DIFF % 1000) / 100))"
echo "PID=$PID"
