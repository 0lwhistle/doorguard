#!/bin/sh
# perf_threads2.sh — TID 对齐的线程级 CPU 分解
P=$(pidof door-guard)
[ -n "$P" ] || { echo NO_PROC; exit 1; }
for T in /proc/$P/task/*; do
  tid=${T##*/}
  printf '%s %s\n' "$tid" "$(awk '{print $14+$15}' $T/stat)"
done > /tmp/ta.$$
sleep 10
for T in /proc/$P/task/*; do
  tid=${T##*/}
  printf '%s %s\n' "$tid" "$(awk '{print $14+$15}' $T/stat)"
done > /tmp/tb.$$
awk 'NR==FNR { a[$1]=$2; next } { d=$2-a[$1]; if (d>3) printf "tid %s: +%d%%\n", $1, d }' /tmp/ta.$$ /tmp/tb.$$
rm -f /tmp/ta.$$ /tmp/tb.$$
