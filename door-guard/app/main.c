/*
 * door-guard main — 模块装配与生命周期
 *
 * 启动顺序(故障即退出,由 systemd 拉起重试):
 *   1. 配置加载(configs/device.json)
 *   2. HAL 初始化:camera → display → npu → storage → gpio/uart
 *   3. 服务启动:capture(取流) → vision(ROCKIVA) → liveness → access
 *   4. UI 启动(LVGL,订阅事件总线)
 *   5. 主循环:事件分发
 *
 * 模块间只经 proto/ 定义的队列与事件总线通信,不直接互调。
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    printf("door-guard starting...\n");
    /* TODO(B8): 按上述顺序装配各模块 */
    return 0;
}
