# k7_rk3576 — RK3576 K7 人脸识别门禁项目仓库

 KickPi K7(RK3576)+ 5寸MIPI屏(F050008M01)+ IMX415 摄像头,LVGL + NPU 人脸识别门禁。

## 仓库内容(刻意最小化,官方 SDK 19GB 不进 git)

```
├── PROJECT_PLAN.md          项目方案与执行手册(唯一事实来源,每阶段更新)
├── docs/DEV_HANDBOOK.md     软件开发手册(硬件事实/编译环境/踩坑索引)
├── NEXT_SESSION_PROMPT.md   下次会话开工提示词
├── sdk-guide/               官方 SDK 开发资源指南(必读) + 9 份精选官方文档 PDF
├── door-guard/              门禁应用源码(后续创建,WSL 开发 / VM 编译)
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
- WSL:写代码、跑纯逻辑单元测试;VM:交叉编译(SDK prebuilts 工具链)、固件全量编译
- 中枢:ssh://git@192.168.2.150:222/olwhistle/k7_rk3576.git(局域网 Gitea,SSH 密钥认证)
