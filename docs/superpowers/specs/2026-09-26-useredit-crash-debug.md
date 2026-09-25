# useredit 必死问题排查手册(2026-09-25 深夜,阶段 C 卡点交接)

> 状态:阶段 A(1c497ce)/阶段 B(7b68570)已完成并提交;阶段 C 七页走查
> 被本问题阻塞。本文档是全部证据链与排查路径,接手者按「下一步」顺序执行。

## 1. 现象

- 任何二进制、任何显示模式,登录进菜单→用户管理→**点用户行打开用户编辑页
  (user_edit)时,应用静默消失**(pidof 空)。
- 死亡特征:无 core、无 dmesg segfault、无 libc abort 报错;进程干净消失。
- 复现率:今晚 8/8 轮必死(不同组合,见 §3);页面轨迹均止于
  `open users (depth=2)` 之后、`open user_edit` 前后。
- 唯一一次留下日志的死亡(直通模式首次测试):`[E][MAIN] 主循环 11970ms
  无心跳(渲染/取流卡死),退出交 S60 重拉` → **该次根因已修复**(见 §4),
  修复后直通模式 idle 稳定 1h+。后续死亡无此日志,是**另一个**死法。

## 2. 已排除(全部有对照实验,勿重查)

| # | 嫌疑 | 对照实验 | 结论 |
|---|---|---|---|
| 1 | LVGL9.5 渲染核心回归 | C3 生产版 3ce8bcab(9.5+XRGB,今早七页全过)今晚同死 | 排除 |
| 2 | 阶段 A ARGB 化 | XRGB 模式(不带 DG_UI_PLANE)同死 | 排除 |
| 3 | 阶段 B video plane | DG_UI_PLANE_FORCE_FAIL=1 降级 + 无流 同死 | 排除 |
| 4 | 注入库 v4(ftruncate) | 换回 v3(unlink)同死 | 排除 |
| 5 | 内存泄漏/OOM | RSS 平稳 151MB;dmesg 无 OOM | 排除 |
| 6 | S60 拉起循环假象 | 曾污染观测(ALIVE/DEAD 交替=拉起循环),stop 后仍死 | 已纠偏 |

## 3. 今晚的 8 轮死亡组合(全死在 useredit)

1. 直通+有流(A+B 今日版 md5 7d3f2f8ac6da)
2. 同上(hide 修复版)
3. ARGB+FORCE_FAIL 降级+**无流**(DG_CAM_DEV=/dev/video99)
4. XRGB+无流(今日版)
5. C3 生产版 3ce8bcab+XRGB+无流
6. C3 生产版+**有流**+C3 原版 walk_pages.sh(方式选择 sleep 2 修正)
7. C3 生产版+有流+001 auth_flags 恢复 5 后
8. (第 6/7 轮间)auth_flags=4/5 两态均死

## 4. 已修复的真 bug(与 useredit 死亡无关,但必须知道)

**video commit NONBLOCK 与 v9 驱动 UI flip 排队互撞**:video plane 的
atomic(NONBLOCK)若排在驱动 flip 之前,驱动的下一次 flip commit 返回
EBUSY——失败的 flip 不入队,驱动 flush_wait(lv_refr wait_for_flushing)
的 poll(fd) 永久等不到事件 → 主循环卡死 → main.c 看门狗 10s 判「无心跳」
exit(1)。**修复**:display_video_plane_show/hide 前
lv_linux_drm_wait_flip(lv_linux_drm.c 新增,转发驱动 drm_flush_wait)+
commit 改阻塞模式(不带 NONBLOCK,应用完才返回)。修复后 1h+ 稳定。

## 5. 今早(C3 走查成功)与今晚的唯一已知差异

1. **库内容被改**(头号嫌疑):
   - 手工 INSERT 了用户 10001(walk,管理员,pwd 123456/1230 改过多次,
     avatar=NULL、face_vec=NULL)
   - 手工 UPDATE 001 的 pwd_hash/pwd_salt(重置密码)、auth_flags 短暂改 4
   - 库文件经多次 scp 回写+rm wal/shm
2. 板子软件态:今晚已 reboot -f 复位(重启后待验证)
3. 走查脚本时序:v8 修正版(C3 原版 sleep 8 会超方式选择页 5s 阈值,已知)

## 6. 板上/WSL 当前状态(2026-09-25 深夜)

- 板子已 reboot -f(等回连);**B 槽 /root/dg_app.B = 今日版 7d3f2f8ac6da**
  (含 A+B);备份:`/root/dg_app.B.stageAB`(同款)、
  `/root/dg_app.B.bak0925`(C3 生产版 3ce8bcab)
- **/var/lib/door-guard/door-guard.db = 原始生产库**(7e4505043a0d,001/002,
  与 /root/dg_db_backup_v8/ 一致);reboot 后 S60 会自动拉起应用(XRGB 默认)
- 板上 /tmp 重启已清空:**走查工具需从 WSL /root/dg_walk/ 重传**
  (dg_touch_inject_v4.so、walk_pages2.sh / walk_pages3.sh、perf_cpu2.sh、
  perf_threads2.sh、getshot.sh、raw2png.py、hb/verify/pbtest 等调试程序)
- WSL:/home/olwhistle/doorguard(source env/env.sh);master 已推 A/B 两
  commit;修复后未提交的改动无(阶段 B 已含 hide 修复?)

## 7. 下一步(按序)

1. **等板子上线**(ssh root@192.168.137.130),确认 S60 拉起生产应用正常
   (XRGB、原库 001/002),观察日志 5 分钟无异常。
2. **重传走查工具**到板上 /tmp/dg_walk/。
3. **重建走查用户——禁止再手工 INSERT**。推荐:WSL 写一个宿主小程序链接
   应用同款 storage.c(参照 /root/dg_walk/verify.c 的编译命令:gcc -o x x.c
   -Imodules/sqlite -Iproto -Icomponents/logger modules/sqlite/storage.c
   modules/sqlite/crypto.c proto/valid.c components/logger/dg_log.c -lcrypto
   -lsqlite3 -lpthread),调 storage_init+db_user_add+db_user_set_password
   写原库(10001/walk/管理员/123456/auth_flags=5),scp 回板。这是「应用
   同款写路径」,排除手工 INSERT 的字段问题。
4. **走查**:walk_pages3.sh 结构(10001/123456+方式选择页 sleep 2)。若
   useredit 仍死→进入 5;若过→确认是「手工改库」引入,继续阶段 C(CPU
   三方定案:直通 20% vs 判据;软渲染 28%;v8 20.9%)→阶段 D。
5. **useredit 仍死的抓现场**(按序):
   a. ulimit -c unlimited + `echo /tmp/core.%p > /proc/sys/kernel/core_pattern`
      复现拿 core,交叉 gdb:工具链 aarch64-none-linux-gnu-gdb,
      `gdb door-guard core` → bt。注意 core_pattern 相对路径写 CWD(/root)。
   b. 宿主 sim 复现:WSL 宿主编 door-guard(sim 后端),构造走到 useredit
      (相机 sim 图片循环+宿主库造 001 带头像数据),崩了直接 gdb,应用层
      一测便知。
   c. 重点怀疑:user_edit 页对「头像 avatar blob」「face_vec」的渲染路径
      (dg_jpeg 解码→lv_image);以及被我 UPDATE 过 pwd 字段的行在编辑页
      表单初始化的分支。今早成功时 001 行的 pwd_hash/salt 是原始值,
      今晚被我重置——**若原库+应用路径重建用户后通过,即锁定手工改库**。
6. 排查结论无论哪种,同步 LOG/DEVLOG;阶段 C 的 CPU 定案与七页走查完成后
   按方案 §6 处置 C3(过→[DONE])。

## 8. 纪律提醒(踩过的坑,勿重踩)

- wsl.exe -e bash -c "..." 双引号内**严禁反引号**;含中文/反引号/长文本一律
  Windows 侧 Write 文件再 cp 进 WSL;WSL /tmp 会清,持久文件放 /root/dg_walk/
- 板上 ssh 挂死:pkill 本地 ssh 重试;调试期间 S60 必须 stop(否则拉起循环
  污染观测);部署后 md5 三方核对
- 走查环境变量:DG_WALK_DUMP_DIR=/tmp/dg_walk 才有导图(touch /tmp/dg_shot
  触发);弹窗 5s 计时打开即起(方式选择页 sleep>5=必超时回普通);
  降级软渲染+有流时事件消费 ~285ms/事件,PRESSED>400ms 被判长按吞 CLICKED
  →走查用无流或直通模式
- 注入库 v4:feed 消费改 ftruncate(不再 unlink)——旧版读空即 unlink 与
  writer append 竞态丢键;源码 /root/dg_walk/dg_touch_inject.c,交叉编译:
  aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o x.so x.c -lpthread
  (工具链 /home/olwhistle/dg-toolchain/gcc-arm-10.3-*/bin,
   板端程序加 --sysroot=/home/olwhistle/dg-toolchain/sysroot)
- 文件边界:modules/display/**、modules/camera/**、ui/widgets/dg_preview.c、
  ui/ui.c、third_party/lvgl9/、tests/、docs/、tools/(新增);勿动 services/、
  modules/sqlite、proto/、configs/cur_config.json
- 回退链:DG_UI_PLANE 关=主线;git lvgl9-baseline tag 可回
