/*
 * types.c — err.h / types.h 的少量非内联实现
 */
#include "err.h"

const char *dg_err_name(dg_err_t err)
{
    switch (err) {
    case DG_OK:                 return "OK";
    case DG_ERR_PARAM:          return "PARAM";
    case DG_ERR_NO_MEMORY:      return "NO_MEMORY";
    case DG_ERR_NOT_FOUND:      return "NOT_FOUND";
    case DG_ERR_NOT_INIT:       return "NOT_INIT";
    case DG_ERR_IO:             return "IO";
    case DG_ERR_DB:             return "DB";
    case DG_ERR_STATE:          return "STATE";
    case DG_ERR_TIMEOUT:        return "TIMEOUT";
    case DG_ERR_BUSY:           return "BUSY";
    case DG_ERR_NETWORK:        return "NETWORK";
    case DG_ERR_INTERNAL:       return "INTERNAL";
    case DG_ERR_NO_PASSWORD:    return "NO_PASSWORD";
    case DG_ERR_DUP_UID:        return "DUP_UID";
    case DG_ERR_DUP_IC:         return "DUP_IC";
    case DG_ERR_DUP_FACE:       return "DUP_FACE";
    case DG_ERR_DUP_FINGER:     return "DUP_FINGER";
    case DG_ERR_USER_LIMIT:     return "USER_LIMIT";
    case DG_ERR_BAD_UID:        return "BAD_UID";
    case DG_ERR_BAD_NAME:       return "BAD_NAME";
    case DG_ERR_BAD_PWD:        return "BAD_PWD";
    case DG_ERR_WRONG_PASSWORD: return "WRONG_PASSWORD";
    case DG_ERR_LOCKED:         return "LOCKED";
    case DG_ERR_AUTH_DISABLED:  return "AUTH_DISABLED";
    case DG_ERR_METHOD_DISABLED: return "METHOD_DISABLED";
    case DG_ERR_BLACKLIST:      return "BLACKLIST";
    case DG_ERR_MISMATCH:       return "MISMATCH";
    default:                    return "UNKNOWN";
    }
}
