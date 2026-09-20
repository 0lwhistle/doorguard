/*
 * events.js — 实时事件流(WebSocket 落地)
 *
 * 服务端推两种消息:auth(每次验证)与 ntp(时间校正结果)。这里做三件事:
 *   1) 维护滚动缓冲(最多 MAX 条,最新的在最前);
 *   2) 把连接状态暴露给顶栏指示;
 *   3) 把 ntp 结果转成提示并刷新设备快照(校正后时间/状态变了)。
 */
import { computed, reactive } from 'vue'
import { createEventSocket } from '../api/ws'
import { getToken } from '../api/client'
import { refreshQuiet } from './device'
import { toast } from './toast'

const MAX_ITEMS = 60

const state = reactive({
  items: [],
  status: 'closed', // connecting | open | closed
  total: 0, // 本次会话收到的认证事件数(概览里的"门禁记录"也由设备快照给)
})

export const events = state
export const isLive = computed(() => state.status === 'open')

let socket = null
let seq = 0

function onEvent(msg) {
  if (msg.type === 'auth') {
    /* 客户端补一个单调 id:列表 key 必须是稳定唯一值,用 ts/user_id 拼字符串
     * 会在"陌生人"(无 user_id)时拼出 NaN,导致 Vue 复用错行(实测告警) */
    state.items.unshift({ ...msg, id: ++seq })
    state.total += 1
    if (state.items.length > MAX_ITEMS) state.items.length = MAX_ITEMS
    return
  }
  if (msg.type === 'ntp') {
    /* 校正会改变系统时间/NTP 状态,顺便刷一次快照 */
    if (msg.ok) toast.ok(msg.msg || '时间校正成功')
    else toast.err(msg.msg || '时间校正失败')
    refreshQuiet()
  }
}

export function start() {
  if (socket) return
  socket = createEventSocket({
    getToken,
    onEvent,
    onStatus: (status) => {
      state.status = status
    },
    onReconnectProbe: () => {
      /* 连不上两次:探一次接口,会话失效时 HTTP 层会 401 → 回登录页 */
      refreshQuiet()
    },
  })
  socket.start()
}

export function stop() {
  if (socket) socket.stop()
  socket = null
  state.status = 'closed'
}

export function clearFeed() {
  state.items.splice(0, state.items.length)
}
