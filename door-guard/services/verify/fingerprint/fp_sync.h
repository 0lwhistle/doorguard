/*
 * fp_sync.h — 指纹两侧数量一致性(启动检查)
 *
 * 背景:模组 flash 与板端 DB 是两个独立的持久存储,不存在跨设备事务——
 * 换模组、掉电、串口抖动都会让两侧漂移(详见 DEVLOG 2026-10-11 与
 * docs/tech/FINGERPRINT_PROTOCOL.md)。本模块只做**判定**,不做 IO:
 * provider 读到两个数,交给 fp_sync_decide() 拿处置结论,再自己执行。
 * 拆开是为了让边界用例(0/0、0/N、N/0、N/M)能纯宿主单测。
 *
 * 处置规则(用户拍板 2026-10-11,含边界):
 *   n_db=0, n_mod=0            → OK(全新设备,无动作)
 *   n_db=0, n_mod>0            → 清模组:那些模板无归属(DB 是映射唯一事实源),
 *                                 永远不可能认证成功 = 孤儿,删之无损
 *   n_db>0, n_mod=0            → 清 DB:模组空库意味着这些行永远验不过。
 *                                 原计划改为"把 DB 副本回灌进新模组",但板上实测
 *                                 UpChar/UpImage 均只回 ACK 0x00、不吐数据包(该模组
 *                                 上传指令为空实现)→ DB 的 finger_vec 副本永远是
 *                                 NULL → 无可注入之物,回灌不可行 → 按用户备选
 *                                 决策延续原规则清 DB
 *   n_db>0, n_mod>0, n_db!=n_mod → 只告知(弹窗建议清除所有指纹),不动手
 *   n_db==n_mod (>0)           → OK
 *
 * 纪律:**只有"成功读到"的两个数才进 decide()**。读取失败(链路错/超时/
 * 非 0x00 确认码)一律不得折算成 0——一次串口抖动就会把全部门禁指纹抹掉。
 * 读失败由 provider 走重试→强制健康检查(见 fp_provider seq_sync_check)。
 */
#ifndef DG_FP_SYNC_H
#define DG_FP_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一致性结论(执行动作由调用方按本值分派) */
typedef enum {
    FP_SYNC_OK           = 0, /**< 一致,或无动作 */
    FP_SYNC_CLEAR_MODULE = 1, /**< 模组>0 且 DB=0 → 清模组 */
    FP_SYNC_CLEAR_DB     = 2, /**< DB>0 且 模组=0 → 清 DB */
    FP_SYNC_MISMATCH     = 3, /**< 都非零但不等 → 仅告知 */
} fp_sync_verdict_t;

/** 纯判定:n_db = fingerprints 行数,n_mod = 模组 ValidTempleteNum */
fp_sync_verdict_t fp_sync_decide(uint32_t n_db, uint32_t n_mod);

/** 开机检查结果 → UI 弹窗的告知种类(取走即清,一次只弹一次) */
typedef enum {
    FP_NOTICE_NONE           = 0,
    FP_NOTICE_CLEARED_MODULE = 1, /**< 已自动清空模组指纹库(去孤儿) */
    FP_NOTICE_CLEARED_DB     = 2, /**< 已自动清空指纹记录(模组为空库) */
    FP_NOTICE_MISMATCH       = 3, /**< 两侧数量不一致,建议清除所有指纹 */
    FP_NOTICE_READ_FAIL      = 4, /**< 读不到模组数量,本次未做检查 */
} fp_sync_notice_t;

/** 最近一次检查快照(UI/日志观测用) */
typedef struct {
    bool     checked;          /**< 是否完成过一次判定 */
    uint32_t n_db;             /**< 板端行数 */
    uint32_t n_mod;            /**< 模组模板数 */
    int32_t  verdict;          /**< fp_sync_verdict_t */
    int32_t  err;              /**< 执行动作的返回码(0 = 成功/无动作) */
    int64_t  at_ms;            /**< 判定时刻(单调 ms) */
} fp_sync_status_t;

/** 快照(线程安全;任何线程可调) */
void fp_sync_status(fp_sync_status_t *out);

/**
 * 取走待告知(线程安全)。@return true = *out_notice 有效且已清空槽位。
 * 同一次检查只产生一条告知;UI 就绪前产生的会被保留到 UI 来取——
 * event_bus 不回放,所以弹窗走"拉"而不是"推"。
 */
bool fp_sync_take_notice(int32_t *out_notice);

/* ---- provider 侧写入(仅 fp_provider 调用) ---- */

/** 记一次判定结果(不产生告知;供状态快照) */
void fp_sync_publish(uint32_t n_db, uint32_t n_mod, int32_t verdict,
                     int32_t err, int64_t at_ms);

/** 压一条待告知。优先级:动作类(1/2)> 不一致(3)> 读失败(4);
 *  高优先级不被低优先级顶掉,同级后者覆盖前者。 */
void fp_sync_push_notice(int32_t notice);

#ifdef __cplusplus
}
#endif

#endif /* DG_FP_SYNC_H */
