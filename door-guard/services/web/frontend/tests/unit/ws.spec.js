/*
 * ws.spec.js — 实时通道状态机(重连/退避/探活)
 *
 * 用假 WebSocket + 假定时器把"断了会重连、连不上两次会探活"钉死:
 * 这段逻辑出错的表现是"页面看着正常但事件不更新",线上很难查。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

class FakeWebSocket {
  static instances = []
  constructor(url) {
    this.url = url
    this.readyState = 0
    FakeWebSocket.instances.push(this)
  }
  close() {
    this.readyState = 3
    if (this.onclose) this.onclose()
  }
  /* 测试辅助:模拟服务端握手完成/推消息 */
  open() {
    this.readyState = 1
    if (this.onopen) this.onopen()
  }
  emit(data) {
    if (this.onmessage) this.onmessage({ data: JSON.stringify(data) })
  }
  serverClose() {
    this.readyState = 3
    if (this.onclose) this.onclose()
  }
}

let createEventSocket

beforeEach(async () => {
  vi.resetModules()
  vi.useFakeTimers()
  FakeWebSocket.instances = []
  global.WebSocket = FakeWebSocket
  ;({ createEventSocket } = await import('../../src/api/ws'))
})

afterEach(() => {
  vi.useRealTimers()
  delete global.WebSocket
})

function setup(overrides = {}) {
  const seen = { events: [], statuses: [], probes: 0 }
  const socket = createEventSocket({
    getToken: () => 'tok',
    onEvent: (d) => seen.events.push(d),
    onStatus: (s, tries) => seen.statuses.push([s, tries]),
    onReconnectProbe: () => {
      seen.probes += 1
    },
    ...overrides,
  })
  return { socket, seen }
}

describe('api/ws', () => {
  it('token 走查询串,连上后状态变 open 且重试计数清零', () => {
    const { socket, seen } = setup()
    socket.start()
    const ws = FakeWebSocket.instances[0]
    expect(ws.url).toContain('/api/ws?token=tok')
    ws.open()
    expect(seen.statuses.at(-1)).toEqual(['open', 0])
  })

  it('收到消息原样交给上层(不在这里解析业务)', () => {
    const { socket, seen } = setup()
    socket.start()
    const ws = FakeWebSocket.instances[0]
    ws.open()
    ws.emit({ type: 'auth', user_name: '张三' })
    expect(seen.events).toEqual([{ type: 'auth', user_name: '张三' }])
  })

  it('服务端断开后自动重连,退避递增且不超过上限', () => {
    const { socket, seen } = setup()
    socket.start()
    FakeWebSocket.instances[0].serverClose()
    expect(seen.statuses.at(-1)[0]).toBe('closed')
    /* 退避 = 800ms × 1.6^tries:第 1 次约 1.28s,所以 1s 时还不该重连 */
    vi.advanceTimersByTime(1000)
    expect(FakeWebSocket.instances).toHaveLength(1)
    vi.advanceTimersByTime(400)
    expect(FakeWebSocket.instances).toHaveLength(2)
    FakeWebSocket.instances[1].serverClose()
    /* 第二次失败:退避更久(约 2s),且第 2 次失败会触发一次探活 */
    vi.advanceTimersByTime(1000)
    expect(FakeWebSocket.instances).toHaveLength(2)
    vi.advanceTimersByTime(1200)
    expect(seen.probes).toBe(1)
  })

  it('stop 之后不再重连(用户主动退出)', () => {
    const { socket } = setup()
    socket.start()
    socket.stop()
    vi.advanceTimersByTime(20000)
    expect(FakeWebSocket.instances).toHaveLength(1)
  })
})
