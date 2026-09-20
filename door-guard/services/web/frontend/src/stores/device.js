/*
 * device.js — 设备快照(/api/device)
 *
 * 视图不直接取数:这里统一拿、统一缓存、统一轮询(30s),并把常用派生文案
 * (地址/存储/版本)算好,避免每个组件各拼一遍字符串。
 */
import { computed, reactive } from 'vue'
import { getDevice } from '../api/device'
import { markPwdDefault, session } from './session'

const POLL_MS = 30000

const state = reactive({
  loaded: false,
  loading: false,
  error: '',
  data: null,
})

let timer = null

export const device = state

export const address = computed(() => {
  const d = state.data
  if (!d) return '—'
  if (!d.have_ip) return '设备未联网'
  const name = d.mdns_running ? `http://${d.mdns_host}.local:${d.web_port}` : null
  const ip = d.ip ? `${d.ip}:${d.web_port}` : null
  return [name, ip].filter(Boolean).join('  ·  ') || '—'
})

export const storageText = computed(() => {
  const d = state.data
  if (!d) return '—'
  return `${fmtBytes(d.db_bytes)} / 余 ${fmtBytes(d.disk_free_bytes)}`
})

export function fmtBytes(n) {
  if (!n) return '—'
  const units = ['B', 'KB', 'MB', 'GB']
  let i = 0
  let v = n
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024
    i += 1
  }
  return `${v.toFixed(v < 10 && i > 0 ? 1 : 0)} ${units[i]}`
}

export async function refresh() {
  state.loading = true
  try {
    const d = await getDevice()
    state.data = d
    state.loaded = true
    state.error = ''
    session.user = d.web_user || session.user
    markPwdDefault(d.pwd_default)
    return d
  } catch (err) {
    state.error = err.message
    throw err
  } finally {
    state.loading = false
  }
}

/** 静默刷新(轮询/事件后):失败不弹错,避免网络抖动刷屏 */
export async function refreshQuiet() {
  try {
    await refresh()
  } catch {
    /* 静默:错误态由 state.error 反映,顶栏连接状态另有指示 */
  }
}

export function startPolling() {
  stopPolling()
  timer = setInterval(refreshQuiet, POLL_MS)
}

export function stopPolling() {
  if (timer) clearInterval(timer)
  timer = null
}
