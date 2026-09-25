#!/bin/bash
# getshot.sh <名字> [前置等待秒] —— 板上触发一次性导图,取回转 PNG 到 Windows 侧
NAME=$1
WAIT=${2:-0}
sleep "$WAIT"
ssh root@192.168.137.130 "rm -f /tmp/dg_walk/dg_screen.raw; touch /tmp/dg_shot; i=0; while [ ! -f /tmp/dg_walk/dg_screen.raw ] && [ \$i -lt 40 ]; do sleep 0.1; i=\$((i+1)); done; mv /tmp/dg_walk/dg_screen.raw /tmp/dg_walk/$NAME.raw" || { echo "BOARD_FAIL"; exit 1; }
scp -q root@192.168.137.130:/tmp/dg_walk/$NAME.raw /root/dg_walk/$NAME.raw || { echo "SCP_FAIL"; exit 1; }
python3 /root/dg_walk/raw2png.py /root/dg_walk/$NAME.raw /root/dg_walk/$NAME.bmp
convert /root/dg_walk/$NAME.bmp "/mnt/c/Users/86151/Desktop/doorguard/tmp_walk/$NAME.png" 2>/dev/null || cp /root/dg_walk/$NAME.bmp "/mnt/c/Users/86151/Desktop/doorguard/tmp_walk/$NAME.bmp"
echo "DONE $NAME"
