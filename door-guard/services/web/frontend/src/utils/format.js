/*
 * format.js — 展示格式化(纯函数,可单测)
 */
/** Date → YYYY-MM-DD(本地时区,服务端按本地时区解释日期) */
export function dateStr(d) {
  const pad = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`
}

/** 距今天 n 天的日期字符串 */
export function daysAgo(n, now = new Date()) {
  return dateStr(new Date(now.getTime() - n * 86400000))
}

/**
 * 结果码 → 语义(true=通过,与服务端 result=0 一致)
 *
 * 必须显式比较 0/'0':用 Number(result) === 0 会把 null/''/false 判成"通过"
 * —— 门禁界面把"拒绝"显示成"通过"是安全事故级错误(实测踩到)。
 */
export function isPass(result) {
  return result === 0 || result === '0'
}

/** 连接状态 → 顶栏可见文案 */
export function connText(status) {
  switch (status) {
    case 'open':
      return '实时连接'
    case 'connecting':
      return '连接中…'
    default:
      return '已断开'
  }
}

/* ---- 点分 IPv4(网络配置的点号固定输入;与设备端 proto/valid 同语义)----
 * 约定与设备一致:用户只敲数字,点号由机器按每 3 位一组插入;完整输入恰
 * 12 位数字;提交前规范化去段内前导零(设备侧 inet_pton 类解析拒 "001") */

const IPV4_RE = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/

/** 点分 IPv4 形态 → 四段数值数组(允许段内前导零);形态非法返回 null */
export function ipv4Octets(v) {
  const m = IPV4_RE.exec((v || '').trim())
  if (!m) return null
  const arr = m.slice(1).map(Number)
  return arr.every((x) => x >= 0 && x <= 255) ? arr : null
}

/** 点号自动补全:抽取数字(≤12 位)按 3 位一组重插点号(展示格式化,
 *  不判段值——那是 validIp/validMask 的事) */
export function ipv4Autodot(v) {
  const d = (v || '').replace(/\D/g, '').slice(0, 12)
  const parts = []
  for (let i = 0; i < d.length; i += 3) parts.push(d.slice(i, i + 3))
  return parts.join('.')
}

/** 恰 12 位数字(点号固定输入的完整度门禁) */
export function ipv4Complete(v) {
  return ((v || '').match(/\d/g) || []).length === 12
}

/** 规范化:去段内前导零("192.168.001.010"→"192.168.1.10");
 *  形态非法原样返回(调用方校验在前) */
export function ipv4Normalize(v) {
  const a = ipv4Octets(v)
  return a ? a.join('.') : v
}

/** 补零形态:"192.168.1.10"→"192.168.001.010"(规范值回填输入框前的
 *  中间态);空串→空串(网关可空);非法原样返回 */
export function ipv4Pad(v) {
  if (!v) return ''
  const a = ipv4Octets(v)
  return a ? a.map((x) => String(x).padStart(3, '0')).join('.') : v
}
