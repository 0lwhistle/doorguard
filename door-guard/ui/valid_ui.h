/*
 * valid_ui.h — 输入弹窗用的校验包装(规则在 proto/valid.h,文案在本层)
 *
 * 为什么单独一层:规则(长度/字符集)是业务契约,必须与存储层同一份;
 * 而「给用户看的错误文案」要过 i18n、属 UI 层。这里把 规则→文案 收在一处,
 * 供 presenter_home(验证流程)与 page_users(用户管理)共用,避免两处各写一套。
 *
 * 用法:直接作为 dg_popup_input_cfg_t.validate 传入(返回 NULL = 通过)。
 */
#ifndef DG_UI_VALID_H
#define DG_UI_VALID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 用户 ID 校验(3~31 位字母/数字/-/_,字母或数字开头) */
const char *dg_ui_valid_uid(const char *text);

/** 姓名校验(1~63 字节,非空/无前后空格/无控制字符) */
const char *dg_ui_valid_name(const char *text);

/** 密码校验(4~31 位可见字符,不含空格) */
const char *dg_ui_valid_pwd(const char *text);

/** 录入/特征操作错误码 → 用户文案(UI 唯一映射点,2026-10-03 自编辑页
 *  收编共用:指纹管理页与用户编辑页同一套说法)。未知码给通用失败语 */
const char *dg_ui_enroll_err_text(int rc);

/* ---- 角色 → 展示文案(用户列表/编辑页共用,C6-5 收编唯一映射点) ---- */
/** dg_role_t → 文案(普通/管理员/黑名单;未知值回退「普通」) */
const char *ui_role_name(int32_t role);

/* ---- 凭据行动作按钮文案(2026-10-11 用户反馈统一) ----
 * 口径:按钮说的是「点下去做什么」,所以**未录入 = 录入、已录入 = 修改**。
 * 此前人脸恒「修改」、指纹恒「管理」,与行内值(「无」)自相矛盾,用户
 * 在添加页看不懂哪个能点。放本层是为了可被宿主测试直接锁死(valid_ui
 * 是 UI 文案的唯一映射点,与 dg_ui_enroll_err_text 同处)。 */

/** 人脸行:已录(库内或未保存草稿)→ 修改;未录 → 录入 */
const char *dg_ui_face_btn_text(bool enrolled);

/** 指纹行:已录 ≥1 枚 → 修改;未录 → 录入 */
const char *dg_ui_finger_btn_text(uint32_t count);

/** IC 卡行:已绑卡 → 修改;未绑 → 录入 */
const char *dg_ui_ic_btn_text(bool bound);

#ifdef __cplusplus
}
#endif

#endif /* DG_UI_VALID_H */
