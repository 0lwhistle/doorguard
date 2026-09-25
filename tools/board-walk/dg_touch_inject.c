/* dg_touch_inject.c v2 — LD_PRELOAD 触摸注入库
 * 协议:向 /tmp/dg_touch 追加写 "P x y"(按下,屏幕域 720x1280)或 "R"(抬起),每条一行。
 * v2:命令文件整读解析为动作队列,库内按 EMIT_MS 限速逐动作发射(P/R 天然分帧,
 *     杜绝 v1 覆盖写竞态丢命令);消费后 unlink 命令文件。
 * 事件走真实 evdev 解析/校准/按下沿链路(Type-B MT 流)。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/input.h>

static ssize_t (*real_read)(int, void *, size_t);
static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);

#define MAX_EVENT_FDS 16
#define ACT_MAX       128
#define EMIT_EV_MAX   16
#define EMIT_MS_P     200   /* R→P 间隔(消费耗时是主项,弹窗稳态 P 帧 ~440ms) */
#define EMIT_MS_R     80    /* P→R 间隔:读空后尽快抬起,PRESSED 时长 ~0.5s,须 <400ms 长按阈值——读空条件保证分帧,80ms 缓冲即可 */
#define CMD_FILE      "/tmp/dg_touch"
#define LOG_FILE      "/tmp/dg_inject.log"

static int g_event_fds[MAX_EVENT_FDS];
static int g_event_fd_cnt;
static int g_touch_fd = -1;
static int g_fd_kind[MAX_EVENT_FDS]; /* 0=未知 1=触摸 2=非触摸 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct act { int press; int x, y; } g_acts[ACT_MAX];
static int g_act_head, g_act_cnt;
static struct input_event g_evs[EMIT_EV_MAX];
static int g_ev_head, g_ev_cnt;
static long g_last_emit_ms;
static int g_pressed;
static FILE *g_log;

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void ilog(const char *fmt, ...)
{
    if (!g_log) {
        g_log = fopen(LOG_FILE, "a");
        if (!g_log) return;
    }
    fprintf(g_log, "[%ld.%03ld] ", now_ms() % 100000, 0L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void evpush(__u16 type, __u16 code, __s32 val)
{
    if (g_ev_cnt >= EMIT_EV_MAX) return;
    struct input_event *ev = &g_evs[(g_ev_head + g_ev_cnt) % EMIT_EV_MAX];
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->code = code;
    ev->value = val;
    g_ev_cnt++;
}

/* 发射一个动作:展开为 MT 事件帧(精简:gesture 关闭,SLOT/BTN 冗余;
 * 弹窗+预览软渲染下事件消费 ~110ms/个,帧越短点击越快) */
static void emit_act(const struct act *a)
{
    if (a->press) {
        ilog("act press %d,%d", a->x, a->y);
        evpush(EV_ABS, ABS_MT_TRACKING_ID, 1);
        evpush(EV_ABS, ABS_MT_POSITION_X, a->x);
        evpush(EV_ABS, ABS_MT_POSITION_Y, a->y);
        evpush(EV_SYN, SYN_REPORT, 0);
        g_pressed = 1;
    } else {
        if (!g_pressed) return; /* 无人按下时的 R 直接丢弃 */
        ilog("act release");
        evpush(EV_ABS, ABS_MT_TRACKING_ID, -1);
        evpush(EV_SYN, SYN_REPORT, 0);
        g_pressed = 0;
    }
    g_last_emit_ms = now_ms();
}

/* 整读命令文件 → 动作队列;消费后 ftruncate 清空而非 unlink——v4 修复:
 * 旧版读空即 unlink,writer 已 open 未 write 时被断链,写入进孤儿 inode
 * 全丢(板上 step3 实测 UID 第 2 键起全丢);truncate 保持 writer 的
 * append fd 有效,根治竞态 */
static void feed_acts(void)
{
    int fd = open(CMD_FILE, O_RDWR);
    if (fd < 0) return;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';

    int added = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (g_act_cnt >= ACT_MAX) break;
        struct act *a = &g_acts[(g_act_head + g_act_cnt) % ACT_MAX];
        int x = 0, y = 0;
        if (line[0] == 'P' && sscanf(line, "P %d %d", &x, &y) == 2) {
            a->press = 1; a->x = x; a->y = y;
            g_act_cnt++; added++;
        } else if (line[0] == 'R') {
            a->press = 0;
            g_act_cnt++; added++;
        }
    }
    if (added) {
        if (lseek(fd, 0, SEEK_SET) == 0)
            ftruncate(fd, 0);
        ilog("fed %d acts (queue=%d)", added, g_act_cnt);
    }
    close(fd);
}

static int classify_fd(int fd);

static void *emitter_thread(void *arg);

static int classify_fd(int fd)
{
    for (int i = 0; i < g_event_fd_cnt; i++) {
        if (g_event_fds[i] == fd) {
            if (g_fd_kind[i]) return g_fd_kind[i] == 1 ? 1 : 0;
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define ABS_MT_SLOT 0x2f
#endif
            unsigned long bits[(ABS_MT_TRACKING_ID + 63) / 64] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) > 0) {
                int has_mt = (bits[ABS_MT_POSITION_X / 64] >> (ABS_MT_POSITION_X % 64)) & 1UL;
                g_fd_kind[i] = has_mt ? 1 : 2;
                ilog("fd %d has_mt=%d", fd, has_mt);
                return has_mt ? 1 : 0;
            }
            g_fd_kind[i] = 2;
            return 0;
        }
    }
    return -1;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (!real_read) {
        real_read = dlsym(RTLD_NEXT, "read");
        if (!real_read) { errno = ENOSYS; return -1; }
    }
    if (fd >= 0 && count >= sizeof(struct input_event) && g_touch_fd != -2) {
        int k = classify_fd(fd);
        if (k < 0) goto real;
        if (k == 1) {
            if (g_touch_fd == -1) {
                g_touch_fd = fd;
                ilog("touch fd=%d taken over", fd);
                pthread_t t;
                if (pthread_create(&t, NULL, emitter_thread, NULL) == 0)
                    pthread_detach(t);
            }
            if (fd == g_touch_fd) {
                pthread_mutex_lock(&g_lock);
                ilog("rd ev=%d act=%d", g_ev_cnt, g_act_cnt);
                if (g_ev_cnt > 0) {
                    struct input_event ev = g_evs[g_ev_head];
                    g_ev_head = (g_ev_head + 1) % EMIT_EV_MAX;
                    g_ev_cnt--;
                    pthread_mutex_unlock(&g_lock);
                    memcpy(buf, &ev, sizeof(ev));
                    return sizeof(ev);
                }
                pthread_mutex_unlock(&g_lock);
                errno = EAGAIN;
                return -1;
            }
        }
    }
real:
    return real_read(fd, buf, count);
}

/* 发射线程:节奏完全由库控制(150ms/动作),不受 LVGL indev 空闲降频影响 */
static void *emitter_thread(void *arg)
{
    (void)arg;
    static long ev_busy_since;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (g_touch_fd != -1 && g_ev_cnt == 0 && g_act_cnt == 0) feed_acts();
        if (g_ev_cnt > 0 && !ev_busy_since) ev_busy_since = now_ms();
        if (g_ev_cnt == 0 && ev_busy_since) {
            ilog("drain %ld ms", now_ms() - ev_busy_since);
            ev_busy_since = 0;
        }
        if (g_touch_fd != -1 && g_ev_cnt == 0 && g_act_cnt > 0) {
            long need = g_acts[g_act_head].press ? EMIT_MS_P : EMIT_MS_R;
            if (now_ms() - g_last_emit_ms >= need) {
                emit_act(&g_acts[g_act_head]);
                g_act_head = (g_act_head + 1) % ACT_MAX;
                g_act_cnt--;
            }
        }
        pthread_mutex_unlock(&g_lock);
        usleep(10000);
    }
    return NULL;
}

static void track_open(const char *path, int fd)
{
    if (!path || fd < 0) return;
    if (strncmp(path, "/dev/input/event", 16) != 0) return;
    pthread_mutex_lock(&g_lock);
    if (g_event_fd_cnt < MAX_EVENT_FDS) {
        g_event_fds[g_event_fd_cnt++] = fd;
        ilog("open %s -> fd %d", path, fd);
    }
    pthread_mutex_unlock(&g_lock);
}

int open(const char *path, int flags, ...)
{
    if (!real_open) {
        real_open = dlsym(RTLD_NEXT, "open");
        if (!real_open) { errno = ENOSYS; return -1; }
    }
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = real_open(path, flags, mode);
    track_open(path, fd);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    if (!real_openat) {
        real_openat = dlsym(RTLD_NEXT, "openat");
        if (!real_openat) { errno = ENOSYS; return -1; }
    }
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = real_openat(dirfd, path, flags, mode);
    track_open(path, fd);
    return fd;
}

__attribute__((constructor)) static void init(void)
{
    real_read = dlsym(RTLD_NEXT, "read");
    real_open = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    ilog("inject lib v2 loaded");
}
