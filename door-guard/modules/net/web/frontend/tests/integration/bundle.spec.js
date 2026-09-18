/*
 * bundle.spec.js — 对**构建产物**的冒烟测试(不看源码,只看真正发下去的东西)
 *
 * 为什么单开一层:源码级测试过了,产物仍可能因为打包/裁剪/别名问题起不来。
 * 这里把 pages/assets/app.js(固件内嵌的那份字节)直接丢进 jsdom 执行,验证:
 *   1) 应用能挂载(无异常);
 *   2) 未登录时显示登录页(路由守卫在产物里也生效);
 *   3) 填表提交后进入概览页并显示设备数据(整条链路:api → store → 视图);
 *   4) WebSocket 收到的验证事件出现在实时列表里。
 * 依赖只有 fetch/WebSocket 两个桩:凡是需要真实网络的行为都不在这里测
 * (那部分在 tests/web/web_test.sh 里对真设备接口测)。
 */
import { readFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const HERE = dirname(fileURLToPath(import.meta.url))
/* pages/ 在前端工程上一层:那里才是要编进固件的那份字节 */
const BUNDLE = resolve(HERE, '../../../pages/assets/app.js')

class FakeWebSocket {
  static instances = []
  constructor(url) {
    this.url = url
    FakeWebSocket.instances.push(this)
  }
  close() {}
  open() {
    if (this.onopen) this.onopen()
  }
  emit(data) {
    if (this.onmessage) this.onmessage({ data: JSON.stringify(data) })
  }
}

const DEVICE = {
  version: 'v-bundle',
  uptime_text: '3 分 0 秒',
  users: 7,
  log_total: 99,
  ip: '192.168.2.7',
  have_ip: true,
  mdns_host: 'doorguard',
  mdns_running: true,
  web_port: 8080,
  web_user: 'admin',
  pwd_default: true,
  db_bytes: 8192,
  disk_free_bytes: 1073741824,
  online: true,
  ntp: { ok: false, last_ok_at: '' },
}

const tick = (ms = 30) => new Promise((r) => setTimeout(r, ms))

beforeEach(() => {
  document.body.innerHTML = '<div id="app"></div>'
  sessionStorage.clear()
  FakeWebSocket.instances = []
  global.WebSocket = FakeWebSocket
  global.fetch = vi.fn(async (url, init = {}) => {
    const route = String(url).split('?')[0]
    if (route === '/api/login') {
      return { status: 200, ok: true, json: async () => ({ token: 'BT', user: 'admin', pwd_default: true }) }
    }
    if (route === '/api/device') return { status: 200, ok: true, json: async () => DEVICE }
    if (route === '/api/logs') {
      return { status: 200, ok: true, json: async () => ({ page: 1, pages: 1, total: 0, logs: [] }) }
    }
    return { status: 200, ok: true, json: async () => ({}) }
  })
})

async function bootBundle() {
  const code = readFileSync(BUNDLE, 'utf8')
  /* 产物是自包含的 IIFE(配置了 inlineDynamicImports),没有 import/import.meta,
   * 所以可以直接执行;必须在每次测试里重新执行——它挂载的是全局单例应用。 */
  // eslint-disable-next-line no-new-func
  new Function(code)()
  await tick(60)
}

describe('构建产物(pages/assets/app.js)', () => {
  it('能挂载并停在登录页', async () => {
    await bootBundle()
    const app = document.getElementById('app')
    expect(app.innerHTML.length).toBeGreaterThan(0)
    expect(app.textContent).toContain('door-guard')
    expect(app.querySelector('input[type="password"]')).toBeTruthy()
  })

  it('登录后进入概览,设备数据与实时连接都渲染出来', async () => {
    await bootBundle()
    const app = document.getElementById('app')
    const pwd = app.querySelector('input[type="password"]')
    pwd.value = 'admin'
    pwd.dispatchEvent(new Event('input', { bubbles: true }))
    app.querySelector('form').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    await tick(80)

    expect(app.textContent).toContain('设备概览')
    expect(app.textContent).toContain('v-bundle')
    expect(app.textContent).toContain('doorguard.local:8080')

    /* 默认口令横幅:服务端说 pwd_default=true 就该出现 */
    expect(app.textContent).toContain('出厂默认口令')

    /* 实时事件:WebSocket 推一条,列表里应能看到 */
    const ws = FakeWebSocket.instances.at(-1)
    expect(ws.url).toContain('/api/ws?token=BT')
    ws.open()
    ws.emit({
      type: 'auth',
      ts: 5,
      time: '11:22:33',
      user_id: '10009',
      user_name: '王五',
      method_name: '人脸1:N',
      result: 1,
    })
    await tick(40)
    expect(app.textContent).toContain('王五')
    expect(app.textContent).toContain('拒绝')
  })
})
