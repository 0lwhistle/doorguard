# modules/sysctl — 重启/停机原语

对 libc `reboot(RB_AUTOBOOT)`/`sync` 的薄封装:统一「重启设备」入口,
web `/api/system/reboot`、设备管理页、看门狗终态均经此。

- `sysctl_reboot()`:sync 后重启(唯一调用方 sysctl_service.c);
- 语义:同步调用,不回调、不发事件(重启即进程终止)。

注:`services/vision` 与 rkaiq 有同名系统头,与本模块无关(复审更正)。
测试:宿主不真重启,`test_sysctl` 验证装配与降级路径。
