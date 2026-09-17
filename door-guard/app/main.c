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
#include <sys/utsname.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    struct utsname u;
    uname(&u);
    printf("door-guard 冒烟版:交叉编译链路通了!\n");
    printf("  arch=%s kernel=%s\n", u.machine, u.release);
    printf("  pid=%d\n", getpid());
    /* TODO(B8): 按上述顺序装配各模块 */
    return 0;
}
