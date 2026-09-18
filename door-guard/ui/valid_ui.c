/*
 * valid_ui.c — 输入校验的文案包装(规则见 proto/valid.h,文案走 _())
 */
#include "valid_ui.h"
#include "i18n.h"
#include "valid.h"

const char *dg_ui_valid_uid(const char *text)
{
    if (dg_valid_uid(text) == DG_OK)
        return NULL;
    return _("ID 需 3~31 位字母、数字、- 或 _，且以字母或数字开头");
}

const char *dg_ui_valid_name(const char *text)
{
    if (dg_valid_name(text) == DG_OK)
        return NULL;
    return _("姓名需 1~63 字节，不能为空或前后带空格");
}

const char *dg_ui_valid_pwd(const char *text)
{
    if (dg_valid_pwd(text) == DG_OK)
        return NULL;
    return _("密码需 4~31 位，不能含空格");
}
