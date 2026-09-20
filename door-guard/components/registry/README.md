# registry — services 注册表(架构 v2 M2③)

与 `components/holder`(modules 注册表)同构的服务注册表,分层职责不同:
holder 管 modules(设备/中间层),registry 管 services(顶层业务)。
测试:`tests/test_registry.c`(纯逻辑,宿主可跑)。

## 出处与差异

按 holder 的 API 形态同构实现(注册/依赖分批初始化/循环依赖检测/状态机
REGISTERED→INITIALIZING→READY/ERROR/DISABLED)。相对 holder 的增量:

- **心跳钩子**:注册时挂 `registry_heartbeat_fn`(返回最近活动 unix ms),
  看门狗据此判活;无常驻线程的服务不挂(仅状态监控)。当前接线:web
  (`web_server_heartbeat_ms`,推送线程每次唤醒刷新)。
- **restart / mark_disabled / restart_count**:看门狗"重启一次 → 仍异常置
  DISABLED"降级策略的原语。
- **依赖解析器注入**:`registry_set_dep_resolver()`——服务依赖的 modules 在
  holder 表里,由装配层(app/main.c `dep_ready`)桥接,组件本身保持通用。
- **is_required / watchdog_poll**:巡检判定(OK/STALE/ERROR)与必需性查询,
  处置策略(停机/禁用)归调用方。

## 使用示例

```c
registry_init();
registry_set_dep_resolver(dep_ready);            // 跨表解析 holder 就绪状态
registry_register("web", web_start, false,
                  (const char *const[]){ "config" }, 1,
                  web_server_heartbeat_ms, NULL);
registry_init_all(true);                          // 必需失败 → REG_ERR_DEPENDENCY

// 看门狗循环里:
if (registry_state("web") == REG_STATE_ERROR && !registry_is_required("web")) {
    if (registry_restart_count("web") < 1)
        registry_restart("web");                  // 重启一次
    else {
        registry_mark_disabled("web");            // 仍异常 → 禁用 + 事件通知
        EVENT_BUS_PUBLISH(EV_SYS_SERVICE_STATE, &ev);
    }
}
```

## 移植纪律(同 holder)

init_fn 必须简短、不得回调本表查询(注册表互斥锁在启动期间被持有);
依赖名既可以是 registry 表内服务,也可以是经解析器放行的跨表模块名。
