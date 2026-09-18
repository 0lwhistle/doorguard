# 下次开工提示词(直接复制使用)

## 主提示词(每次开工用这条,按进度改目标行)

```
继续 RK3576 K7 人脸识别门禁项目。

先读 /home/olwhistle/Linux/rk3576/k7_rk3576/PROJECT_PLAN.md 恢复上下文(含进度快照、
架构、编译计划、工程纪律),严格按文档执行,不要重新调研已定结论。

关键路径:
- 官方 SDK:/home/olwhistle/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008
  (已解压,git fsck 已通过,HEAD=f06400239,不要重复解压;SDK 内已有分支
  k7-door-guard-dev,包含 K7 门禁定制 Buildroot 配置)
- 板型:K7;设备 defconfig:rockchip_rk3576_kickpi_k7_buildroot_defconfig;
  Buildroot 配置:rockchip_rk3576_kickpi_k7_doorGuard_defconfig
- 硬件:5寸屏 F050008M01(720x1280,GT9xx触摸)+ IMX415 摄像头

本次目标:【从 PROJECT_PLAN.md 第五节"固件编译计划"的下一个未完成 Phase 继续,
首次开工填:Phase B1 环境自检 + B2 Buildroot 定制配置生效 + B3 屏幕使能】

纪律提醒:产物 >1GB 必须 md5 连读两次一致;make -j6;SDK 内改动走 git 分支。
完成后更新 PROJECT_PLAN.md 的进度快照表。
```

## 分阶段补充提示词(需要单项推进时替换"本次目标")

### Phase B1+B2+B3(首次编译前准备)
```
本次目标:Phase B1 环境自检、B2 Buildroot 定制配置生效、B3 屏幕使能。
B2 要点:SDK 分支 k7-door-guard-dev 已含
buildroot/configs/rockchip_rk3576_kickpi_k7_doorGuard_defconfig
(去 weston/chromium,含 lvgl+DRM、gdb/strace、dropbear、RKADK+AIQ)。
任务:1) 确认 RK_BUILDROOT_CFG 如何绑定到该配置(build.sh 菜单或环境变量);
2) 执行一次 buildroot 构建验证配置可用(缺包按报错补);
3) 把过程中的 SDK 变更提交到 k7-door-guard-dev 分支并 format-patch 进仓库。
B3 要点:kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-linux.dts
增加 #include "rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi"(先确认
该 dtsi 的节点和 K7 的 io 复用无冲突),同样提交到 k7-door-guard-dev 并导出补丁。
```

### Phase B4(全量编译)
```
本次目标:Phase B4 全量编译 Buildroot 无桌面固件。用 ./build.sh 交互选
kickpi_k7_buildroot 配置或直接命令行;长时间任务放后台跑,期间每 10 分钟汇报
进度;失败先读日志定位,不要盲目重跑。产物镜像 md5 双读一致后列出
output/ 下全部产物路径和大小。
```

### Phase B5(烧录验证)
```
本次目标:Phase B5 烧录验证。指导我用 USB 烧录(给出具体工具和步骤,
Maskrom 进法),串口参数 1500000 8N1。烧录后按 PROJECT_PLAN.md B5 验收
清单逐项检查,把每项的实测输出贴给我。
```

### Phase B6~B9(逐项)
```
本次目标:Phase B【6/7/8/9】。涉及取流/NPU/应用的开发,先按 PROJECT_PLAN.md
第三、四节的架构和模块约定搭骨架,给我看目录和接口设计再写实现。
人脸识别用官方 ROCKIVA(external/iva,模型自带无需转换,API 见
librockiva/rockiva-rk3576-Linux/include,指南在 sdk-guide/docs 的 ROCKIVA PDF);
仅当需要自定义模型时才用 rknn-toolkit2(PC 端 pip 安装,注意 python 版本)。
```

## 备注
- 若会话中又要传大文件:一律流式(tar 管道),禁止"拷包再解压"。
- 若出现任何文件损坏异常:先 `sudo memtester 4G 1` 查内存,回报结果。
