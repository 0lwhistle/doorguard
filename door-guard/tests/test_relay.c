/*
 * test_relay.c — 继电器模块宿主测试(A4)
 *
 * 宿主无 GPIO:用 line=-1(未配置)走降级路径,不触碰真实 sysfs。
 * 钉死契约:降级装配恒成功、脉冲不崩、参数边界显式报错、复位幂等。
 */
#include "dg_test.h"
#include "relay.h"

int main(void)
{
    /* 未配置引脚(line<0):恒降级,装配成功 */
    DG_CHECK(relay_module_init(-1) == DG_OK);

    /* 降级脉冲:不崩、返回 OK(开门仅事件可观测) */
    DG_CHECK(relay_door_pulse(50) == DG_OK);
    DG_CHECK(relay_level(NULL) == DG_ERR_NOT_INIT);   /* 降级态无电平可读 */

    /* 参数边界:0 与超上限显式拒绝(降级态也拦) */
    DG_CHECK(relay_door_pulse(0) == DG_ERR_PARAM);
    DG_CHECK(relay_door_pulse(10001) == DG_ERR_PARAM);

    /* 复位:降级态幂等成功 */
    DG_CHECK(relay_reset() == DG_OK);
    DG_CHECK(relay_reset() == DG_OK);

    /* 回收后再脉冲:回到降级路径,不崩 */
    relay_module_deinit();
    DG_CHECK(relay_door_pulse(50) == DG_OK);

    DG_TEST_EXIT();
}
