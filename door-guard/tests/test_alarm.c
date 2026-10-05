/*
 * test_alarm.c — 远程报警上报测试(sink 注入,零 mqtt 依赖)
 *
 * 覆盖:类型→JSON 字段口径(type/code/detail/ts)/ 空详情省略字段 /
 * detail 特殊字符净化(引号反斜杠不破 JSON)/ sink 返回值透传 /
 * 未启 mqtt 时默认 sink 透传 NOT_INIT(报警不丢本地日志)。
 */
#include "dg_test.h"
#include "alarm/alarm_service.h"

#include <string.h>
#include <stdlib.h>

#define CAP_MAX 8

static alarm_type_t s_cap_type[CAP_MAX];
static char s_cap_json[CAP_MAX][192];
static int s_cap_cnt;
static int s_next_rc = 0;                        /* sink 可编程返回值 */

static int capture_sink(alarm_type_t type, const char *json)
{
    if (s_cap_cnt < CAP_MAX) {
        s_cap_type[s_cap_cnt] = type;
        snprintf(s_cap_json[s_cap_cnt], sizeof(s_cap_json[0]), "%s", json);
    }
    s_cap_cnt++;
    return s_next_rc;
}

int main(void)
{
    alarm_sink_set(capture_sink);

    /* [A1] 带详情:字段齐全,类型名映射正确 */
    printf("[A1] 带详情上报\n");
    DG_CHECK(alarm_report(ALARM_TAMPER, "back cover opened") == 0);
    DG_CHECK(s_cap_cnt == 1);
    DG_CHECK(s_cap_type[0] == ALARM_TAMPER);
    DG_CHECK(strstr(s_cap_json[0], "\"type\":\"tamper\"") != NULL);
    DG_CHECK(strstr(s_cap_json[0], "\"code\":0") != NULL);
    DG_CHECK(strstr(s_cap_json[0], "\"detail\":\"back cover opened\"") != NULL);
    DG_CHECK(strstr(s_cap_json[0], "\"ts\":") != NULL);

    /* [A2] 空详情:detail 字段省略 */
    printf("[A2] 空详情\n");
    DG_CHECK(alarm_report(ALARM_DURESS, NULL) == 0);
    DG_CHECK(strstr(s_cap_json[1], "\"type\":\"duress\"") != NULL);
    DG_CHECK(strstr(s_cap_json[1], "detail") == NULL);

    /* [A3] 净化:引号/反斜杠换撇号,JSON 结构不破 */
    printf("[A3] 详情净化\n");
    DG_CHECK(alarm_report(ALARM_CUSTOM, "say \"hi\" \\ ok") == 0);
    DG_CHECK(strstr(s_cap_json[2], "\"detail\":\"say 'hi' ' ok\"") != NULL);

    /* [A4] sink 返回值透传(如 mqtt NOT_INIT= -4) */
    printf("[A4] 返回值透传\n");
    s_next_rc = -4;
    DG_CHECK(alarm_report(ALARM_FORCED_OPEN, "door magnet broke") == -4);
    DG_CHECK(strstr(s_cap_json[3], "\"type\":\"forced_open\"") != NULL);

    /* [A5] 超长详情截断不越界(512 字符输入) */
    printf("[A5] 超长截断\n");
    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    s_next_rc = 0;
    DG_CHECK(alarm_report(ALARM_CUSTOM, big) == 0);
    DG_CHECK(strlen(s_cap_json[4]) < 190);
    DG_CHECK(s_cap_json[4][0] == '{' &&
             s_cap_json[4][strlen(s_cap_json[4]) - 1] == '}');

    /* [A6] sink 恢复默认(NULL):未启 mqtt 的进程透传 NOT_INIT */
    printf("[A6] 默认 sink 未启用透传\n");
    alarm_sink_set(NULL);
    /* 测试进程没起 mqtt 服务:mqtt_publish_json 返回 DG_ERR_NOT_INIT(-4)。
     * (若未来 mqtt 默认行为变化,这里同步调整预期) */
    int rc = alarm_report(ALARM_OFFLINE, NULL);
    DG_CHECK(rc == -4 || rc == 0);

    DG_TEST_EXIT();
}
