# hal/camera — 相机 HAL(RGB 帧投递)

同一 `camera.h` 接口两种后端,UI/业务不感知差异:

| 文件 | 端 | 说明 |
|---|---|---|
| `camera_sim.c` | DG_SIM | 目录图片循环(stb 解码),开发机无硬件用 |
| `camera_board.c` | 板 | V4L2+RGA+rkaiq 真实链路(见下) |

## 板上链路(2026-09-18 B6 打通)

```
IMX415(cam2,实体名 m02_b_imx415 8-0037)→ rkcif → rkisp-vir2(/dev/media5)
  → mainpath /dev/video51(V4L2 MPLANE,单平面 NV12 1280x720,MMAP 4 缓冲)
  → RGA:转 XRGB8888 + 旋转 90(字节序 B,G,R,X 与 LVGL 32 位色直通)
  → camera_frame_t → 主页 canvas 100ms 轮询
```

## 三个致命坑(全部实测,改动前必读)

1. **rkaiq uAPI2 的 `sns_ent_name` 是传感器实体名**(如 `m02_b_imx415 8-0037`,
   见 `/sys/class/video4linux/v4l-subdev*/name`),传 `/dev/mediaN` 直接段错误。
2. **aiq2.lock 死锁**:3A server 被 prepare 触发后持锁等"流启动事件",而 client
   的 init/prepare 都要拿这把锁。解法=并发会合:取流线程延迟 500ms STREAMON,
   prepare 在主初始化线程里等锁,server 见流放锁。单线程任何顺序都死锁。
3. **cam2 口传感器映射第 3 个虚拟 ISP**(rkisp-vir2=/dev/media5,mainpath=
   /dev/video51),不是 vir0;换端口按 `media-ctl -d /dev/media* -p` 重查。

另:librga 成功码有 `SUCCESS=1`/`NOERROR=2` 两个;V4L2 用 `V4L2_PIX_FMT_NV12`
(单平面),`NV12M` 是双平面会 QUERYBUF EINVAL。

## 环境开关(零魔数)

| 变量 | 默认 | 说明 |
|---|---|---|
| `DG_AIQ_SENSOR` | `m02_b_imx415 8-0037` | 传感器实体名 |
| `DG_AIQ_IQDIR` | `/etc/iqfiles` | IQ 文件目录 |
| `DG_AIQ` | 开 | 置 0 跳过 3A(裸流欠曝光,逃生通道) |
| `DG_CAM_DEV` | `/dev/video51` | mainpath 节点 |
| `DG_CAM_W/H` | 1280/720 | ISP 输出分辨率 |
| `DG_CAM_ROT` | 90 | 0/90/180/270,方向不对改这里 |
| `DG_CAM_DUMP` | 关 | 置 1 首帧写 /tmp/dg_cam.raw(远程取证) |

## 使用示例

```c
#include "hal/camera/camera.h"
camera_init("/dev/video51", NULL, NULL);   /* 内部后台线程初始化,立即返回 */
for (;;) {                                  /* 主循环 */
    camera_poll();                          /* DQBUF+RGA 转换 */
    const camera_frame_t *f = camera_latest(); /* 无帧返回 NULL */
}
```

## 验证方法(无需肉眼)

`DG_CAM_DUMP=1` 抓首帧 → scp 回开发机 → 检查通道均值非零非饱和、
动态范围、ASCII 缩略图有场景结构(见 DEVLOG 2026-09-18 B6 条目)。
