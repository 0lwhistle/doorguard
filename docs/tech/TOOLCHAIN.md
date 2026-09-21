# 交叉编译环境(WSL)技术文档

> 面向:在 WSL 上开发 door-guard 及一切跑在 K7 板上的 aarch64 程序的人。
> 快速上手看 `deliverables/wsl-toolchain/README.md`;本文讲构成、原理与坑。

## 1. 组成

| 部件 | 版本/来源 | 位置(`~/dg-toolchain/`) | 作用 |
|---|---|---|---|
| 交叉编译器 | gcc-arm-10.3-2021.07(`aarch64-none-linux-gnu`,Arm 官方) | `gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/` | 编译/链接;**自带一份基础目标 libc(glibc 2.32)** |
| doorguard sysroot | B4 buildroot staging(glibc **2.38**) | `sysroot/` | 板载全部库的头文件+.so:rockiva / lvgl / sqlite3 / gstreamer / rkaiq / rga 等 |

- 二者都从仓库 `deliverables/wsl-toolchain/` 解压,`env/bin/dg-tc-install` 一键完成(md5 校验 → 解压)
- 编译器自带 libc 与 sysroot 的关系:**没有 sysroot 时只能编基础程序**(stdio 等),
  链接板载库必须用 sysroot;自带 libc(2.32)编出的程序在板上 glibc 2.38 运行向下兼容,
  反过来(用 2.38 头文件编、依赖新符号)不成立,所以正式程序一律走 sysroot

## 2. 为什么不用 SDK 的 prebuilts 工具链

SDK `prebuilts/toolschain/` 的工具链主要为内核/U-Boot 服务;应用开发用 buildroot
生成的 staging 做 sysroot,保证 **libc 版本、库版本与板上 rootfs 严格一致**,
不存在"我这边编过、板上缺库/符号不对"。

## 3. 版本配套关系(重要)

**sysroot ↔ 固件一一对应**。sysroot 就是某版固件 rootfs 的 staging:

- 当前 sysroot 对应 **20260917-B4**
- 以后固件升级(加/改了包,如新增库)**必须重导 sysroot**,否则 WSL 编译用的头文件/库
  与板上不一致,出现编译过、运行缺符号

重导命令(在 **VM** 上;注意 `staging` 是绝对路径软链,直接打包只含链接本身,必须 `-C` 进实体目录):

```bash
cd <SDK>/buildroot/output/rockchip_rk3576_kickpi_k7_doorGuard/host/aarch64-buildroot-linux-gnu
tar czf doorguard-sysroot.tar.gz sysroot
# 放回仓库 deliverables/wsl-toolchain/,更新 md5sums.txt,push;WSL 侧重跑 dg-tc-install
```

## 4. cmake 工具链文件(`door-guard/cmake/aarch64.cmake`)

自动探测,无需手工改路径:

1. 工具链根目录:`DG_TC_ROOT` 环境变量 → `~/dg-toolchain`(env.sh 已自动设置)
2. gcc 目录:glob 匹配 `gcc-arm-*aarch64-none-linux-gnu`(目录名带版本号,勿写死)
3. sysroot 回退链:`~/dg-toolchain/sysroot`(正式)→ `staging`(兼容)→ 编译器自带 libc(仅基础程序,configure 时会打印提示)

`CMAKE_FIND_ROOT_PATH_MODE_{PROGRAM,LIBRARY,INCLUDE,PACKAGE}` 按 cross 标准配置,
保证头文件/库只从 sysroot 找、不误用 WSL 宿主机的 x86_64 内容。

## 5. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `Cannot find -lxxx` / 头文件找不到 | sysroot 里没有该库(固件没编进去) | 回 VM 在 doorGuard buildroot 配置加包 → 重编固件 → 重导 sysroot |
| configure 打印"暂用编译器自带 libc" | sysroot 未安装/路径不对 | 跑 `dg-tc-install`;确认 `~/dg-toolchain/sysroot/usr/include` 存在 |
| 板上运行 `No such file or directory`(文件明明在) | 缺动态加载器/库架构不对(编成 x86_64) | `file` 确认 aarch64;确认用了工具链文件而不是宿主 gcc |
| 板上 `version 'GLIBC_2.3x' not found` | 用了比板新的 sysroot 编译 | sysroot 与固件版本配套(见 §3),重导对应版本的 sysroot |
| 板上 `libjpeg.so.62: cannot open shared object file`(rc=127) | sysroot 里混进了两套 libjpeg:dev 名 `usr/lib{,64}/libjpeg.so` 实为旧 IJG 6b(SONAME .62),板上运行库只有 turbo 的 `libjpeg.so.8` | **别用 find_package(JPEG)/`-ljpeg`**(会选中 .62);项目已显式链 `${CMAKE_SYSROOT}/usr/lib/libjpeg.so.8`(见 CMakeLists dg_jpeg 段,2026-09-21) |
| 想在 WSL 直接试跑 aarch64 程序 | — | `sudo apt install qemu-user-static`,然后 `qemu-aarch64 -L ~/dg-toolchain/sysroot ./build/door-guard`(可选,非必需) |

## 6. 分工边界

- **WSL**:door-guard 编码、编译、单元测试(dg-build / dg-deploy)
- **VM**:固件/内核/rootfs/Buildroot(`./build.sh` 体系,见 DEV_HANDBOOK §6)
- 固件升级后:VM 重导 sysroot → push → WSL `git pull && dg-tc-install`
