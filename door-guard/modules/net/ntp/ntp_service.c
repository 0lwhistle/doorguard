/*
 * ntp_service.c — NTP 服务实现
 *
 * chrony(rootfs 已带)后台常驻;"执行一次" = chronyc burst 4/4 +
 * waitsync 10(spec-network §3)。联网检测:ping 外探。未联网/命令
 * 失败 → 明确失败结果,不重试到死。
 */
#include "ntp_service.h"
#include "dg_log.h"
#include "event_bus.h"
#include "events.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "[NTP]";

static bool s_running = false;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool probe_online(void)
{
    int rc = system("ping -c1 -W2 223.5.5.5 > /dev/null 2>&1");
    return rc == 0;
}

static void publish(bool ok, int err, int64_t ts)
{
    ev_ntp_result_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.ok = ok;
    ev.err = err;
    ev.synced_ts = ts;
    EVENT_BUS_PUBLISH(EV_NET_NTP_RESULT, &ev);
}

int ntp_service_trigger(void)
{
    pthread_mutex_lock(&s_mtx);
    if (!s_running) {
        pthread_mutex_unlock(&s_mtx);
        return DG_ERR_NOT_INIT;
    }
    pthread_mutex_unlock(&s_mtx);

    if (!probe_online()) {
        DG_LOGW(TAG, "设备未联网,NTP 校正失败");
        publish(false, DG_ERR_NETWORK, 0);
        return DG_ERR_NETWORK;
    }

    int rc = system("chronyc -a burst 4/4 > /dev/null 2>&1 && "
                    "chronyc waitsync 10 > /dev/null 2>&1");
    if (rc == 0) {
        DG_LOGI(TAG, "NTP 同步成功");
        publish(true, DG_OK, (int64_t)time(NULL));
        return DG_OK;
    }
    DG_LOGW(TAG, "NTP 同步失败 rc=%d", rc);
    publish(false, DG_ERR_NETWORK, 0);
    return DG_ERR_NETWORK;
}

static void *boot_trigger(void *arg)
{
    (void)arg;
    sleep(10);                              /* 等网络栈就绪 */
    if (probe_online()) {
        DG_LOGI(TAG, "开机联网检测通过,自动校正一次");
        ntp_service_trigger();
    } else {
        DG_LOGW(TAG, "开机未联网,跳过自动校正");
    }
    return NULL;
}

int ntp_service_start(void)
{
    if (s_running)
        return DG_OK;
    s_running = true;
    pthread_t tid;
    if (pthread_create(&tid, NULL, boot_trigger, NULL) == 0)
        pthread_detach(tid);
    DG_LOGI(TAG, "ntp 服务启动");
    return DG_OK;
}
