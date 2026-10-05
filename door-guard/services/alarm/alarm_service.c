/*
 * alarm_service.c — 远程报警上报实现
 *
 * 极薄:类型名/详情拼 JSON → 发送(mqtt 邮箱投递)。触发源(防拆/胁迫/
 * 门磁)硬件与业务尚未接入,接入时 include 本头调 alarm_report 即可,
 * 不再各自拼主题——主题命名与字段口径收口在此处。
 */
#include "alarm_service.h"
#include "mqtt_service.h"
#include "dg_log.h"
#include "timeutil.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "[ALARM]";

#define ALARM_DETAIL_MAX 96

static const char *type_name(alarm_type_t t)
{
    switch (t) {
    case ALARM_TAMPER:      return "tamper";
    case ALARM_FORCED_OPEN: return "forced_open";
    case ALARM_DURESS:      return "duress";
    case ALARM_OFFLINE:     return "offline";
    case ALARM_CUSTOM:      return "custom";
    default:                return "unknown";
    }
}

/* JSON 字符串净化:与 mqtt_service 同款口径(引号/反斜杠换撇号,控制字符丢)。
 * 不直接复用 mqtt 内部 static:跨文件暴露净化工具是后续重构项,先按最小
 * 依赖落(报警路径 96 字符上限,重复 12 行可接受) */
static void json_sanitize(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; p && *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
            dst[o++] = '\'';
        else if (c >= 0x20)
            dst[o++] = *p;
    }
    dst[o] = '\0';
}

/* 默认 sink:mqtt <p>/event/alarm(返回值原样透传,调用方自决提示) */
static int send_mqtt(alarm_type_t type, const char *json)
{
    (void)type;
    return mqtt_publish_json("event/alarm", json, false);
}

static alarm_send_fn s_sink = send_mqtt;
static pthread_mutex_t s_sink_mtx = PTHREAD_MUTEX_INITIALIZER;

int alarm_report(alarm_type_t type, const char *detail)
{
    char det[ALARM_DETAIL_MAX];
    json_sanitize(det, sizeof(det), detail);

    char json[ALARM_DETAIL_MAX + 64];
    if (det[0])
        snprintf(json, sizeof(json),
                 "{\"type\":\"%s\",\"code\":%d,\"detail\":\"%s\",\"ts\":%lld}",
                 type_name(type), (int)type, det,
                 (long long)(now_s()));
    else
        snprintf(json, sizeof(json),
                 "{\"type\":\"%s\",\"code\":%d,\"ts\":%lld}",
                 type_name(type), (int)type,
                 (long long)(now_s()));

    DG_LOGW(TAG, "报警 %s(%d)%s%s", type_name(type), (int)type,
            det[0] ? ": " : "", det);

    pthread_mutex_lock(&s_sink_mtx);
    alarm_send_fn fn = s_sink;
    pthread_mutex_unlock(&s_sink_mtx);
    return fn ? fn(type, json) : DG_ERR_NOT_INIT;
}

void alarm_sink_set(alarm_send_fn fn)
{
    pthread_mutex_lock(&s_sink_mtx);
    s_sink = fn ? fn : send_mqtt;
    pthread_mutex_unlock(&s_sink_mtx);
}

int alarm_service_start(void)
{
    DG_LOGI(TAG, "报警上报就绪(<prefix>/event/alarm;触发源待业务接入)");
    return DG_OK;
}

void alarm_service_stop(void)
{
}
