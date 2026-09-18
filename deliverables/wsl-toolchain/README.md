# WSL 交叉编译工具链(目标应用开发包)

> 用途:在 WSL/任意 x86_64 Linux 上编译 door-guard(以及任何跑在 K7 板上的 aarch64 程序),
> 不依赖编译 VM。**固件全量编译仍只能在 VM**(Buildroot 体系在 VM)。

## 内容

| 文件 | 解压后 | 说明 |
|---|---|---|
| `gcc-aarch64-10.3.tar.gz` | ~700M,目录 `gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/` | SDK 官方 aarch64 交叉编译器(gcc-arm-10.3,与固件同源),自带基础目标 libc |
| `doorguard-sysroot.tar.gz` | ~300M,顶层即 `usr/` | 目标 sysroot(B4 buildroot):rockiva / lvgl / sqlite3 / gstreamer / rkaiq / rga 等全部头文件+.so,**与板上 .so 版本严格一致**(20260917 曾误打包为纯软链,已重导修复,命令见文末) |

md5sum 见 `md5sums.txt`(先 `md5sum -c md5sums.txt` 再解压;重导 sysroot 后需更新)。

## WSL 上手

```bash
md5sum -c md5sums.txt
mkdir -p ~/dg-toolchain
tar xzf gcc-aarch64-10.3.tar.gz       -C ~/dg-toolchain            # 解出 gcc-arm-10.3-...
mkdir -p ~/dg-toolchain/sysroot
tar xzf doorguard-sysroot.tar.gz      -C ~/dg-toolchain/sysroot    # 顶层即 usr/,别解到 dg-toolchain 根

cd door-guard
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake
cmake --build build -j$(nproc)
```

- 工具链根目录默认 `~/dg-toolchain`,环境变量 `DG_TC_ROOT` 可改
- sysroot 就绪后放到 `~/dg-toolchain/sysroot`(cmake 优先用它);没有时自动退回编译器自带 libc(仅基础程序)

## 部署到板子

```bash
scp build/door-guard root@<板子IP>:/root/
ssh root@<板子IP> /root/door-guard
```

rootfs(B4)已包含基础程序运行所需全部 .so;rockiva/sqlite3/lvgl 等要等 sysroot 重导后编进来的程序才涉及。

## 分工边界(重申)

- **WSL**:door-guard 及一切目标应用的编码、编译、单元测试
- **VM**:固件/内核/rootfs/Buildroot 相关(`./build.sh` 体系)
- 板端调试:dropbear SSH + gdbserver(板上)/ WSL gdb(host-gdb 未进此包,调试走 VM 或后续补)

## 注意

- 目标 glibc 为 buildroot 的 2.38;编译器自带 libc(glibc 2.32)编出的基础程序在板上可正常运行
- **sysroot 重导(在 VM 上;原命令 `tar czf ... staging` 会把符号链接打进包、丢掉全部实体文件,必须归档目录本体)**:

  ```bash
  cd <SDK>/buildroot/output/rockchip_rk3576_kickpi_k7_doorGuard/host/aarch64-buildroot-linux-gnu
  tar czf doorguard-sysroot.tar.gz sysroot
  # 替换 deliverables/wsl-toolchain/ 里的坏包,并更新 md5sums.txt 后 push
  ```

- 若后续 PROJECT_PLAN 升级了固件(加/改包),sysroot 需按上面命令重新导出

## 二进制不进 git(2026-09-18)

gcc-aarch64-10.3.tar.gz / doorguard-sysroot.tar.gz 自 2026-09-18 起不再进 git 跟踪
(.gitignore 排除)。安装走 `env/bin/dg-tc-install`(自动探测本地包);包本体只存
本地 + 局域网 Gitea 历史;md5sums.txt 保留用于校验。
