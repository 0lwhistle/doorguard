/*
 * valid_ui.h — 输入弹窗用的校验包装(规则在 proto/valid.h,文案在本层)
 *
 * 为什么单独一层:规则(长度/字符集)是业务契约,必须与存储层同一份;
 * 而"给用户看的错误文案"要过 i18n、属 UI 层。这里把 规则→文案 收在一处,
 * 供 presenter_home(验证流程)与 page_users(用户管理)共用,避免两处各写一套。
 *
 * 用法:直接作为 dg_popup_input_cfg_t.validate 传入(返回 NULL = 通过)。
 */
#ifndef DG_UI_VALID_H
#define DG_UI_VALID_H

#ifdef __cplusplus
extern "C" {
#endif

/** 用户 ID 校验(3~31 位字母/数字/-/_,字母或数字开头) */
const char *dg_ui_valid_uid(const char *text);

/** 姓名校验(1~63 字节,非空/无前后空格/无控制字符) */
const char *dg_ui_valid_name(const char *text);

/** 密码校验(4~31 位可见字符,不含空格) */
const char *dg_ui_valid_pwd(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* DG_UI_VALID_H */
