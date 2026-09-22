# k7_rk3576 — RK3576 K7 人脸识别门禁项目仓库

 KickPi K7(RK3576)+ 5寸MIPI屏(F050008M01)+ IMX415 摄像头,LVGL + NPU 人脸识别门禁。

## 仓库内容

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
