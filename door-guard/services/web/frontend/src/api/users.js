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

/* IC 卡号(ICCARD_PROTOCOL §5):8~30 位十六进制 = 4~15B UID。
 * 全链路口径恒为大写 HEX,小写输入归一后再校验 */
export function normalizeCardNo(v) {
  return (v || '').trim().toUpperCase()
}

export function validCardNo(v) {
  return /^[0-9A-F]{8,30}$/.test(normalizeCardNo(v))
}

export const CARD_NO_HINT = '卡号 8~30 位十六进制(4~15 字节 UID),例:04A3B2C1'

export const UID_HINT = 'ID 需 3~31 位字母/数字/\'-\'/\'_\',且以字母或数字开头'
export const NAME_HINT = '姓名 1~63 字节,不能为空/前后带空格/含控制字符'
export const PWD_HINT = '密码 4~31 位可见字符(不含空格)'

/** 用户列表(分页)。@returns {{users:Array,page:number,pages:number,total:number}} */
export function listUsers({ page = 1, pageSize = 20 } = {}) {
  return request(PATHS.users, { params: { page, page_size: pageSize } })
}

/** 添加用户(密码必填)。默认只勾密码:「方式位 ⇒ 已录凭据」不变式下,
 *  新用户除密码外没有可用方式;人脸位在上传录入时自动开启 */
export function addUser({ uid, name, pwd, role = 0, authFlags = 0b100 }) {
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

/** 上传人脸照片录入/重录(受理制)。blob = 降采样后的 JPEG;
 *  服务端硬顶 512KB,提取结果经 WS enroll 消息(seq 配对)回推。
 *  前端应先用 downscaleJpeg() 把图降到最长边 ≤1024 再传 */
export function setUserFace({ uid, blob }) {
  return request(PATHS.usersFaceSet, {
    method: 'POST',
    params: { uid },
    rawBody: blob,
    rawType: 'image/jpeg',
  })
}

/** 绑定 IC 卡(同步落库;查重/置位在服务端,重绑覆盖)。卡号小写自动归一 */
export function setUserIc({ uid, cardNo }) {
  return request(PATHS.usersIcSet, {
    method: 'POST',
    body: { uid, card_no: normalizeCardNo(cardNo) },
  })
}

/** 解绑 IC 卡(同步;未绑卡幂等成功) */
export function clearUserIc({ uid }) {
  return request(PATHS.usersIcClear, { method: 'POST', body: { uid } })
}

/**
 * 图片文件 → 降采样 JPEG Blob(canvas 重编码;顺带抹掉 EXIF/方向)。
 * @param {File} file 用户选择的图片
 * @param {{maxDim?:number, quality?:number}} opts
 * @returns {Promise<{blob:Blob, width:number, height:number}>}
 */
export async function downscaleJpeg(file, { maxDim = 1024, quality = 0.85 } = {}) {
  const bitmap = await createImageBitmap(file, { imageOrientation: 'from-image' })
  const scale = Math.min(1, maxDim / Math.max(bitmap.width, bitmap.height))
  const w = Math.max(1, Math.round(bitmap.width * scale))
  const h = Math.max(1, Math.round(bitmap.height * scale))
  const canvas = document.createElement('canvas')
  canvas.width = w
  canvas.height = h
  const ctx = canvas.getContext('2d')
  ctx.imageSmoothingQuality = 'high'
  ctx.drawImage(bitmap, 0, 0, w, h)
  bitmap.close?.()
  const blob = await new Promise((resolve, reject) =>
    canvas.toBlob(
      (b) => (b ? resolve(b) : reject(new Error('图片编码失败'))),
      'image/jpeg',
      quality,
    ),
  )
  return { blob, width: w, height: h }
}
