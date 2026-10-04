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

const char *dg_ui_enroll_err_text(int rc)
{
    switch (rc) {
    case DG_ERR_NO_PASSWORD:  return _("请先设置密码");
    case DG_ERR_DUP_UID:      return _("该用户ID已存在");
    case DG_ERR_DUP_IC:       return _("该卡已绑定其他用户");
    case DG_ERR_DUP_FACE:     return _("该人脸已绑定其他用户");
    case DG_ERR_DUP_FINGER:   return _("该指纹已绑定其他用户");
    case DG_ERR_FINGER_FULL:  return _("指纹库已满");
    case DG_ERR_FINGER_LIMIT: return _("该用户指纹已达上限");
    case DG_ERR_AUTH_NO_CRED: return _("该验证方式未录入，无法开启");
    case DG_ERR_USER_LIMIT:   return _("用户数已达上限");
    case DG_ERR_BAD_NAME:     return _("姓名不合法");
    case DG_ERR_BAD_PWD:      return _("密码不合法");
    case DG_ERR_BAD_UID:      return _("用户ID不合法");
    case DG_ERR_IO:           return _("设备通信异常，请重试");
    case DG_ERR_TIMEOUT:      return _("操作超时，请重试");
    case DG_ERR_DB:           return _("存储异常，请重试");
    case DG_ERR_MISMATCH:     return _("指纹采集质量差，请重按");
    default:                  return _("操作失败,请重试");
    }
}
