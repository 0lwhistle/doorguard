/*
 * enroll_service.h — 用户/特征录入编排(architecture §1)
 *
 * 流程(2026-09-27 起为**两段式草稿**):EV_ENROLL_REQUEST → 视觉抓取
 * (EV_VISION_CAPTURE_REQ → EV_VISION_FEATURE 槽位回执)→ 特征与头像
 * **暂存服务内草稿槽,不落库**(UI 即时预览,编辑页「保存」才提交)→
 * EV_ENROLL_RESULT(OK = 草稿就绪)。落库时机由 UI 决定:
 *   commit_draft  = 保存(查重+DB+特征库+头像一次完成)
 *   discard_draft = 放弃(安全擦除)
 * 错误码对齐 spec-database §2。
 */
#ifndef DG_ENROLL_SERVICE_H
#define DG_ENROLL_SERVICE_H

#include "err.h"
#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int enroll_service_start(void);
void enroll_service_stop(void);

/* ---- 用户生命周期读/写(2026-09-28 A1 收口:UI 三页不再直写 SQLite)----
 * 编辑/列表/记录页原直调 db_user_* 与 db_log_query,越层写路径未登记;
 * 现统一收进本服务(它本就管草稿/删除/清脸,是事实上的用户生命周期服务)。
 * 错误码原样透传 storage,UI 侧 err_text() 映射不变;同步保存要同步返回码
 * 做弹窗回显与「保存退出」衔接,故登记为直调例外而非事件往返
 * (docs/architecture-v2-proposal §1)。 */

/** 单用户读取(编辑页刷新/保存前置;透传 db_user_get) */
int enroll_service_user_get(const char *user_id, user_rec_t *out);

/** 列表页单行投影(不含特征/密码;头像走 dg_avatar 独立链路) */
typedef struct {
    char    user_id[DG_UID_LEN];
    char    user_name[DG_NAME_LEN];
    int32_t role;
} enroll_user_row_t;

/** 分页读取上限(user_id 字典序取前 cap 行;UI 页容量 6,留裕量) */
#define ENROLL_USER_PAGE_MAX 16

/** 列表页一次取齐:字典序 cap 行 + 用户总数(替代 UI 的
 *  count+list_ids+逐行 get 三连调)。rows 调用方分配;out_n 实际行数 */
int enroll_service_user_page(enroll_user_row_t *rows, uint32_t cap,
                             uint32_t *out_n, uint32_t *out_total);

/** 门禁日志分页查询(记录查询页数据源;q/out 由调用方提供,透传 db_log_query) */
int enroll_service_log_query(const log_query_t *q, log_page_t *out);

/** 用户生命周期保存(编辑页「保存」唯一入口;字段+密码一次落库):
 *  uid 不存在 = 建用户:密码必填(空 → DG_ERR_NO_PASSWORD),auth_flags
 *  固定 FACE|PWD——指纹/IC 事件两端未接硬件,放开只会让用户白录白验
 *  (机制见 spec-auth-business「验证方式开关」);
 *  已存在   = 覆写姓名/权限;pwd 非空才改密(空/NULL = 保持原密码)。
 *  @return DG_OK;失败码透传 storage(DUP_UID/NO_PASSWORD/BAD_NAME/…) */
int enroll_service_user_save(const char *user_id, const char *name,
                             int32_t role, const char *pwd);

/* ---- 人脸草稿(单槽;UI 线程直调,登记见 docs/architecture-v2-proposal §1) ---- */

/** 该用户是否有人脸草稿(已采集未保存) */
bool enroll_service_draft_active(const char *user_id);

/** 取草稿头像 JPEG 明文指针(仅 LVGL 线程在拍摄/编辑页内使用,取用期间
 *  不得并发 commit/discard;无草稿返回 false)
 *  @return true = *jpeg 与 *len 有效(内部静态缓冲,下次采集前稳定) */
bool enroll_service_draft_avatar(const char *user_id, const uint8_t **jpeg,
                                 size_t *len);

/** 提交草稿:查重 → 特征落库 → 特征库 INSERT → 头像落库。
 *  @return DG_OK 成功(草稿已消费);失败码(DUP_FACE/DB 等)时草稿保留,
 *          由 UI 决定重拍(覆盖)或放弃(discard) */
int enroll_service_commit_draft(const char *user_id);

/** 丢弃草稿(明文安全擦除);无草稿时静默成功 */
void enroll_service_discard_draft(const char *user_id);

/** 清除已录人脸(保留用户;DB len=0 + 特征库 DELETE + 头像清空)。
 *  同步版,供 UI「保存」路径直调;web 侧仍走 DG_ENROLL_FACE_CLEAR 事件 */
int enroll_service_clear_face(const char *user_id);

/* ---- 指纹/IC 查询包装(2026-10-01;UI 不直触 storage,同 user_get 惯例) ---- */

/** 该用户已录指纹 PageID 列表(编辑页 n/3 与逐枚删除数据源;≤3 枚)。
 *  cap < 实际数 → DG_ERR_NO_MEMORY;用户无指纹返回 DG_OK 且 *out_n=0 */
int enroll_service_finger_pages(const char *user_id, int32_t *pages,
                                uint32_t cap, uint32_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* DG_ENROLL_SERVICE_H */
