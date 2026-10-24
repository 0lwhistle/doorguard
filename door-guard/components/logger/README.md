# components/logger — dg_log 统一日志

ESP-IDF 模板 `esp_log` 的移植(Linux/pthread),全仓唯一日志门面。
分域 TAG(`[WEB]`/`[OTA]`/`[KBD]`…)按模块前缀约定命名。

- 级别:E/W/I/D,运行期由 env `DG_LOG_LEVEL` 调;
- 输出:stderr(板上由 S60 重定向进 `/var/log/door-guard.log`);
- 零依赖叶子:不反依任何组件,任何层可 include(与 timeutil 同纪律)。

测试:无独立用例(纯格式化输出),由全部测试的日志通路间接覆盖。
