# jpeg — 头像 JPEG 编解码(libjpeg 薄封装)

2026-09-21 随「拍摄录入 + 头像」功能落位(交接见 `docs/tech/CAPTURE_AVATAR_HANDOFF.md`)。

## 定位

只回答"RGB 像素 ↔ JPEG 字节"这一件事,内存↔内存,不落盘。业务语义
(何时拍、存到哪个用户、何时显示)分别归 `services/vision`(编码)与
`ui/widgets/dg_avatar`(解码)所有——本模块不含任何业务知识。

为什么不用 LVGL 的解码器:`lv_conf.h` 里 `LV_USE_PNG/LV_USE_SJPG/LV_USE_FS_*`
全是关的(固件裁剪决定),而板上 rootfs 与交叉 sysroot 本来就带 libjpeg-turbo
(`/usr/lib/libjpeg.so.8`),零新增依赖。

## 接口

```c
#include "modules/jpeg/dg_jpeg.h"

/* 编码:拍摄那一刻用一次(160×160 q80 约 6~10KB) */
size_t jlen = 0;
dg_jpeg_encode_rgb(rgb, 160, 160, 80, buf, sizeof(buf), &jlen);

/* 解码:scale_denom 用 libjpeg 的 DCT 缩放,缩略图省内存省 CPU */
uint8_t px[40 * 40 * 3];
int w, h;
dg_jpeg_decode_rgb(jpeg, jlen, 4 /* 160/4=40 */, px, sizeof(px), &w, &h);
```

## 设计要点

- **错误不 exit**:libjpeg 默认 `error_exit` 直接终止进程;这里换成 `setjmp`
  跳出并返回 `DG_ERR_INTERNAL`——一张损坏的头像不该带崩整个门禁。
- **无静态状态**:每次调用一套局部 `cinfo/jerr`,多线程并发安全
  (编码在总线线程、解码在 LVGL 线程,各叫各的)。
- **显式报错不截断**:输出容量不足返回 `DG_ERR_NO_MEMORY`,绝不交付半张图。
- 逐行喂/取(单行缓冲),不整图二次拷贝;头像量级(≤160×160)毫秒级完成。

## 测试

`tests/test_jpeg.c`(宿主 gcc):编解码往返误差上界、缩放解码尺寸、
损坏输入/参数非法等错误路径。
