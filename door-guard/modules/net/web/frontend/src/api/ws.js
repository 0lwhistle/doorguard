/*
 * ws.js — 实时事件通道(WebSocket 客户端)
 *
 * 职责边界:只管"连接与重连"和"把收到的 JSON 交给上层",不解析业务语义
 * (事件分流在 stores/events.js)。三条经验都写进代码里:
 *   1) 浏览器无法给 WebSocket 加请求头 → token 走查询串(服务端校验);
 *   2) 服务端是**主动推送**,客户端不需要发心跳,收到消息就是消息;
 *   3) 断线一律退避重连;连不上两次就顺手探一次接口——若是会话失效,
 *      HTTP 侧会给 401,由 session store 把用户送回登录页(而不是干等重连)。
 */
import { PATHS } from './endpoints'

const MAX_BACKOFF_MS = 15000
const BASE_BACKOFF_MS = 800

/**
 * @param {object} o
 * @param {() => string} o.getToken
 * @param {(data:object) => void} o.onEvent
 * @param {(state:'connecting'|'open'|'closed') => void} o.onStatus
 * @param {() => void} o.onReconnectProbe 连续失败后探活(用于发现会话失效)
 */
export function createEventSocket({ getToken, onEvent, onStatus, onReconnectProbe }) {
  let ws = null
  let tries = 0
  let timer = null
  let closedByUs = false

  function url() {
    const proto = location.protocol === 'https:' ? 'wss://' : 'ws://'
    return `${proto}${location.host}${PATHS.ws}?token=${encodeURIComponent(getToken())}`
  }

  function scheduleReconnect() {
    tries += 1
    const delay = Math.min(MAX_BACKOFF_MS, BASE_BACKOFF_MS * Math.pow(1.6, tries))
    onStatus('closed', tries)
    if (tries === 2 && onReconnectProbe) onReconnectProbe()
    timer = setTimeout(connect, delay)
  }

  function connect() {
    if (closedByUs) return
    onStatus('connecting', tries)
    try {
      ws = new WebSocket(url())
    } catch {
      scheduleReconnect()
      return
    }
    ws.onopen = () => {
      tries = 0
      onStatus('open', 0)
    }
    ws.onmessage = (ev) => {
      let data
      try {
        data = JSON.parse(ev.data)
      } catch {
        return
      }
      onEvent(data)
    }
    ws.onclose = () => {
      ws = null
      if (!closedByUs) scheduleReconnect()
    }
    ws.onerror = () => {
      /* onerror 之后必有 onclose,重连统一在 onclose 里做,避免双触发 */
    }
  }

  return {
    start() {
      closedByUs = false
      tries = 0
      connect()
    },
    stop() {
      closedByUs = true
      if (timer) clearTimeout(timer)
      timer = null
      if (ws) {
        try {
          ws.close()
        } catch {
          /* 忽略:关闭失败无后续影响 */
        }
        ws = null
      }
      onStatus('closed', 0)
    },
  }
}
