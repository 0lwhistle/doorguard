# k7_rk3576 — RK3576 K7 人脸识别门禁项目仓库

 KickPi K7(RK3576)+ 5寸MIPI屏(F050008M01)+ IMX415 摄像头,LVGL + NPU 人脸识别门禁。

## 仓库内容(刻意最小化,官方 SDK 19GB 不进 git)

```
├── PROJECT_PLAN.md          项目方案与执行手册(唯一事实来源,每阶段更新)
├── docs/                    开发文档
│   ├── DEV_HANDBOOK.md      软件开发手册(硬件事实/编译环境/踩坑索引)
│   ├── DEVLOG.md            开发日志(过程与坑,按日追加)
│   ├── prompts/             会话开工提示词(NEXT_SESSION / IDLE_TASK)
│   └── tech/                技术文档(FLASHING 烧录 / TOOLCHAIN 交叉编译)
├── env/                     WSL 环境与脚本(source env/env.sh 后 dg-* 直接可用)
├── door-guard/              门禁应用源码(WSL 编码编译 / VM 兜底)
├── deliverables/            产物:固件包(firmware/)+ WSL 工具链包(wsl-toolchain/)
│                            (仅 README/md5 等文本进 git;镜像/工具链 tar 本地保留)
├── sdk-guide/               官方 SDK 开发资源指南(必读) + 9 份精选官方文档 PDF
└── sdk-patches/             对官方 SDK 的全部修改,以 git patch 管理
```

## SDK 修改管理约定

官方 SDK(19GB)只存在于编译 VM:`~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008`
(git 仓库,HEAD=f06400239,origin 为厂家内网 gitea,不可达仅作记录)。

修改流程:
1. VM 的 SDK 里建分支 `k7-door-guard-dev`,所有修改(DTS/rootfs脚本等)提交在该分支
2. 每次修改后导出补丁进本仓库:
   `git format-patch <起点> -o /path/to/k7_rk3576/sdk-patches/`
3. 换机/重建 VM 时:原版 SDK + `git am sdk-patches/*.patch` 即可复原

## 开发工作流(WSL 开发 + VM 编译)

- 代码流转只经本仓库 push/pull,不复制目录
- **WSL**:door-guard 编码→编译→部署:`source env/env.sh` 后 `dg-build` / `dg-deploy`;
  纯逻辑单元测试也在 WSL;工具链见 `docs/tech/TOOLCHAIN.md`
- **VM**:固件/内核/rootfs 全量编译(SDK `./build.sh` 体系,见 docs/DEV_HANDBOOK.md §6)
- 远程仓库:
  - **origin** = `git@github.com:0lwhistle/doorguard.git`(GitHub,源码主仓;
    大于 100MB 的产物二进制被 .gitignore 排除,GitHub 历史中不含固件镜像)
  - **gitea** = `ssh://git@192.168.2.150:222/olwhistle/k7_rk3576.git`(局域网备份,
    保留含固件/工具链大文件的完整历史;2026-09-18 目录整理后与 origin 历史分叉,
    同步需 force push,慎用)
