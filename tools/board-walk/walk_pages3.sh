#!/bin/sh
# walk_pages3.sh — C3 原版结构+登录时序修正(方式选择 sleep 2,v7 的 sleep 8 超 5s 阈值)
# 10001/123456 auth_flags=5(有方式选择页)+有流模式
shot() { rm -f /tmp/dg_walk/dg_screen.raw; touch /tmp/dg_shot; i=0; while [ ! -f /tmp/dg_walk/dg_screen.raw ] && [ $i -lt 40 ]; do sleep 0.1; i=$((i+1)); done; mv /tmp/dg_walk/dg_screen.raw /tmp/dg_walk/$1.raw; }
tap() { printf "P $1 $2\nR\n" >> /tmp/dg_touch; }

printf 'P 360 640\nR\n' >> /tmp/dg_touch
sleep 12
tap 116 1216
sleep 0.8
tap 604 1216
sleep 0.9
tap 152 501; tap 354 708; tap 354 708; tap 354 708; tap 152 501; tap 556 708
sleep 2
tap 360 656
sleep 0.9
tap 152 501; tap 354 501; tap 556 501; tap 152 570; tap 354 570; tap 556 570; tap 556 708
sleep 6
# ===== 子页走查 =====
tap 184 435; sleep 3; shot 04_usermgr
tap 360 127; sleep 3; shot 05_useredit
tap 614 1216; sleep 3
tap 614 1216; sleep 3
tap 184 845; sleep 3; shot 06_accessset
tap 360 1216; sleep 3
tap 536 845; sleep 3; shot 07_logs
tap 614 1216; sleep 3
tap 536 435; sleep 3; shot 08_device
tap 360 564; sleep 3; shot 09_webset
tap 360 1216; sleep 3
tap 360 1216; sleep 3
# ===== 残影专项 =====
tap 184 435; sleep 1.5; tap 614 1216; sleep 1.5
tap 536 845; sleep 1.5; tap 360 1216; sleep 1.5
tap 536 435; sleep 1.5; tap 360 1216; sleep 1.5
tap 360 1216; sleep 4
shot 10_home_clean
echo WALK_PAGES3_DONE
