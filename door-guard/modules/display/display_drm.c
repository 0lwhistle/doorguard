/*
 * display_drm.c — 板上显示后端(lv_drivers DRM dumb-buffer,MIT)
 *
 * 为什么不用 /dev/fb0:B4 固件 rockchipdrmfb 的 mmap 返回 EBUSY(实测,
 * fbdev 仿真层限制);lv_demo 实际走 DRM。本后端经 libdrm 创建 dumb
 * buffer + atomic modeset,与 lv_drivers/display/drm.c 同源。
 * 触摸输入:evdev 自动探测(fts_ts/goodix,见 touch_evdev.c),注册 pointer indev。
 *
 * 2026-09-22 video-plane 直通改造:
 *  - UI fb 换 ARGB8888;LVGL 改 direct_mode + 单全幅缓冲(脏区原位补丁到
 *    dumb fb,不再整屏重渲/页翻转)——预览移入硬件 plane 后 UI 只画控件;
 *  - 新增 video plane 直通:NV12 dma-buf 送 VOP2 Overlay plane(zpos 压到
 *    UI 之下),预览零 CPU;失败自动降级,页面走软渲染回退;
 *  - 透明底页面(主页)进入时清 fb(drm_clear_fbs),未画区域透出视频。
 */
#include "display.h"
#include "dg_log.h"
#include "lvgl.h"
#include "../../third_party/lv_drivers/drm.h"
#include "touch_evdev.h"
#include "ui/theme.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

void drm_dump_first_frame_once(const lv_color_t *frame, int32_t w, int32_t h);
static void drm_flush_wrapper(lv_disp_drv_t *drv, const lv_area_t *area,
                              lv_color_t *color_p);

/* direct 模式 LVGL 直绘缓冲(与 dumb fb 镜像;clear 时两处都要清) */
static lv_color_t *s_lvbuf;
static size_t s_lvbuf_size;

int display_init(void)
{
    drm_init();
    lv_coord_t w = 0, h = 0;
    drm_get_sizes(&w, &h, NULL);
    if (w != DG_SCREEN_W || h != DG_SCREEN_H)
        DG_LOGW("[DISPLAY]", "DRM 分辨率 %dx%d 与设计 %dx%d 不同",
                w, h, DG_SCREEN_W, DG_SCREEN_H);

    /* direct 模式:LVGL 只重绘脏区,缓冲常驻(与 fb 镜像),flush 原位补丁 */
    static lv_disp_draw_buf_t draw_buf;
    s_lvbuf_size = (size_t)w * h * sizeof(lv_color_t);
    s_lvbuf = malloc(s_lvbuf_size);
    if (!s_lvbuf) {
        DG_LOGE("[DISPLAY]", "DRM 渲染缓冲分配失败");
        return DG_ERR_NO_MEMORY;
    }
    memset(s_lvbuf, 0, s_lvbuf_size);           /* 与清零后的 dumb fb 一致 */
    lv_disp_draw_buf_init(&draw_buf, s_lvbuf, NULL, w * h);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = w;
    disp_drv.ver_res = h;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = drm_flush_wrapper;
    disp_drv.direct_mode = 1;
    disp_drv.full_refresh = 0;
    /* video-plane underlay 关键开关:声明显示支持逐像素 alpha。不开时
     * LVGL8 渲染每个脏区前用 bg_color(白)预填缓冲,fb 永远不透明,
     * 下层视频被盖死(2026-09-22 实测踩坑) */
    disp_drv.screen_transp = 1;
    lv_disp_drv_register(&disp_drv);

    /* 触摸注册失败不阻塞显示:web 上位机路径仍完整可用 */
    if (touch_evdev_start(w, h) != DG_OK)
        DG_LOGW("[DISPLAY]", "触摸未注册,UI 需经上位机操作");

    DG_LOGI("[DISPLAY]", "DRM 后端就绪 %dx%d direct=1 flush_cb=%p", w, h,
            (void *)disp_drv.flush_cb);
    return DG_OK;
}

static void drm_flush_wrapper(lv_disp_drv_t *drv, const lv_area_t *area,
                              lv_color_t *color_p)
{
    static int n = 0;
    if (n < 3)
        DG_LOGI("[DISPLAY]", "flush #%d area=%d,%d %dx%d", n, area->x1, area->y1,
                area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
    n++;
    drm_flush(drv, area, color_p);
    if (n == 20)                          /* 第 20 帧:页面已完整渲染 */
        drm_dump_first_frame_once(color_p, drv->hor_res, drv->ver_res);
    /* 注意:drm_dump_first_frame_once 内部受 DG_DUMP_FIRST_FRAME 环境变量
     * 门控,未设置时立即返回,生产路径无 IO */
}

void display_poll(void)
{
}

/* 首帧导出(板上取证:DG_DUMP_FIRST_FRAME=1 时写 /tmp/dg_frame.raw,
 * ARGB8888 720x1280;仅首帧一次,不影响运行) */
static int s_dump_done = 0;

void drm_dump_first_frame_once(const lv_color_t *frame, int32_t w, int32_t h)
{
    if (s_dump_done || getenv("DG_DUMP_FIRST_FRAME") == NULL)
        return;
    FILE *f = fopen("/tmp/dg_frame.raw", "wb");
    if (f) {
        fwrite(frame, sizeof(lv_color_t), (size_t)w * h, f);
        fclose(f);
        DG_LOGI("[DISPLAY]", "首帧已导出 /tmp/dg_frame.raw");
    }
    s_dump_done = 1;
}

/* ---- 视频 overlay plane(zpos 压在 UI plane 之下,预览零 CPU) ----
 * 板上实测(2026-09-22 探针):crtc 上空闲 Overlay(Esmart3/Cluster0)均
 * 支持 NV12,zpos/alpha 可写;Esmart/Cluster 都无 90° 硬件旋转 → 相机侧
 * RGA 预旋转后 1:1 直送。UI atomic 与本处 atomic 同 fd,均只动各自 plane */

static struct {
    bool tried;                            /* 发现只做一次(失败即永久降级) */
    bool ok;
    unsigned plane_id;
    uint32_t zpos_prop;
    /* fb 缓存:相机槽位固定,导入/AddFB 每槽只做一次 */
    struct {
        int slot;
        uint32_t fb;
    } cache[4];
} s_vp = { .cache = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}} };

/* 取对象指定名字的属性 id(注意:各 plane 的 prop id 互不相同,不能复用
 * UI plane 缓存的那份) */
static uint32_t vp_prop(unsigned obj_id, const char *name)
{
    int fd = drm_fd();
    drmModeObjectPropertiesPtr op =
        drmModeObjectGetProperties(fd, obj_id, DRM_MODE_OBJECT_PLANE);
    uint32_t id = 0;
    for (uint32_t i = 0; op && i < op->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, op->props[i]);
        if (!prop)
            continue;
        if (!strcmp(prop->name, name))
            id = prop->prop_id;
        drmModeFreeProperty(prop);
        if (id)
            break;
    }
    if (op)
        drmModeFreeObjectProperties(op);
    return id;
}

/* 找一个能挂当前 CRTC、支持 NV12、空闲的 plane;顺带取 zpos 属性 */
static bool vp_discover(void)
{
    s_vp.tried = true;
    int fd = drm_fd();
    unsigned ui_plane = drm_ui_plane_id();
    unsigned crtc_idx = drm_crtc_idx_get();

    drmModePlaneResPtr pr = drmModeGetPlaneResources(fd);
    if (!pr)
        return false;
    bool found = false;
    for (uint32_t i = 0; i < pr->count_planes && !found; i++) {
        drmModePlanePtr p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p)
            continue;
        bool nv12 = false;
        for (uint32_t j = 0; j < p->count_formats; j++)
            if (p->formats[j] == DRM_FORMAT_NV12)
                nv12 = true;
        bool usable = nv12 && p->fb_id == 0 &&
                      (p->possible_crtcs & (1u << crtc_idx)) &&
                      p->plane_id != ui_plane;
        if (usable) {
            s_vp.zpos_prop = vp_prop(p->plane_id, "zpos");
            if (s_vp.zpos_prop) {
                s_vp.plane_id = p->plane_id;
                found = true;
            }
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    if (!found)
        return false;

    /* zpos 压到 UI plane(zpos=1)之下;层级重配带回 modeset 标志 */
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, s_vp.plane_id, s_vp.zpos_prop, 0);
    int rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    if (rc) {
        DG_LOGE("[DISPLAY]", "video plane zpos=0 失败(rc=%d),降级软渲染", rc);
        return false;
    }
    DG_LOGI("[DISPLAY]", "video plane=%u 就绪(zpos=0,NV12 underlay)",
            s_vp.plane_id);
    return true;
}

/* 槽位 → NV12 fb(每槽只导入/AddFB 一次)。哨兵用 -1 而非 0——相机槽位
 * 从 0 开始,0 当空位会在首帧假命中返回 fb=0(2026-09-22 实测踩坑) */
static uint32_t vp_fb_for(int slot, int dmabuf_fd, int32_t h, int32_t stride)
{
    for (size_t i = 0; i < sizeof(s_vp.cache) / sizeof(s_vp.cache[0]); i++) {
        if (s_vp.cache[i].slot == slot)
            return s_vp.cache[i].fb;
        if (s_vp.cache[i].slot < 0) {
            uint32_t handle = 0;
            if (drmPrimeFDToHandle(drm_fd(), dmabuf_fd, &handle)) {
                DG_LOGE("[DISPLAY]", "PrimeFD 导入失败:%s", strerror(errno));
                return 0;
            }
            uint32_t handles[2] = { handle, handle };
            uint32_t pitches[2] = { (uint32_t)stride, (uint32_t)stride };
            uint32_t offsets[2] = { 0, (uint32_t)stride * h };
            uint32_t fb = 0;
            if (drmModeAddFB2(drm_fd(), DG_SCREEN_W, DG_SCREEN_H, DRM_FORMAT_NV12,
                              handles, pitches, offsets, &fb, 0)) {
                DG_LOGE("[DISPLAY]", "AddFB2 NV12 失败:%s", strerror(errno));
                return 0;
            }
            s_vp.cache[i].slot = slot;
            s_vp.cache[i].fb = fb;
            return fb;
        }
    }
    return 0;
}

bool display_has_video_plane(void)
{
    if (!s_vp.tried)
        s_vp.ok = vp_discover();
    return s_vp.ok;
}

int display_video_plane_show(int slot, int dmabuf_fd, int32_t w, int32_t h,
                             int32_t stride)
{
    if (!display_has_video_plane())
        return DG_ERR_UNSUPPORTED;
    uint32_t fb = vp_fb_for(slot, dmabuf_fd, h, stride);
    if (!fb) {
        s_vp.ok = false;
        DG_LOGE("[DISPLAY]", "video fb 导入失败,永久降级软渲染");
        return DG_ERR_NO_MEMORY;
    }

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "FB_ID"), fb);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "CRTC_ID"),
                             drm_crtc_id_get());
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "SRC_X"), 0);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "SRC_Y"), 0);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "SRC_W"),
                             (uint64_t)w << 16);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "SRC_H"),
                             (uint64_t)h << 16);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "CRTC_X"), 0);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "CRTC_Y"), 0);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "CRTC_W"),
                             DG_SCREEN_W);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "CRTC_H"),
                             DG_SCREEN_H);
    int rc = drmModeAtomicCommit(drm_fd(), req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    drmModeAtomicFree(req);
    if (rc) {
        static int n_err;
        if (n_err++ < 3)
            DG_LOGE("[DISPLAY]", "video plane 提交失败(rc=%d)", rc);
        return rc;
    }
    return DG_OK;
}

void display_video_plane_hide(void)
{
    if (!s_vp.ok)
        return;
    uint32_t fb_prop = vp_prop(s_vp.plane_id, "FB_ID");
    if (!fb_prop)
        return;
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, s_vp.plane_id, fb_prop, 0);
    drmModeAtomicCommit(drm_fd(), req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    drmModeAtomicFree(req);
}

void display_clear_fbs(void)
{
    drm_clear_fbs();
    if (s_lvbuf)
        memset(s_lvbuf, 0, s_lvbuf_size);
}
