#!/bin/sh
# walk_login.sh v7 — 菜单按钮→管理员认证→验证(10001/123456)→进菜单→用户管理
shot() { rm -f /tmp/dg_walk/dg_screen.raw; touch /tmp/dg_shot; i=0; while [ ! -f /tmp/dg_walk/dg_screen.raw ] && [ $i -lt 40 ]; do sleep 0.1; i=$((i+1)); done; mv /tmp/dg_walk/dg_screen.raw /tmp/dg_walk/$1.raw; }

printf 'P 360 640\nR\n' >> /tmp/dg_touch          # 唤醒
sleep 12                                          # 等预览 fps 稳定
printf 'P 116 1216\nR\n' >> /tmp/dg_touch         # 菜单按钮 → 管理员认证模式(5s 窗口)
sleep 0.8
printf 'P 604 1216\nR\n' >> /tmp/dg_touch         # 验证 → UID 弹窗(~0.6s 后开)
sleep 0.9
printf 'P 152 501\nR\n' >> /tmp/dg_touch          # 1
printf 'P 354 708\nR\nP 354 708\nR\nP 354 708\nR\n' >> /tmp/dg_touch  # 000
printf 'P 152 501\nR\n' >> /tmp/dg_touch          # 1
printf 'P 556 708\nR\n' >> /tmp/dg_touch          # 确认 → 方式选择
sleep 8
printf 'P 360 656\nR\n' >> /tmp/dg_touch          # 密码 → 密码弹窗
sleep 0.9
printf 'P 152 501\nR\nP 354 501\nR\nP 556 501\nR\n' >> /tmp/dg_touch  # 123
printf 'P 152 570\nR\nP 354 570\nR\nP 556 570\nR\n' >> /tmp/dg_touch  # 456
printf 'P 556 708\nR\n' >> /tmp/dg_touch          # 确认 → 管理员验证通过,直接进菜单
sleep 6
shot 03_menu
printf 'P 184 435\nR\n' >> /tmp/dg_touch          # 用户管理宫格
sleep 4
shot 04_usermgr
echo WALK_LOGIN_DONE
