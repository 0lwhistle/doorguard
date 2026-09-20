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
