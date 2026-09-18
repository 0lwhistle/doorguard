# OTA A/B 分区方案(方案先行,不改固件)

> 状态:**应用级 A/B 已实施**(2026-09-18,见下"已实施方案");系统固件级
> A/B(分区表/uboot)仍为方案,需要时再排期。
> 应用侧:HTTP 流式收包 + sha256 校验 + 持久区暂存 + sidecar,由 S60 bash 完成
> 监听/复核/槽位切换/自动重启/秒退回滚。

## 已实施方案(2026-09-18,应用级 A/B)

- **槽位**:/root/dg_app.A 与 /root/dg_app.B;/root/door-guard 为指向当前槽的
  符号链接(所有启动路径与 dg-deploy 都只认 /root/door-guard)
- **流程**:web /api/ota/upload(sha256/大小校验)→ 暂存
  /var/lib/door-guard/ota_staged.bin + sidecar(.sha256/.ver)→ S60 监听循环
  (2s 轮询,收包完成判定=大小 2s 不变)→ sha 复核 → 拷入**非活动槽** →
  符号链接原子切换 → killall 触发监督循环拉起新版本
- **回滚**:新版本 15s 内退出且连续 3 次 → 自动切回另一槽;exec 失败(126/127)
  跳过 3A 服务重启(快速循环反复重启 aiq 曾致 ISP 内核崩溃,实测)
- **上传工具**:`dg-ota-upload <IP> <包> [版本]`
- **实测**:有效包切换 A↔B 正常运行;坏包秒退 3 次后回滚成功;槽位化在
  重启后自愈(读符号链接决定启动槽)
- 已知边界:坏包切换过程若恰逢快速重启循环,ISP 驱动可能内核崩溃一次,
  板上看门狗会重启自愈(固件层问题,后续在 SDK 侧排查)

## 现状与目标

- 现状:B4 固件单一 rootfs 分区(grow),无 A/B、无 recovery。
- 目标:升级失败可回退(断电安全),主机侧脚本一键完成。

## 方案 A(推荐):应用级 A/B

```
分区规划(在 parameter.txt 中新增,均 ≥0x4000 扇区):
  app_a  <地址A> <大小>   door-guard 应用 + 依赖库(只读 squashfs 或 ext4)
  app_b  <地址B> <大小>   同上(升级写入目标)

rootfs 不动:系统/库升级走 B10 buildroot 重刷(低频)。
door-guard 及其私有依赖打包为 app 镜像(高频升级)。
```

uboot 环境变量约定:

```
boot_app        a|b            当前启动槽
bootcount       N              启动计数(每次 boot_app 启动 +1)
upgrade_ok      0|1            新槽应用上报成功标志
altbootcmd      ...            bootcount>3 且 upgrade_ok!=1 → 切回另一槽
```

启动脚本(应用侧,systemd/init):
1. 应用启动并自检(数据库可开、服务全部 READY)→ `fw_setenv upgrade_ok 1`
2. uboot:bootcount 超 3 次仍无 upgrade_ok=1 → 切换 boot_app 回滚

## 升级流程(与已实现代码的对应)

```
主机: dg-ota-upload <板IP> <包>
  ① HEAD 探测(可选)
  ② POST /api/ota/upload
     头:X-Auth-Token / X-OTA-Version / X-OTA-Size / X-OTA-SHA256 / X-OTA-Offset(续传)
     体:包二进制(流式,不落内存)
板:  ota_begin   → 大小预检(超 64MB 直接 400)+ 续传偏移对拍(409)
     ota_write_chunk → /tmp/ota_staging.part 流式落盘
     ota_finish  → sha256(EVP 流式)不符 → 422 拒收;
                   通过 → 改名 /tmp/ota_staged.bin(校验闭环)
  ③ (方案 A 落地后)staged → dd 写 app_b → fw_setenv boot_app=b,
     bootcount=0, upgrade_ok=0 → 重启 → 新应用上报 upgrade_ok=1
```

## 已实现/待实现对照

| 项 | 状态 |
|---|---|
| HTTP 流式收包(内存占用 O(8KB)) | ✅ ota_service |
| 大小预检(超限开始前拒绝) | ✅ |
| sha256 校验(不符 422) | ✅ |
| 断点续传(X-OTA-Offset) | ✅ |
| 写分区 | ⏳ 方案 A 分区落地后(ota_finish 后接 dd/fw_setenv) |
| uboot 回退 | ⏳ uboot 脚本与分区方案同步落地 |
| 断电中途:写坏 app_b,A 启动不受影响 | 设计保证(只写 B 槽) |

## 风险与注意

- 写分区期间断电:只影响 B 槽,A 槽仍可启动(方案 A 核心价值)
- /tmp 为 tmpfs:staged 文件重启即失,落地后改为直接流写 app_b
- 升级包制作:tar 打包 door-guard + 依赖清单,签名机制(后续可加 ed25519)
