# door-guard — 门禁应用

RK3576 K7 人脸识别门禁主应用。架构与约定见仓库根 `PROJECT_PLAN.md` §三/§四。

## 目录职责

| 目录 | 职责 | 对接的下层 |
|---|---|---|
| `app/` | main 装配、启动顺序、配置加载 | 所有模块 |
| `ui/` | LVGL 界面:待机/识别中/动作引导/结果/管理页 | 事件总线(只读事件) |
| `modules/capture/` | 取流线程:rkaiq 3A + V4L2/RKADK → NV12 帧环形缓冲 | hal/camera |
| `modules/vision/` | 人脸链路:ROCKIVA 检测/关键点/识别/1:N 检索 | hal/npu、auth/face |
| `modules/liveness/` | 动作活体状态机:随机动作序列 + 姿态/张嘴判定 | vision 输出 |
| `modules/access/` | 认证融合、门控决策、开门日志 | hal/gpio、hal/storage |
| `hal/*` | 薄封装:camera/display(DRM)/npu/gpio(libgpiod)/uart/storage(SQLite) | 内核/库 |
| `auth/` | `auth_provider` 统一认证接口 + face/finger/card 三个实现 | modules/access |
| `proto/` | 线程间消息、事件总线定义(纯 C 头/结构) | — |
| `third_party/` | rockiva 头+lib、LVGL、cjson、sqlite3 的接入说明 | SDK/系统 |
| `tools/` | PC 端脚本(如后续自定义模型的转换脚本) | — |
| `configs/` | `device.json` 运行配置 | app |

## 硬性约定

1. 模块间只经 `proto/` 的队列/事件总线通信,禁止跨层直调
2. 像素零拷贝:ISP→RGA→DRM/NPU 全链路 dma-buf
3. 比对阈值、GPIO 编号、串口路径等全部进 `configs/device.json`,代码里不留魔数
4. 新增认证方式 = 新写一个 `auth_provider` 实现,`access_service` 不改

## 交叉编译(VM 上)

```bash
export SDK_ROOT=~/Linux/rk3576/Rk3576-SDK/rk3576_data/rk3576-linux-2026091008
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64.cmake   # B8 阶段补齐工具链文件
cmake --build build
# 产物 scp 到板子 /usr/bin/ 或做成 buildroot 包(B10)
```

## 里程碑对应(PROJECT_PLAN §五)

B8:骨架编译过 + LVGL 空界面 + 预览上屏
B9:人脸全链路(注册→识别→开门→日志)
B10:活体 → 指纹/卡 → 产品化裁剪
