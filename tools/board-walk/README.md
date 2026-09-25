# board-walk — 板端走查与取证工具集(K7 门禁,RK3576)

LVGL 真机走查/取证/性能测量的工具集。源起 C3 阶段(LVGL9 迁移板端验收),
v4 注入库与部分脚本为 video plane 直通重启(2026-09-25/26)迭代。

## 环境

- 板端:RK3576 K7,SSH root@<板IP>(网线直插笔记本 ICS,IP 由 DHCP 分配,
  **重启后可能变化,以 `ip addr` 实际为准**);屏幕 720x1280(XRGB/ARGB8888,
  stride=行宽×4)。
- 宿主:WSL(工具链 `~/dg-toolchain`,env 见仓库 `env/env.sh`);持久文件放
  `/root/dg_walk/`(WSL 与板上 `/tmp` 都会在 VM/板重启后清空)。
- 应用侧取证:启动时带 `DG_WALK_DUMP_DIR=<dir>`,之后 `touch /tmp/dg_shot`
  触发单帧导出 `<dir>/dg_screen.raw`(720x1280 4B/px,自上而下)。

## 触摸注入(LD_PRELOAD)

```sh
aarch64-none-linux-gnu-gcc --sysroot=$SYSROOT -shared -fPIC -O2 \
    -o dg_touch_inject_v4.so dg_touch_inject.c -lpthread
# 板上启动:
LD_PRELOAD=/tmp/dg_walk/dg_touch_inject_v4.so /root/door-guard
```

- 命令协议:向 `/tmp/dg_touch` 追加写 `P x y` / `R`(每条一行,屏幕域
  720x1280)。**必须追加写**;按下帧 4 事件(TRACKING_ID/X/Y/SYN),抬起 2 事件。
- 时序:注入库内部节拍 ~200ms/动作(队列化);**弹窗类 UI 有 5s 计时(打开即
  起)**,键入须全速(脚本内 `put()` 后仅 sleep 0.05)。
- **v4 修复**:命令文件消费改 `ftruncate`(旧 v3 读空即 unlink,与 writer 的
  append 竞态丢键——板上实测 UID 第 2 键起全丢)。
- 已知交互:**降级软渲染+有流时事件消费 ~285ms/事件**,PRESSED>400ms 被判
  长按吞 CLICKED → 触摸走查用「无流」(`DG_CAM_DEV=/dev/video99`)或
  plane 直通模式。

## 脚本

| 脚本 | 用途 |
|---|---|
| `walk_login.sh` | 登录链走查(v8 时序:菜单→验证→UID→方式选择 sleep 2→密码) |
| `walk_pages2.sh` | 七页走查(auth_flags=4 仅密码用户,无方式选择页) |
| `walk_pages3.sh` | 七页走查+残影专项(auth_flags=5 多方式,推荐) |
| `standby_hide.sh` | 待机三态取证(主页直通→待机黑屏→唤醒恢复) |
| `walk_ghost.sh` | 残影专项(C3 版,急速往返) |
| `getshot.sh` | 板上触发导图并取回转 PNG |
| `raw2png.py` | RAW→bottom-up BMP(pillow/convert 转 PNG) |
| `perf_cpu2.sh` | 整机 CPU(/proc/pid/stat utime+stime 增量/10s,单核%) |
| `perf_threads2.sh` | 线程级 CPU 分解(TID 对齐;busybox awk 下绝对值粗略) |
| `perf_cpu.sh`/`perf_switch.sh` | C3 遗留(切页响应注入+md5 轮询法) |

## 宿主调试程序(链接应用同款 modules/sqlite)

```sh
gcc -o <out> <src>.c -Imodules/sqlite -Iproto -Icomponents/logger \
    modules/sqlite/storage.c modules/sqlite/crypto.c proto/valid.c \
    components/logger/dg_log.c -lcrypto -lsqlite3 -lpthread
```

| 程序 | 用途 |
|---|---|
| `useradd.c` | **走查用户重建(推荐路径)**:应用同款 storage 写库,勿手工 INSERT |
| `verify.c` | 库/密码离线校验(db_user_get+db_verify_password) |
| `pbtest.c` | PBKDF2 固定向量对拍(板端跑,RFC 7914 §11) |
| `hb.c` | 宿主 PBKDF2 对拍(与 python hashlib 等价性确认) |
| `dg_touch_inject.c` | 注入库源码(v4,见上) |

## 走查坐标表(板上实测,720x1280)

主页 菜单=(116,1216) 验证=(604,1216);UID 键盘 1=(152,501) 0=(354,708)
确认=(556,708);密码键盘 123=行 y501(152/354/556)、456=行 y570;
菜单宫格 用户管理=(184,435) 设备=(536,435) 门禁设置=(184,845) 记录=(536,845)
菜单返回=(360,1216);右下返回型页(users/logs/user_edit)=(614,1216);
底中返回型页(access_set/device/web_set)=(360,1216);设备页 Web 管理=(360,564);
用户管理行=(360,127)。
