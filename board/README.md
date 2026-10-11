# board/ — 板端部署物(不入固件的运行期文件)

`restore-app.sh` — **重刷 / 裸板后一键恢复应用层**(2026-10-11 新增,幂等):

```bash
# 在 WSL 侧运行(构建产物与检测模型都在 WSL)
bash board/restore-app.sh [板IP]      # 缺省 192.168.137.100
```

应用层**不烘进固件**,所以重刷 rootfs 后板上只剩 SDK 基础镜像:没有 S60doorguard、
没有槽位、没有 `/etc/door-guard`、没有 `/userdata/doorguard/{models,cur_config.json}`。
此时 **`dg-deploy` 用不了**(它假定应用已装、只做已装应用的 A/B 换槽),必须先用本脚本
种下首次安装;之后日常改代码再走 `dg-deploy`。脚本做五件事:推 S60 + 语言包 +
`default.json` + 4 个模型 → 禁 `S00-lv_demo`(否则它占住屏幕)→ 把二进制装进**非活动槽**
并原子切链(直写活动槽会 `Text file busy`)→ md5 核对 → stop/start 拉起。

模型来源(二进制硬编码这四个名字):检测 `RetinaFace_rk3576_i8.rknn`、识别
`w600k_r50.rknn`、反欺骗 `MiniFASNet ×2`。**`RetinaFace_rk3576_i8.rknn` 不在 git**,
缺省从 WSL `/root/reinface/out/` 取(可用 `DG_DET_DIR` 等环境变量覆盖,见脚本头)。

`rootfs-overlay/` — 将合并进 buildroot rootfs overlay 的文件(VM SDK 侧 overlay
目录同步一份,下一版固件自带;当前经 dg-deploy 直接推板即时生效):

```
rootfs-overlay/
└── etc/init.d/S60doorguard   开机自启 + 监督循环(崩 3s 拉起)+ OTA A/B 监听
```

- **应用升级走 OTA A/B 槽位**(2026-09-26 起):`dg-deploy`(开发)与 web 上传
  (生产)都把新包放进暂存区 `/var/lib/door-guard/ota_staged.bin(+.sha256/.ver)`,
  S60 的 ota_watch 统一消费:sha 复核 → 装**非活动槽** → symlink 原子切换 →
  重启;新包连续 3 次秒退(<15s)自动回滚旧槽
- 槽位:`/root/dg_app.A` / `/root/dg_app.B`,`/root/door-guard` 为指向活动槽的
  符号链接(切换原子;不要直写符号链接——绕过 A/B 防护)
- 语言包 `/root/ui/lang/` 由 `dg-deploy` 推送(资源文件不走槽位)
- 运行态目录 `/var/lib/door-guard/`(db/key/ota_staged)由脚本自动创建
- 配置:出厂模板 `/etc/door-guard/default.json`(缺省走代码默认);现用配置 `/userdata/doorguard/cur_config.json`(首启自动生成,升级不丢)
- 日志 `/var/log/door-guard.log`(OTA 安装/回滚记录也在这里)
- **注意**:本版 rootfs 的 dropbear 主机钥未持久化,板子重启后 key 会变,
  开发机需 `ssh-keygen -R <板IP>` 后重连(固件侧持久化待办)
- 触摸轴向校准:在 S60doorguard 的启动循环前 export `DG_TOUCH_SWAP_XY` /
  `DG_TOUCH_INVERT_X` / `DG_TOUCH_INVERT_Y`(见 door-guard/modules/display/README.md)
