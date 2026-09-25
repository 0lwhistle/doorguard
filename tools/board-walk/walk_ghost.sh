#!/bin/sh
# walk_ghost.sh — 残影专项(修正坐标版):四组急速往返 + logs 复查 + 主页终帧
shot() { rm -f /tmp/dg_walk/dg_screen.raw; touch /tmp/dg_shot; i=0; while [ ! -f /tmp/dg_walk/dg_screen.raw ] && [ $i -lt 40 ]; do sleep 0.1; i=$((i+1)); done; mv /tmp/dg_walk/dg_screen.raw /tmp/dg_walk/$1.raw; }
tap() { printf "P $1 $2\nR\n" >> /tmp/dg_touch; }

printf 'P 360 640\nR\n' >> /tmp/dg_touch
sleep 12
tap 116 1216
sleep 0.8
tap 604 1216
sleep 0.9
tap 152 501; tap 354 708; tap 354 708; tap 354 708; tap 152 501; tap 556 708
sleep 8
tap 360 656
sleep 0.9
tap 152 501; tap 354 501; tap 556 501; tap 152 570; tap 354 570; tap 556 570; tap 556 708
sleep 6
# ===== 残影急速往返(坐标按各页实际返回位置)=====
tap 184 435; sleep 1.5; tap 614 1216; sleep 1.5
tap 536 845; sleep 1.5; tap 614 1216; sleep 1.5; shot 07b_logs2
tap 536 435; sleep 1.5; tap 360 1216; sleep 1.5
tap 184 845; sleep 1.5; tap 360 1216; sleep 1.5
tap 360 1216; sleep 4
shot 11_home_clean2
echo GHOST_DONE
