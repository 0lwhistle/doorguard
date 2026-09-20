# board/ — 板端部署物(不入固件的运行期文件)

`rootfs-overlay/` — 将合并进 buildroot rootfs overlay 的文件(VM SDK 侧 overlay
目录同步一份,下一版固件自带;当前经 dg-deploy 直接推板即时生效):

```
rootfs-overlay/
└── etc/init.d/S60doorguard   开机自启 + 崩溃 3s 拉起(SysV rcS,S60 序号)
```

- 应用本体 `/root/door-guard` 与语言包 `/root/ui/lang/` 由 `dg-deploy` 推送
- 运行态目录 `/var/lib/door-guard/`(db/key)由脚本自动创建
- 配置:出厂模板 `/etc/door-guard/default.json`(缺省走代码默认);现用配置 `/userdata/doorguard/cur_config.json`(首启自动生成,升级不丢)
- 日志 `/var/log/door-guard.log`
- 触摸轴向校准:在 S60doorguard 的启动循环前 export `DG_TOUCH_SWAP_XY` /
  `DG_TOUCH_INVERT_X` / `DG_TOUCH_INVERT_Y`(见 door-guard/modules/display/README.md)
