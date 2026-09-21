/*
 * camera_board.c — 板上相机后端(2026-09-18 B6 接入)
 *
 * 链路:IMX415(cam2)→ rkcif → rkisp-vir0(/dev/media3,3A 由 rkaiq 驱动)
 *       → rkisp_mainpath(/dev/video33)V4L2 MPLANE NV12
 *       → RGA 硬件旋转+转 BGRA8888(字节序 B,G,R,X,与 LVGL 32 位色直通)
 *       → camera_frame_t 投递(主页 canvas 100ms 轮询拷贝)。
 *
 * 为什么 RGA 而非软件转换:720p 每帧 ~1.3MB,软转在 A53 上占满主循环预算,
 * RGA 单帧 <5ms;板上 librga.so.2 与 sysroot 头均已具备。
 *
 * 板级适配走环境变量(零魔数,板上无 /etc 改码需求):
 *   DG_AIQ_SENSOR 传感器 media entity 名(默认 m02_b_imx415 8-0037;
 *                 注意是实体名而非 /dev/mediaN——rkaiq 按名走链路,传错即崩)
 *   DG_AIQ_IQDIR  IQ 文件目录(默认 /etc/iqfiles)
 *   DG_CAM_DEV    mainpath 视频节点(默认 /dev/video51 = rkisp-vir2;
 *                 cam2 口传感器按端口序映射到第 3 个虚拟 ISP,实测 media5)
 *   DG_CAM_W/H    ISP 输出 NV12 分辨率(默认 1280x720)
 *   DG_CAM_ROT    预览旋转 0/90/180/270(默认 90:横置摄像头→竖屏满幅)
 *   DG_AIQ        置 0 跳过 rkaiq(裸流,驱动默认曝光;3A 死锁逃生通道)
 *   DG_CAM_DUMP   置 1 时首帧写 /tmp/dg_cam.raw(远程取证用,生产不开)
 *
 * 3A 说明:uAPI2 默认经 rkaiq_3A_server(rootfs S40 服务)计算算法,
 * 本进程只做链路控制;AE 收敛约 1~2s,期间画面由暗转正常属预期。
 */
#include "camera.h"
#include "dg_log.h"

#include "im2d.h"
#include "uAPI2/rk_aiq_user_api2_imgproc.h"
#include "uAPI2/rk_aiq_user_api2_sysctl.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAM_TAG "[CAMERA]"
#define CAM_BUF_CNT 4
#define CAM_OUT_W 720 /* 面板竖屏尺寸;90/270 旋转后目标幅面 */
#define CAM_OUT_H 1280

static rk_aiq_sys_ctx_t *s_aiq;
static int s_vfd = -1;
static bool s_stream_on;

static struct {
    void *addr;
    size_t size;
} s_cap[CAM_BUF_CNT];

/* 三缓冲:capture/视觉(worker 线程)与 UI 轮询并发读,轮转写入——
 * 双缓冲下主循环偶尔连投两帧、UI 还没拷走时第三帧会写回 s_frame 正指向的
 * 缓冲(撕裂);三缓冲给读方两帧周期的拷贝窗口(约 66ms,30fps 下足够) */
static uint8_t *s_out[3];
static int s_out_idx;
static camera_frame_t s_frame;
static uint32_t s_seq;
static camera_nv12_fn s_nv12_fn;             /* 视觉帧监听(可空) */
static camera_nv12_release_fn s_nv12_rel;
static bool s_busy[CAM_BUF_CNT];             /* ROCKIVA 占用中,归还待 release */
static uint32_t s_fid[CAM_BUF_CNT];
static bool s_ready; /* 取流线程完成 STREAMON 后置位 */
static int s_cap_w, s_cap_h;
static int s_nplanes = 1, s_stride;
static int s_rot;
static int s_out_w, s_out_h;

static const char *env_or(const char *k, const char *dflt)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : dflt;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int rot_from_env(void)
{
    const char *v = getenv("DG_CAM_ROT");
    if (!v || !v[0])
        return 90;
    if (!strcmp(v, "0"))
        return 0;
    if (!strcmp(v, "180"))
        return 180;
    if (!strcmp(v, "270"))
        return 270;
    return 90;
}

static int v4l2_setup(void)
{
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    struct v4l2_requestbuffers req = { 0 };
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };

    s_vfd = open(env_or("DG_CAM_DEV", "/dev/video51"), O_RDWR | O_NONBLOCK);
    if (s_vfd < 0) {
        DG_LOGE(CAM_TAG, "打开 mainpath 失败(%s)", env_or("DG_CAM_DEV", "/dev/video51"));
        return DG_ERR_IO;
    }

    s_cap_w = atoi(env_or("DG_CAM_W", "1280"));
    s_cap_h = atoi(env_or("DG_CAM_H", "720"));
    fmt.fmt.pix_mp.width = s_cap_w;
    fmt.fmt.pix_mp.height = s_cap_h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12; /* 单平面连续(NV12M 是双平面) */
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(s_vfd, VIDIOC_S_FMT, &fmt) < 0) {
        DG_LOGE(CAM_TAG, "S_FMT NV12 失败: %s", strerror(errno));
        return DG_ERR_IO;
    }
    s_cap_w = fmt.fmt.pix_mp.width;
    s_cap_h = fmt.fmt.pix_mp.height;
    s_nplanes = fmt.fmt.pix_mp.num_planes;
    s_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    DG_LOGI(CAM_TAG, "mainpath 格式 %dx%d NV12 stride=%d planes=%d", s_cap_w,
            s_cap_h, s_stride, s_nplanes);
    if (s_nplanes != 1) {
        DG_LOGE(CAM_TAG, "仅支持单平面 NV12,驱动给 %d 平面", s_nplanes);
        return DG_ERR_PARAM;
    }

    req.count = CAM_BUF_CNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s_vfd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        DG_LOGE(CAM_TAG, "REQBUFS 失败: %s", strerror(errno));
        return DG_ERR_IO;
    }
    for (unsigned i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = s_nplanes;
        buf.m.planes = planes;
        if (xioctl(s_vfd, VIDIOC_QUERYBUF, &buf) < 0) {
            DG_LOGE(CAM_TAG, "QUERYBUF %u 失败: %s", i, strerror(errno));
            return DG_ERR_IO;
        }
        s_cap[i].size = planes[0].length;
        s_cap[i].addr = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, s_vfd, planes[0].m.mem_offset);
        if (s_cap[i].addr == MAP_FAILED) {
            DG_LOGE(CAM_TAG, "mmap %u 失败: %s", i, strerror(errno));
            return DG_ERR_NO_MEMORY;
        }
    }
    for (unsigned i = 0; i < req.count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = s_nplanes;
        buf.m.planes = planes;
        if (xioctl(s_vfd, VIDIOC_QBUF, &buf) < 0) {
            DG_LOGE(CAM_TAG, "QBUF %u 失败: %s", i, strerror(errno));
            return DG_ERR_IO;
        }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(s_vfd, VIDIOC_STREAMON, &type) < 0) {
        DG_LOGE(CAM_TAG, "STREAMON 失败: %s", strerror(errno));
        return DG_ERR_IO;
    }
    s_stream_on = true;
    DG_LOGI(CAM_TAG, "V4L2 流已启动 %d 缓冲", req.count);
    return DG_OK;
}

/* 取流线程:延迟后 S_FMT+STREAMON。与 aiq prepare 并发会合——
 * server 在 prepare 里配置完 ISP 统计节点后持 aiq2.lock 等"流启动事件",
 * 提前量给 server 留配置时间;流起 → server 放锁 → prepare 返回 */
static void *stream_thread(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 500 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    if (v4l2_setup() != DG_OK) {
        DG_LOGE(CAM_TAG, "取流线程:V4L2 初始化失败(相机未就绪,不影响 UI)");
        return NULL;
    }
    s_ready = true;
    DG_LOGI(CAM_TAG, "相机后端就绪:%dx%d NV12 → RGA rot=%d → %dx%d XRGB",
            s_cap_w, s_cap_h, s_rot, s_out_w, s_out_h);
    return NULL;
}

/* 相机初始化线程:全部阻塞点(aiq 锁握手等)都关在这里,
 * 绝不拖累主循环——UI 渲染必须立刻起立(黑屏事故 2026-09-18) */
static void *cam_init_thread(void *arg)
{
    const char *sensor = arg;
    bool want_aiq = (getenv("DG_AIQ") == NULL || strcmp(getenv("DG_AIQ"), "0") != 0);

    pthread_t tid;
    if (pthread_create(&tid, NULL, stream_thread, NULL) != 0) {
        DG_LOGE(CAM_TAG, "取流线程创建失败");
        return NULL;
    }

    if (!want_aiq) {
        DG_LOGW(CAM_TAG, "DG_AIQ=0:跳过 3A,裸流出图");
        pthread_join(tid, NULL);
        return NULL;
    }

    /* uAPI2 的 sns_ent_name 是传感器实体名(非设备路径):传 /dev/mediaN
     * 会在实体查找处空指针崩溃(实测) */
    s_aiq = rk_aiq_uapi2_sysctl_init(sensor, env_or("DG_AIQ_IQDIR", "/etc/iqfiles"),
                                     NULL, NULL);
    if (!s_aiq) {
        DG_LOGW(CAM_TAG, "rkaiq init 失败(裸流继续,画面可能偏暗)");
        pthread_join(tid, NULL);
        return NULL;
    }
    if (rk_aiq_uapi2_sysctl_prepare(s_aiq, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) != 0) {
        DG_LOGW(CAM_TAG, "rkaiq prepare 失败(裸流继续)");
    } else if (rk_aiq_uapi2_sysctl_start(s_aiq) != 0) {
        DG_LOGW(CAM_TAG, "rkaiq start 失败(裸流继续)");
    } else {
        DG_LOGI(CAM_TAG, "rkaiq 3A 就绪(sensor=%s)", sensor);
        /* 门禁预览人物在走动:压曝光上限(默认 20ms=1/50s)换更高增益,
         * 运动模糊显著下降;DG_AE_MAX_MS 可调,0=不限制 */
        const char *e = getenv("DG_AE_MAX_MS");
        int max_ms = (e && e[0]) ? atoi(e) : 20;
        if (max_ms > 0) {
            paRange_t tr;
            if (rk_aiq_uapi2_getExpTimeRange(s_aiq, &tr) == 0) {
                tr.max = max_ms / 1000.0f;
                if (rk_aiq_uapi2_setExpTimeRange(s_aiq, &tr) == 0)
                    DG_LOGI(CAM_TAG, "AE 曝光上限 %dms(减运动模糊)", max_ms);
                else
                    DG_LOGW(CAM_TAG, "AE 曝光上限设置失败");
            }
        }
    }
    pthread_join(tid, NULL);
    return NULL;
}

int camera_init(const char *res_path, camera_frame_fn cb, void *ud)
{
    (void)res_path;
    (void)cb;
    (void)ud;

    const char *sensor = env_or("DG_AIQ_SENSOR", "m02_b_imx415 8-0037");
    DG_LOGI(CAM_TAG, "camera_init(后台线程): sensor=%s", sensor);

    s_rot = rot_from_env();
    s_out_w = (s_rot == 90 || s_rot == 270) ? CAM_OUT_W : s_cap_w;
    s_out_h = (s_rot == 90 || s_rot == 270) ? CAM_OUT_H : s_cap_h;
    for (int i = 0; i < 3; i++) {
        s_out[i] = malloc((size_t)s_out_w * s_out_h * 4);
        if (!s_out[i]) {
            DG_LOGE(CAM_TAG, "输出缓冲分配失败");
            return DG_ERR_NO_MEMORY;
        }
    }

    /* 全部可能阻塞的初始化放后台线程;camera_init 本身立即返回,
     * 主循环(渲染/触摸/认证)零等待 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, cam_init_thread, (void *)sensor) != 0) {
        DG_LOGE(CAM_TAG, "初始化线程创建失败");
        return DG_ERR_NO_MEMORY;
    }
    pthread_detach(tid);
    return DG_OK;
}

/* RGA:NV12 → XRGB(BGRA8888 字节序,与 LVGL 32 位直通),含旋转 */
static bool convert_frame(int buf_idx)
{
    /* NV12 Y 平面 1 字节/像素;stride 用驱动返回值(对齐可能大于宽) */
    rga_buffer_t src = wrapbuffer_virtualaddr_t(s_cap[buf_idx].addr, s_cap_w,
                                                s_cap_h, s_stride, s_cap_h,
                                                RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr_t(s_out[s_out_idx], s_out_w,
                                                s_out_h, s_out_w, s_out_h,
                                                RK_FORMAT_BGRA_8888);
    int rot_flag = (s_rot == 180)  ? IM_HAL_TRANSFORM_ROT_180
                   : (s_rot == 270) ? IM_HAL_TRANSFORM_ROT_270
                                    : (s_rot == 90 ? IM_HAL_TRANSFORM_ROT_90 : 0);
    IM_STATUS st = imrotate_t(src, dst, rot_flag, 1);
    /* librga 有两个成功码:SUCCESS=1 / NOERROR=2,只认其一会把成功当失败 */
    if (st != IM_STATUS_NOERROR && st != IM_STATUS_SUCCESS) {
        static int n_err;
        if (n_err++ < 3) {
            DG_LOGE(CAM_TAG, "RGA 失败(%s)", imStrError_t(st));
            if (n_err == 3)
                DG_LOGE(CAM_TAG, "RGA 连续失败,后续不再刷屏");
        }
        return false;
    }
    return true;
}

/* 归还缓冲(ROCKIVA 释放回调线程调用;V4L2 ioctl 内核侧串行化) */
static void qbuf_index(int idx)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = idx;
    buf.length = s_nplanes;
    buf.m.planes = planes;
    xioctl(s_vfd, VIDIOC_QBUF, &buf);
}

void camera_set_nv12_listener(camera_nv12_fn on_frame,
                              camera_nv12_release_fn on_release)
{
    s_nv12_fn = on_frame;
    s_nv12_rel = on_release;
}

void camera_poll(void)
{
    if (!s_ready || !s_stream_on)
        return;

    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = s_nplanes;
    buf.m.planes = planes;

    if (xioctl(s_vfd, VIDIOC_DQBUF, &buf) < 0)
        return; /* EAGAIN=暂无帧,轮询节奏本来就密 */

    int idx = buf.index;
    if (convert_frame(idx)) {
        if (getenv("DG_CAM_DUMP") && s_seq == 0) {
            FILE *f = fopen("/tmp/dg_cam.raw", "wb");
            if (f) {
                fwrite(s_out[s_out_idx], 1, (size_t)s_out_w * s_out_h * 4, f);
                fclose(f);
            }
            /* Y 均值粗判曝光:全黑(<8)或全白(>245)说明 3A/镜头异常 */
            const uint8_t *y = s_cap[idx].addr;
            unsigned long sum = 0;
            for (int i = 0; i < s_cap_w * s_cap_h; i++)
                sum += y[i];
            DG_LOGI(CAM_TAG, "首帧已导出 /tmp/dg_cam.raw,Y 均值=%lu", sum / (s_cap_w * s_cap_h));
        }
        /* 先投内容再轮转序号:读方见到新 seq 时缓冲必已完整 */
        s_frame.pixels = s_out[s_out_idx];
        s_frame.w = s_out_w;
        s_frame.h = s_out_h;
        s_out_idx = (s_out_idx + 1) % 3;
        s_frame.seq = ++s_seq;

        /* NV12 出口:视觉占用期间不归还,等 release 回调再 QBUF */
        if (s_nv12_fn) {
            s_busy[idx] = true;
            s_fid[idx] = s_frame.seq;
            s_nv12_fn(s_cap[idx].addr, s_cap_w, s_cap_h, s_frame.seq);
        }

        /* 帧率实测(DG_CAM_FPS_LOG=1):每 5s 报一次实际送达帧率 */
        if (getenv("DG_CAM_FPS_LOG")) {
            static uint32_t win_seq;
            static int64_t win_ms;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            if (win_ms == 0) {
                win_ms = ms;
                win_seq = s_seq;
            } else if (ms - win_ms >= 5000) {
                DG_LOGI(CAM_TAG, "实测帧率 %.1f fps(5s 窗口 %u 帧)",
                        (s_seq - win_seq) * 1000.0 / (ms - win_ms), s_seq - win_seq);
                win_ms = ms;
                win_seq = s_seq;
            }
        }
    }

    if (s_busy[idx])
        return;                          /* 视觉占用中,on_release 归还 */
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = idx;
    buf.length = s_nplanes;
    buf.m.planes = planes;
    xioctl(s_vfd, VIDIOC_QBUF, &buf);
}

void camera_nv12_release(uint32_t frame_id)
{
    for (int i = 0; i < CAM_BUF_CNT; i++) {
        if (s_busy[i] && s_fid[i] == frame_id) {
            s_busy[i] = false;
            qbuf_index(i);
            return;
        }
    }
}

const camera_frame_t *camera_latest(void)
{
    return s_seq ? &s_frame : NULL;
}
