/*
 * display_drm_v9.c — 板上显示后端(LVGL 9.5 内置 Linux DRM 驱动)
 *
 * 为什么用 v9 内置驱动而非自研 flush:v9 的 linux_drm 后端(dumb buffer x2,
 * DIRECT 双缓冲 + atomic 翻转 + vblank 等待)与 8.3 时代已验证的
 * 「full_refresh 双缓冲页翻转」架构等价——脏区渲染 + sync_areas 双向同步 +
 * 垂直同步翻转,无残影语义由 v9 内建(refr_sync_areas),不必重走
 * 2026-09-22 video plane 的擦除语义深坑(DEVLOG 深夜条)。
 * 若板上 RK3576 实测有坑(atomic/分辨率/刷新异常),预案=复用已验证的
 * legacy modeset 自研 flush(契约 §2 D2),本文件只换 flush 层。
 *
 * 触摸:自研 touch_evdev(Type-B MT + 按下沿 + DG_TOUCH_* 校准)喂 v9 indev。
 * 注意:v9 DRM 驱动在 lv_linux_drm_create() 内部会 lv_tick_set_cb(自带
 * CLOCK_MONOTONIC 毫秒回调);C2 的 ui_init 在 display_init 之后执行,
 * 届时 lv_tick_set_cb(dg_ui_tick_ms) 覆盖为 ui 时基,顺序天然正确。
 */
#include "display.h"
#include "dg_log.h"
#include "lvgl.h"       /* LV_USE_LINUX_DRM=1 时 lvgl.h 已暴露 lv_linux_drm.h */
#include "touch_evdev.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>   /* 走查取证:触发文件检查 */
#include <unistd.h>     /* unlink */

int display_init(void)
{
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        DG_LOGE("[DISPLAY]", "lv_linux_drm_create 失败");
        return DG_ERR_IO;
    }

    /* DG_UI_PLANE=1(video plane 直通阶段 A,2026-09-25 方案):display 换
     * ARGB8888——v9 渲染核心对带 alpha 的 display 逐脏区清透明(lv_refr.c
     * 内建,8.3 残影根因已除),screen 透明后不透明页自备底、透明页(主页)
     * 洞出下层;驱动 fourcc 跟随本设置(lv_linux_drm_set_file 内)。关=主线
     * XRGB 现状零影响 */
    bool ui_plane = false;
    const char *up = getenv("DG_UI_PLANE");
    if (up && up[0] == '1') {
        ui_plane = true;
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    }

    char *path = lv_linux_drm_find_device_path();
    lv_result_t res = lv_linux_drm_set_file(disp, path ? path : "/dev/dri/card0", -1);
    /* 路径由驱动 lv_zalloc(tlsf 池)分配:必须用 lv_free——libc free 会
     * abort("free(): invalid pointer",板上实测 C3)。宿主 sim 不编本文件,
     * 故 C1 未暴露 */
    lv_free(path);
    if (res != LV_RESULT_OK) {
        DG_LOGE("[DISPLAY]", "DRM 设备打开/配置失败");
        return DG_ERR_IO;
    }

    if (ui_plane) {
        /* screen 透明须在 set_file 成功后:失败路径不留下半透明状态。
         * bottom_layer 的透明由 set_color_format 内建处理,无需另设 */
        lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
    }

    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    if (w != DG_SCREEN_W || h != DG_SCREEN_H)
        DG_LOGW("[DISPLAY]", "DRM 分辨率 %dx%d 与设计 %dx%d 不同",
                w, h, DG_SCREEN_W, DG_SCREEN_H);

    /* 触摸注册失败不阻塞显示:web 上位机路径仍完整可用 */
    if (touch_evdev_start(w, h) != DG_OK)
        DG_LOGW("[DISPLAY]", "触摸未注册,UI 需经上位机操作");

    DG_LOGI("[DISPLAY]", "v9 DRM 后端就绪 %dx%d %s(direct 双缓冲 atomic 翻转%s)",
            w, h, ui_plane ? "ARGB8888" : "XRGB8888",
            ui_plane ? ",screen 透明" : "");
    return DG_OK;
}

void display_poll(void)
{
    /* ---- 走查取证(C3,诊断专用;v8 时代 DG_DUMP_FIRST_FRAME 的对等物) ----
     * 仅当 DG_WALK_DUMP_DIR 设置时生效:每帧检查触发文件 /tmp/dg_shot,
     * 存在则把「当前屏幕」(DIRECT 双缓冲经 sync_areas 收敛后两缓冲同像,
     * 取活动缓冲即整帧 XRGB8888,行距=dumb pitch)落 RAW 后删触发文件。
     * 生产路径(DG_WALK_DUMP_DIR 未设)只有一次 getenv,零开销 */
    static int walk_mode = -1;
    if (walk_mode < 0)
        walk_mode = getenv("DG_WALK_DUMP_DIR") != NULL;
    if (!walk_mode || stat("/tmp/dg_shot", &(struct stat){0}) != 0)
        return;
    unlink("/tmp/dg_shot");
    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_display_get_default());
    const char *dir = getenv("DG_WALK_DUMP_DIR");
    char path[128];
    snprintf(path, sizeof(path), "%s/dg_screen.raw", dir ? dir : "/tmp");
    FILE *f = fopen(path, "wb");
    if (f && buf) {
        fwrite(buf->data, 1, (size_t)buf->header.stride * buf->header.h, f);
        fclose(f);
        DG_LOGI("[DISPLAY]", "屏幕已导出 %s", path);
    } else if (f) {
        fclose(f);
    }
}

/* ---- video plane 直通(2026-09-25 重启,重启方案阶段B)----
 * v9 渲染核心内建逐脏区透明擦除(阶段A 板上实证:95% 透明洞、4 轮急速
 * 往返零残影),8.3 的擦除语义缺失根因已除。本实现 = f519b1e 的 plane
 * 发现/atomic 提交移植到 v9 驱动之上:fd/crtc/ui_plane 经 lv_linux_drm_*
 * 访问器取自驱动同一 fd(禁混 legacy/第二 fd);NV12 dma-buf 每槽只
 * 导入/AddFB 一次缓存;任何失败永久降级软渲染(dg_preview 双模承接)。 */

#include <errno.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

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

/* 取对象指定名字的属性 id。各 plane 的 prop id 互不相同,不能复用
 * v9 驱动为 UI plane 缓存的那份 */
static uint32_t vp_prop(unsigned obj_id, const char *name)
{
    int fd = lv_linux_drm_get_fd(lv_display_get_default());
    drmModeObjectPropertiesPtr op =
        drmModeObjectGetProperties(fd, obj_id, DRM_MODE_OBJECT_PLANE);
    uint32_t id = 0;
    for (uint32_t i = 0; op && i < op->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, op->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, name) == 0)
            id = prop->prop_id;
        drmModeFreeProperty(prop);
        if (id)
            break;
    }
    if (op)
        drmModeFreeObjectProperties(op);
    return id;
}

/* crtc_id 在 res->crtcs[] 中的下标(plane 的 possible_crtcs 是位掩码) */
static unsigned vp_crtc_idx(int fd, uint32_t crtc_id)
{
    unsigned idx = UINT_MAX;
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res)
        return idx;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            idx = (unsigned)i;
            break;
        }
    }
    drmModeFreeResources(res);
    return idx;
}

/* 找一个能挂当前 CRTC、支持 NV12、空闲、非 UI plane 的 overlay;
 * zpos 压到 0(UI plane 之下),层级重配带回 modeset 标志 */
static bool vp_discover(void)
{
    s_vp.tried = true;
    /* 降级演练注入:强制走「plane 不可用→永久降级软渲染」路径(B3 门禁) */
    const char *ff = getenv("DG_UI_PLANE_FORCE_FAIL");
    if (ff && ff[0] == '1') {
        DG_LOGW("[DISPLAY]", "DG_UI_PLANE_FORCE_FAIL=1:演练降级,plane 不可用");
        return false;
    }
    lv_display_t *disp = lv_display_get_default();
    int fd = lv_linux_drm_get_fd(disp);
    uint32_t ui_plane = lv_linux_drm_get_plane_id(disp);
    uint32_t crtc_id = lv_linux_drm_get_crtc_id(disp);
    unsigned crtc_idx = vp_crtc_idx(fd, crtc_id);
    if (crtc_idx == UINT_MAX)
        return false;

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
 * 从 0 开始,0 当空位会在首帧假命中返回 fb=0(f519b1e 实测踩坑) */
static uint32_t vp_fb_for(int slot, int dmabuf_fd, int32_t h, int32_t stride)
{
    int fd = lv_linux_drm_get_fd(lv_display_get_default());
    for (size_t i = 0; i < sizeof(s_vp.cache) / sizeof(s_vp.cache[0]); i++) {
        if (s_vp.cache[i].slot == slot)
            return s_vp.cache[i].fb;
        if (s_vp.cache[i].slot < 0) {
            uint32_t handle = 0;
            if (drmPrimeFDToHandle(fd, dmabuf_fd, &handle)) {
                DG_LOGE("[DISPLAY]", "PrimeFD 导入失败:%s", strerror(errno));
                return 0;
            }
            /* NV12 双平面共用一块 dma-buf:UV 平面由 offset 表达,
             * drm 需要 4 字节对齐(720x1280 行距偶数,天然满足) */
            uint32_t handles[2] = { handle, handle };
            uint32_t pitches[2] = { (uint32_t)stride, (uint32_t)stride };
            uint32_t offsets[2] = { 0, (uint32_t)stride * h };
            uint32_t fb = 0;
            if (drmModeAddFB2(fd, DG_SCREEN_W, DG_SCREEN_H, DRM_FORMAT_NV12,
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

    /* 时序契约(板上 2026-09-25 实测):先等驱动挂起的 UI flip 完成再提交,
     * 且 video commit 用阻塞模式(应用完才返回)。NONBLOCK 排队会让驱动的
     * 下一次 flip EBUSY——失败的 flip 不入队,驱动的 flush_wait poll 便永久
     * 等不到事件,主循环卡死(看门狗 12s 退出) */
    lv_display_t *disp = lv_display_get_default();
    int fd = lv_linux_drm_get_fd(disp);
    lv_linux_drm_wait_flip(disp);
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "FB_ID"), fb);
    drmModeAtomicAddProperty(req, s_vp.plane_id, vp_prop(s_vp.plane_id, "CRTC_ID"),
                             lv_linux_drm_get_crtc_id(lv_display_get_default()));
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
    int rc = drmModeAtomicCommit(fd, req, 0, NULL);   /* 阻塞:应用完才返回 */
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
    int fd = lv_linux_drm_get_fd(lv_display_get_default());
    uint32_t fb_prop = vp_prop(s_vp.plane_id, "FB_ID");
    if (!fb_prop)
        return;
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, s_vp.plane_id, fb_prop, 0);
    drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    drmModeAtomicFree(req);
}

void display_clear_fbs(void)
{
}
