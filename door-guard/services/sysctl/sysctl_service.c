/*
 * sysctl_service.c — 系统控制服务实现(设备重启)
 *
 * 延迟执行:EV_SYS_REBOOT 带 delay_ms(HTTP 回执/弹窗先落地再重启)。
 * 不在事件总线工作线程里睡——起一次性分离线程睡够再执行,总线上高频
 * 事件(视觉帧/质量)不被拖住。并发去重:已有待执行请求时忽略新请求
 * (UI+web 同时触发只重启一次,后到者无需排队)。
 */
#include "sysctl_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"
#include "sysctl.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[SYSCTL]";

static event_subscription_t *s_sub = NULL;
static atomic_int s_pending;                     /* 0/1:重启线程在途 */
static atomic_int_fast32_t s_last_err;           /* SYSCTL_ERR_NONE=未执行过 */

int32_t sysctl_service_last_err(void)
{
    return (int32_t)atomic_load(&s_last_err);
}

static void *reboot_thread(void *arg)
{
    const unsigned delay_ms = (unsigned)(uintptr_t)arg;
    if (delay_ms)
        usleep(delay_ms * 1000u);

    const int rc = sysctl_reboot();
    atomic_store(&s_last_err, rc);
    /* 板上成功路径走不到这(系统关停中);能看到返回即失败或宿主模拟 */
    if (rc != DG_OK)
        DG_LOGE(TAG, "重启执行失败(%d)", rc);
    atomic_store(&s_pending, 0);
    return NULL;
}

static int on_reboot_req(const event_t *e, void *ud)
{
    (void)ud;
    const ev_sys_reboot_t *r = (const ev_sys_reboot_t *)e->data;

    int expect = 0;
    if (!atomic_compare_exchange_strong(&s_pending, &expect, 1)) {
        DG_LOGW(TAG, "重启已在执行队列,忽略重复请求(delay=%ums)",
                (unsigned)r->delay_ms);
        return 0;
    }
    atomic_store(&s_last_err, SYSCTL_ERR_NONE);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&tid, &attr, reboot_thread,
                                  (void *)(uintptr_t)r->delay_ms);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_store(&s_pending, 0);
        DG_LOGE(TAG, "重启线程创建失败(%d)", rc);
        return 0;
    }
    DG_LOGI(TAG, "重启请求已受理,将在 %ums 后执行", (unsigned)r->delay_ms);
    return 0;
}

int sysctl_service_start(void)
{
    if (s_sub)
        return DG_OK;
    atomic_init(&s_pending, 0);
    atomic_init(&s_last_err, SYSCTL_ERR_NONE);
    s_sub = event_bus_subscribe(EV_SYS_REBOOT, on_reboot_req, NULL);
    if (!s_sub)
        return DG_ERR_NOT_INIT;
    DG_LOGI(TAG, "sysctl 服务启动");
    return DG_OK;
}

void sysctl_service_stop(void)
{
    if (s_sub) {
        event_bus_unsubscribe(s_sub);
        s_sub = NULL;
    }
}
