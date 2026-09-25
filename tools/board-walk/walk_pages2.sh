#!/bin/sh
# walk_pages2.sh — 直通版七页走查:登录(001/1230)→六子页→残影专项
# 时序要点:001 仅密码方式(auth_flags=4),UID 确认后直进密码弹窗(无方式选择页);
# 弹窗 5s 计时打开即起,键入须全速(发射节拍 200ms/act)
shot() { rm -f /tmp/dg_walk/dg_screen.raw; touch /tmp/dg_shot; i=0; while [ ! -f /tmp/dg_walk/dg_screen.raw ] && [ $i -lt 40 ]; do sleep 0.1; i=$((i+1)); done; mv /tmp/dg_walk/dg_screen.raw /tmp/dg_walk/$1.raw; }
put() { printf '%s\n' "$1" >> /tmp/dg_touch; sleep 0.05; }
key() { put "P $1 $2"; put 'R'; }

# ===== 登录段 =====
printf 'P 360 640\nR\n' >> /tmp/dg_touch
sleep 3
printf 'P 116 1216\nR\n' >> /tmp/dg_touch
sleep 2
printf 'P 604 1216\nR\n' >> /tmp/dg_touch
sleep 0.5
key 354 708
key 354 708
key 152 501
printf 'P 556 708\nR\n' >> /tmp/dg_touch
sleep 2
key 152 501
key 354 501
key 556 501
key 354 708
printf 'P 556 708\nR\n' >> /tmp/dg_touch
sleep 3
shot 03_menu

# ===== 子页走查 =====
key 184 435; sleep 3; shot 04_usermgr
key 360 127; sleep 3; shot 05_useredit
key 614 1216; sleep 3
key 614 1216; sleep 3
key 184 845; sleep 3; shot 06_accessset
key 360 1216; sleep 3
key 536 845; sleep 3; shot 07_logs
key 614 1216; sleep 3
key 536 435; sleep 3; shot 08_device
key 360 564; sleep 3; shot 09_webset
key 360 1216; sleep 3
key 360 1216; sleep 3

# ===== 残影专项:急速往返 =====
key 184 435; sleep 1.5; key 614 1216; sleep 1.5
key 536 845; sleep 1.5; key 360 1216; sleep 1.5
key 536 435; sleep 1.5; key 360 1216; sleep 1.5
key 360 1216; sleep 4
shot 10_home_clean
echo WALK_PAGES2_DONE
