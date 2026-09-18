/*
 * valid.h — 业务字段合法性规则(唯一权威;storage 与 UI 共用同一份判断)
 *
 * 为什么单独一个文件:输入合法性必须"一处定义、两处执行"——
 *   ① 存储层(db_user_add/update/set_password)强制:任何创建途径
 *      (设备 UI / 上位机 / 测试脚本 / 未来的 API)都绕不过;
 *   ② UI 弹窗在按"OK"时即时校验:不让非法值提交,也不让用户等超时。
 * 两条用同一函数,规范不会漂移;纯函数无依赖,宿主 gcc 可直接单测。
 *
 * 规则(改动须同步 spec-database §2 字段规则表):
 *   user_id : 3~31 字节;字母/数字/'-'/'_';首字符必须是字母或数字(避免 -/_
 *             打头与日志/命令行/URL 混用);区分大小写;不允许空格
 *   user_name: 1~63 字节 UTF-8;不含控制字符与前导/尾随空格(允许中文/空格/·/-)
 *   password: 4~31 字节;可见 ASCII(0x21~0x7E),不含空格/控制字符;
 *             区分大小写;与 user_id 相同的规则过严(密码可以含符号)
 */
#ifndef DG_VALID_H
#define DG_VALID_H

#include <stdbool.h>
#include <stddef.h>

#include "err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** user_id 合法性(DG_OK / DG_ERR_BAD_UID) */
int dg_valid_uid(const char *uid);

/** user_name 合法性(DG_OK / DG_ERR_BAD_NAME) */
int dg_valid_name(const char *name);

/** 明文密码合法性(DG_OK / DG_ERR_BAD_PWD) */
int dg_valid_pwd(const char *pwd);

/** 校验失败原因(简短中文说明,写入日志/上位机响应;不含 i18n) */
const char *dg_valid_hint(int err);

#ifdef __cplusplus
}
#endif

#endif /* DG_VALID_H */
