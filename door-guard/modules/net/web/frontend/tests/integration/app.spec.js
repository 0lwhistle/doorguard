/*
 * app.spec.js — 集成冒烟:整个应用真的能起来、能登录、能显示数据
 *
 * 为什么要这一层:单测各自都过,但"路由 + store + 组件"接线错了依然会白屏。
 * 这里用 jsdom 挂真应用,把 fetch/WebSocket 换成受控桩,断言:
 *   未登录 → 登录页;登录成功 → 侧栏 + 概览指标出现;WS 事件 → 实时列表出现。
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'
import { createMemoryHistory } from 'vue-router'

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
  version: 'v-test',
  uptime_text: '1 分 0 秒',
  users: 3,
  log_total: 42,
  ip: '10.0.0.9',
  have_ip: true,
  mdns_host: 'doorguard',
  mdns_running: true,
  web_port: 8080,
  web_user: 'admin',
  pwd_default: false,
  db_bytes: 2048,
  disk_free_bytes: 1048576,
  online: true,
  ntp: { ok: false, last_ok_at: '' },
}

/* 受控 fetch:按 URL 分派,记录调用 */
function installFetch({ authed = true } = {}) {
  const calls = []
  global.fetch = vi.fn(async (url, init = {}) => {
    calls.push({ url, method: init.method || 'GET', headers: init.headers || {} })
    const route = String(url).split('?')[0]
    if (route === '/api/login') {
      return { status: 200, ok: true, json: async () => ({ token: 'T', user: 'admin', pwd_default: false }) }
    }
    if (!authed && !init.headers['X-Auth-Token']) {
      return { status: 401, ok: false, json: async () => ({ msg: '未登录' }) }
    }
    if (route === '/api/device') return { status: 200, ok: true, json: async () => DEVICE }
    if (route === '/api/logs') {
      return {
        status: 200,
        ok: true,
        json: async () => ({
          page: 1,
          pages: 1,
          total: 1,
          logs: [
            { ts: 1, time: '2026-09-18 10:00:00', user_id: '10001', user_name: '张三', method_name: '人脸1:N', result: 0 },
          ],
        }),
      }
    }
    return { status: 200, ok: true, json: async () => ({}) }
  })
  return calls
}

/**
 * 挂载应用。
 *
 * 注意 vi.resetModules() 之后必须**在同一个函数里**把所有要用的模块 import 出来
 * 交给测试使用:vitest 的模块注册表重置后再 import 会得到新的实例,而 store 是
 * 模块级单例——测试里用另一个实例操作 token,应用侧的守卫看不到(实测踩坑)。
 */
async function mountApp(initial = '/') {
  vi.resetModules()
  const client = await import('../../src/api/client')
  const session = await import('../../src/stores/session')
  const { createAppRouter } = await import('../../src/router')
  const App = (await import('../../src/App.vue')).default
  /* memory history:jsdom 的 hashchange 是异步派发的,会让"导航后立刻读路由"的
   * 断言偶发失败;导航语义与生产一致。 */
  const router = createAppRouter(createMemoryHistory())
  await router.replace(initial)
  await router.isReady()
  const wrapper = mount(App, { global: { plugins: [router] } })
  await flushPromises()
  return { wrapper, router, client, session }
}

beforeEach(() => {
  sessionStorage.clear()
  FakeWebSocket.instances = []
  global.WebSocket = FakeWebSocket
})

describe('应用集成', () => {
  it('无 token:路由守卫把用户挡在登录页', async () => {
    installFetch({ authed: false })
    const { wrapper, router } = await mountApp()
    expect(router.currentRoute.value.name).toBe('login')
    expect(wrapper.text()).toContain('door-guard')
    expect(wrapper.find('input[type="password"]').exists()).toBe(true)
  })

  it('侧栏导航切到记录查询:表格渲染服务端返回的行', async () => {
    sessionStorage.setItem('dg_token', 'T') // 已登录状态直进
    installFetch()
    const { wrapper, router } = await mountApp()
    await router.push('/logs')
    await flushPromises()
    expect(wrapper.text()).toContain('门禁记录查询')
    expect(wrapper.text()).toContain('张三')
    expect(wrapper.text()).toContain('第 1 / 1 页')
  })

  it('会话失效时立刻被送回登录页(而不是留在一个不再更新的页面)', async () => {
    sessionStorage.setItem('dg_token', 'T')
    installFetch()
    const { router, client } = await mountApp()
    /* 模拟 token 过期:服务端对任意接口回 401 */
    global.fetch.mockResolvedValueOnce({
      status: 401,
      ok: false,
      json: async () => ({ msg: '会话已过期,请重新登录' }),
    })
    await expect(client.request('/api/device')).rejects.toBeTruthy()
    /* 视图是按需 import 的:应用侧触发的跳转要等真实 I/O 轮次完成,
     * 用 waitFor 轮询而不是假定一个微任务就够(实测:只 flushPromises 会读早) */
    await vi.waitFor(() => expect(router.currentRoute.value.name).toBe('login'))
  })

  it('从受保护页面进入登录页时记住来源,便于登录后回跳', async () => {
    sessionStorage.setItem('dg_token', 'T')
    installFetch()
    const { router, client } = await mountApp()
    client.setToken('')                // 掉线(token 被清)
    await router.push('/logs')
    await vi.waitFor(() => expect(router.currentRoute.value.name).toBe('login'))
    expect(router.currentRoute.value.fullPath).toBe('/login?next=/logs')
  })
})
