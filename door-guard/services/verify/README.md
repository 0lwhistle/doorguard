# services/verify — 验证方式 provider(指纹/IC/人脸底层链路)

按验证方式分目录,统一「provider 契约」:发布协议事件、提供 1:1/1:N 的
服务层入口;FSM 编排(access_service)只认事件与方式位,不直接触硬件。

- `fingerprint/`:AS608 串口协议(fp_as608)+ provider(录入/删除/检索/
  WAK 按压沿);纯协议单测 `test_fp_proto`、录入链 `test_fp_enroll`、
  防重窗 `test_fp_press_gate`、同步对账 `test_fp_sync`;
- `ic/`:RC522 用户态 daemon 链路(card_provider;SPI 由
  `tools/iccard_daemon` 独立进程吐卡号,经 pipe 注入);
  **IC 链路当前挂起**(`iccard.enabled=0` + S60 flag 双门控,资产保留);
- `face/`:保留空目录(人脸底层在 services/vision,验签/质量阈值同源)。

provider 侧细节见各子目录头注与 `FINGERPRINT_AS608.md`。
