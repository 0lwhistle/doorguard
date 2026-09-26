/*
 * users.js — 用户管理 API + 字段校验(与设备端 proto/valid.c 同源规则)
 *
 * 校验规则唯一权威在设备端 storage(绕不过);这里的 JS 版本只做"提交前
 * 即时反馈",两边规则必须保持一致——改一边必须同步另一边(tests/web 有
 * 端点核对,字段规则靠 review 纪律)。
 */
import { PATHS } from './endpoints'
import { request } from './client'

export const ROLES = [
  { value: 0, label: '普通' },
  { value: 1, label: '管理员' },
  { value: 2, label: '黑名单' },
]

export const ROLE_TEXT = ['普通', '管理员', '黑名单']

export const AUTH_FLAGS = [
  { bit: 1, label: '人脸' },
  { bit: 4, label: '密码' },
  { bit: 2, label: '指纹' },
  { bit: 8, label: 'IC 卡' },
]

/** user_id:3~31 位字母/数字/'-'/'_',首字符字母或数字(valid.c 同规则) */
export function validUid(v) {
  return /^[0-9A-Za-z][0-9A-Za-z_-]{2,31}$/.test(v)
}

/** user_name:1~63 字节,非空,无控制字符,前后无空格 */
export function validName(v) {
  if (!v || v.length > 63) return false
  if (v !== v.trim()) return false
  for (const ch of v) {
    const c = ch.codePointAt(0)
    if (c < 0x20 || c === 0x7f) return false
  }
  return true
}

/** 密码:4~31 位可见 ASCII(不含空格) */
export function validPwd(v) {
  return /^[\x21-\x7e]{4,31}$/.test(v)
}

export const UID_HINT = 'ID 需 3~31 位字母/数字/\'-\'/\'_\',且以字母或数字开头'
export const NAME_HINT = '姓名 1~63 字节,不能为空/前后带空格/含控制字符'
export const PWD_HINT = '密码 4~31 位可见字符(不含空格)'

/** 用户列表(分页)。@returns {{users:Array,page:number,pages:number,total:number}} */
export function listUsers({ page = 1, pageSize = 20 } = {}) {
  return request(PATHS.users, { params: { page, page_size: pageSize } })
}

/** 添加用户(密码必填;人脸等特征录入口在设备端) */
export function addUser({ uid, name, pwd, role = 0, authFlags = 0b101 }) {
  return request(PATHS.usersAdd, {
    method: 'POST',
    body: { uid, name, pwd, role, auth_flags: authFlags },
  })
}

/** 编辑用户(name/role/auth_flags 任选) */
export function updateUser({ uid, name, role, authFlags }) {
  const body = { uid }
  if (name !== undefined) body.name = name
  if (role !== undefined) body.role = role
  if (authFlags !== undefined) body.auth_flags = authFlags
  return request(PATHS.usersUpdate, { method: 'POST', body })
}

/** 重置密码 */
export function setUserPwd({ uid, pwd }) {
  return request(PATHS.usersPwd, { method: 'POST', body: { uid, pwd } })
}

/** 删除用户(受理制:返回后刷新列表确认) */
export function deleteUser({ uid }) {
  return request(PATHS.usersDelete, { method: 'POST', body: { uid } })
}

/** 清除已录人脸(保留用户;受理制) */
export function clearUserFace({ uid }) {
  return request(PATHS.usersFaceClear, { method: 'POST', body: { uid } })
}
