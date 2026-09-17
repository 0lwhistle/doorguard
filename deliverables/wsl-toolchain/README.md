# WSL 交叉编译工具链(目标应用开发包)

> 用途:在 WSL/任意 x86_64 Linux 上编译 door-guard(以及任何跑在 K7 板上的 aarch64 程序),
> 不依赖编译 VM。**固件全量编译仍只能在 VM**(Buildroot 体系在 VM)。

## 内容

| 文件 | 解压后 | 说明 |
|---|---|---|
| `gcc-aarch64-10.3.tar.gz` | ~700M | SDK 官方 aarch64 交叉编译器(gcc-arm-10.3,与固件同源) |
| `doorguard-sysroot.tar.gz` | ~316M | 目标 sysroot(buildroot staging):全部目标库的头文件+.so,含 **rockiva / lvgl / sqlite3 / gstreamer / rkaiq / rga** 等 |

md5sum 见 `md5sums.txt`(先 `md5sum -c md5sums.txt` 再解压)。

## WSL 三步上手

```bash
md5sum -c md5sums.txt
mkdir -p ~/dg-toolchain
tar xzf gcc-aarch64-10.3.tar.gz  -C ~/dg-toolchain     # → ~/dg-toolchain/gcc-aarch64
tar xzf doorguard-sysroot.tar.gz -C ~/dg-toolchain     # → ~/dg-toolchain/sysroot

cd door-guard
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake
cmake --build build -j$(nproc)
```

- 工具链根目录默认 `~/dg-toolchain`,环境变量 `DG_TC_ROOT` 可改
- sysroot 就是这版固件(B4)的 staging,**与板上 .so 版本严格一致**,不存在"我这边编过、板上缺库"

## 部署到板子

```bash
scp build/door-guard root@<板子IP>:/usr/bin/
```

rootfs(B4)已包含二进制运行所需的全部 .so(rockiva/sqlite3/lvgl 等),推上去就能跑。

## 分工边界(重申)

- **WSL**:door-guard 及一切目标应用的编码、编译、单元测试
- **VM**:固件/内核/rootfs/Buildroot 相关(`./build.sh` 体系)
- 板端调试:dropbear SSH + gdbserver(板上)/ WSL gdb(host-gdb 未进此包,调试走 VM 或后续补)

## 注意

- 目标 glibc 为 buildroot 的 2.38;WSL 编译时链接的是 sysroot 里的库,与板一致
- 若后续 PROJECT_PLAN 升级了固件(加/改包),**sysroot 需要重新导出**(在 VM 上重打
  `doorguard-sysroot.tar.gz`,命令:`tar czf doorguard-sysroot.tar.gz -C <SDK>/buildroot/output/rockchip_rk3576_kickpi_k7_doorGuard staging`)
