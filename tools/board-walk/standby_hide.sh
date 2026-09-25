#!/bin/sh
# standby_hide.sh — 主页直通→待机 hide(黑屏)→唤醒恢复 三态取证
shot() { rm -f /tmp/dg_walk/dg_screen.raw; touch /tmp/dg_shot; i=0; while [ ! -f /tmp/dg_walk/dg_screen.raw ] && [ $i -lt 40 ]; do sleep 0.1; i=$((i+1)); done; mv /tmp/dg_walk/dg_screen.raw /tmp/dg_walk/$1.raw; }
printf 'P 360 640\nR\n' >> /tmp/dg_touch
sleep 4
shot h1_home
sleep 32
shot h2_standby
printf 'P 360 640\nR\n' >> /tmp/dg_touch
sleep 4
shot h3_home_back
echo HIDE_DONE
