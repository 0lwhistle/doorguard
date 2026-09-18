/*
 * toast.js — 全局提示队列
 *
 * 模块级单例(import 即同一份状态),不需要 Pinia:门禁上位机的共享状态就
 * 这么几处,组合式 API + 模块单例足够,还少一层依赖和体积。
 */
import { reactive, readonly } from 'vue'

const state = reactive({ items: [] })
let seq = 0

function push(message, kind = 'info', ttlMs) {
  const id = ++seq
  const ttl = ttlMs ?? (kind === 'err' ? 5200 : 3200)
  state.items.push({ id, message, kind, leaving: false })
  setTimeout(() => dismiss(id), ttl)
  return id
}

function dismiss(id) {
  const item = state.items.find((i) => i.id === id)
  if (!item || item.leaving) return
  item.leaving = true                       /* 先播放退出动画,再移除 */
  setTimeout(() => {
    const idx = state.items.findIndex((i) => i.id === id)
    if (idx >= 0) state.items.splice(idx, 1)
  }, 280)
}

export const toasts = readonly(state)
export const toast = {
  info: (m) => push(m, 'info'),
  ok: (m) => push(m, 'ok'),
  err: (m) => push(m, 'err'),
  dismiss,
}
