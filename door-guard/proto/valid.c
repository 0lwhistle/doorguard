/*
 * valid.c — 业务字段合法性规则实现(规则与理由见 valid.h)
 *
 * 实现要点:按字节扫描 + 中文按"高位字节成对"放过(不做完整 UTF-8 解码:
 * 名称只要求"不含控制字符",非法 UTF-8 由上层清洗/数据库 UTF-8 约束兜底)。
 */
#include "valid.h"
#include "types.h"

#include <ctype.h>
#include <string.h>

#define DG_UID_MIN_LEN  3
#define DG_UID_MAX_LEN  (DG_UID_LEN - 1)     /* 31:留 '\0' */
#define DG_NAME_MAX_LEN (DG_NAME_LEN - 1)    /* 63 */
#define DG_PWD_MIN_LEN  4
#define DG_PWD_LIMIT    (DG_PWD_MAX_LEN - 1) /* 31:明文输入上限见 types.h */

static bool is_uid_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '-' || c == '_';
}

static bool is_uid_head(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

int dg_valid_uid(const char *uid)
{
    if (!uid || !*uid)
        return DG_ERR_BAD_UID;
    size_t n = strlen(uid);
    if (n < DG_UID_MIN_LEN || n > DG_UID_MAX_LEN)
        return DG_ERR_BAD_UID;
    if (!is_uid_head((unsigned char)uid[0]))
        return DG_ERR_BAD_UID;
    for (size_t i = 0; i < n; i++) {
        if (!is_uid_char((unsigned char)uid[i]))
            return DG_ERR_BAD_UID;
    }
    return DG_OK;
}

int dg_valid_name(const char *name)
{
    if (!name || !*name)
        return DG_ERR_BAD_NAME;
    size_t n = strlen(name);
    if (n > DG_NAME_MAX_LEN)
        return DG_ERR_BAD_NAME;
    if (name[0] == ' ' || name[n - 1] == ' ')
        return DG_ERR_BAD_NAME;                  /* 前导/尾随空格 */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f)               /* 控制字符 */
            return DG_ERR_BAD_NAME;
    }
    return DG_OK;
}

int dg_valid_pwd(const char *pwd)
{
    if (!pwd || !*pwd)
        return DG_ERR_BAD_PWD;
    size_t n = strlen(pwd);
    if (n < DG_PWD_MIN_LEN || n > DG_PWD_LIMIT)
        return DG_ERR_BAD_PWD;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pwd[i];
        if (c < 0x21 || c > 0x7e)                /* 可见 ASCII,无空格 */
            return DG_ERR_BAD_PWD;
    }
    return DG_OK;
}

const char *dg_valid_hint(int err)
{
    switch (err) {
    case DG_ERR_BAD_UID:  return "ID 需 3~31 位字母/数字/'-'/'_',且以字母或数字开头";
    case DG_ERR_BAD_NAME: return "姓名 1~63 字节,不能为空/前后带空格/含控制字符";
    case DG_ERR_BAD_PWD:  return "密码 4~31 位可见字符(不含空格)";
    default:              return "取值非法";
    }
}
