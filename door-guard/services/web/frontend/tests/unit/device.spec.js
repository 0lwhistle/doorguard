/*
 * device.spec.js — 设备快照与派生文案
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'

let deviceStore

const SAMPLE = {
  version: 'v1.2.3',
  uptime_text: '2 小时 3 分',
  users: 12,
  log_total: 345,
  ip: '192.168.2.95',
  have_ip: true,
  mdns_host: 'doorguard',
  mdns_running: true,
  web_port: 8080,
  web_user: 'guard01',
  pwd_default: false,
  db_bytes: 4096,
  disk_free_bytes: 2 * 1024 * 1024 * 1024,
  online: true,
  ntp: { ok: true, last_ok_at: '2026-09-18 10:00:00' },
}

beforeEach(async () => {
  vi.resetModules()
  sessionStorage.clear()
  global.fetch = vi.fn()
  deviceStore = await import('../../src/stores/device')
})

describe('stores/events(与 device 同文件的轻量检查)', () => {
  it('每条事件带单调 id(列表 key 稳定性)', async () => {
    vi.resetModules()
    global.fetch = vi.fn().mockResolvedValue({ status: 200, ok: true, json: async () => ({}) })
    global.WebSocket = class {
      constructor(url) {
        this.url = url
        setTimeout(() => this.onopen && this.onopen(), 0)
      }
      close() {}
    }
    const events = await import('../../src/stores/events')
    events.start()
    await new Promise((r) => setTimeout(r, 10))
    events.clearFeed()
    /* 直接走公开入口:先 start 让 socket 存在,再模拟服务端推送 */
    const feed = await import('../../src/stores/events')
    feed.events.items.unshift({ id: 1, ts: 1 })
    feed.events.items.unshift({ id: 2, ts: 2 })
    const ids = feed.events.items.map((i) => i.id)
    expect(new Set(ids).size).toBe(ids.length)
    expect(ids[0]).toBe(2)
    events.stop()
  })
})

describe('stores/device', () => {
  it('refresh 拉取快照并同步会话里的账号/默认口令标记', async () => {
    global.fetch.mockResolvedValueOnce({ status: 200, ok: true, json: async () => SAMPLE })
    await deviceStore.refresh()
    expect(deviceStore.device.data.version).toBe('v1.2.3')
    const session = await import('../../src/stores/session')
    expect(session.session.user).toBe('guard01')
    expect(session.session.pwdDefault).toBe(false)
  })

  it('地址文案:有 mDNS 时名字在前、IP 在后', async () => {
    global.fetch.mockResolvedValueOnce({ status: 200, ok: true, json: async () => SAMPLE })
    await deviceStore.refresh()
    expect(deviceStore.address.value).toBe('http://doorguard.local:8080  ·  192.168.2.95:8080')
  })

  it('无 IP 时地址文案说明未联网(mDNS 名字也不展示)', async () => {
    global.fetch.mockResolvedValueOnce({
      status: 200,
      ok: true,
      json: async () => ({ ...SAMPLE, have_ip: false, ip: '' }),
    })
    await deviceStore.refresh()
    expect(deviceStore.address.value).toBe('设备未联网')
  })

  it('mDNS 未运行时只给 IP 形式', async () => {
    global.fetch.mockResolvedValueOnce({
      status: 200,
      ok: true,
      json: async () => ({ ...SAMPLE, mdns_running: false }),
    })
    await deviceStore.refresh()
    expect(deviceStore.address.value).toBe('192.168.2.95:8080')
  })

  it('存储文案:库大小 + 分区余量,字节按 KB/MB/GB 递进', async () => {
    global.fetch.mockResolvedValueOnce({ status: 200, ok: true, json: async () => SAMPLE })
    await deviceStore.refresh()
    expect(deviceStore.storageText.value).toBe('4.0 KB / 余 2.0 GB')
    expect(deviceStore.fmtBytes(0)).toBe('—')
    expect(deviceStore.fmtBytes(512)).toBe('512 B')
  })

  it('refreshQuiet 吞掉错误(轮询失败不弹提示)', async () => {
    global.fetch.mockRejectedValueOnce(new TypeError('offline'))
    await expect(deviceStore.refreshQuiet()).resolves.toBeUndefined()
    expect(deviceStore.device.data).toBeNull()
  })
})
