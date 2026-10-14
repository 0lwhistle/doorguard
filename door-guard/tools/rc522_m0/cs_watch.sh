#!/bin/sh
# 高频采样 CS(gpio-95)/MISO(gpio-19) 电平,输出跳变压缩波形
# 用法:板端在 insmod 前 nohup 后台运行,结束后看输出文件
mount -t debugfs none /sys/kernel/debug 2>/dev/null
prev95=""
prev19=""
c95=0
c19=0
end=$(( $(date +%s) + 12 ))
while [ $(date +%s) -lt $end ]; do
  line=$(grep -E "gpio-(19|95) " /sys/kernel/debug/gpio 2>/dev/null)
  v95=$(echo "$line" | grep gpio-95 | grep -o "hi\|lo" | head -1)
  v19=$(echo "$line" | grep gpio-19 | grep -o "hi\|lo" | head -1)
  if [ "$v95" != "$prev95" ]; then
    if [ -n "$prev95" ]; then
      printf "CS=%s x%d " "$prev95" "$c95"
    fi
    prev95=$v95
    c95=0
  fi
  c95=$((c95+1))
  if [ "$v19" != "$prev19" ]; then
    if [ -n "$prev19" ]; then
      printf "MISO=%s x%d " "$prev19" "$c19"
    fi
    prev19=$v19
    c19=0
  fi
  c19=$((c19+1))
done
printf "CS=%s x%d MISO=%s x%d END\n" "$prev95" "$c95" "$prev19" "$c19"
