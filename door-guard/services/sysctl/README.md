# sysctl — 系统控制服务(设备重启)

## 职责

设备重启的**唯一执行点**。UI(设备管理页)与 web 上位机都发
`EV_SYS_REBOOT` 请求(写与命令走事件总线,architecture §1),本服务订阅
后延迟执行,经 `modules/sysctl` 原语真正重启。

## 流程

```
page_device / web POST /api/system/reboot
        │ EVENT_BUS_PUBLISH(EV_SYS_REBOOT{delay_ms})
        ▼
sysctl_service(去重:在途请求忽略新请求)
        │ 分离线程 sleep(delay_ms)     ← 不占用事件总线工作线程
        ▼
modules/sysctl sysctl_reboot()
        ├─ 板上:sync → system("reboot")(busybox,经 init 干净关停)
        │        └ 失败兜底 reboot(RB_AUTOBOOT) 系统调用
        └─ 宿主(DG_SIM/DG_BUILD_TESTS):模拟,打日志返回 DG_OK
```

## 关键决策

- **延迟执行**:web 回 202 后 1s、UI 弹提示后 1.5s 才真重启——回执/弹窗
  先落地。板上成功路径进程在关停中被杀,`sysctl_service_last_err()` 观察
  不到返回;能看到值即为失败或宿主模拟。
- **宿主不真重启**:sim/ctest 构建下 `sysctl_reboot()` 是模拟(WSL 里
  root 跑 sim 有先例,真执行会把宿主机带走)。
- 与 OTA A/B 切换正交:OTA 改槽位后由 S60 重拉应用;本服务是整机重启。

## 使用示例

```c
ev_sys_reboot_t ev = { .delay_ms = 1500 };
EVENT_BUS_PUBLISH(EV_SYS_REBOOT, &ev);
```

## 测试

`tests/test_sysctl.c`(宿主):请求受理 → 延迟执行 → last_err=DG_OK;
重复请求去重;服务生命周期 start/stop。
