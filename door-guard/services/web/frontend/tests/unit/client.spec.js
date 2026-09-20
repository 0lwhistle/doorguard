/*
 * client.spec.js — HTTP 客户端契约
 *
 * 这些行为以前散在页面里,现在集中在 client.js,所以值得逐条测:
 * token 注入、空参数不拼接、401 清 token 并通知、错误文案归一。
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

let client

beforeEach(async () => {
  vi.resetModules()
  sessionStorage.clear()
  global.fetch = vi.fn()
  client = await import('../../src/api/client')
})

afterEach(() => {
  delete global.fetch
})

function mockJson(status, payload) {
  global.fetch.mockResolvedValueOnce({
    status,
    ok: status >= 200 && status < 300,
    json: async () => payload,
  })
}

describe('api/client', () => {
  it('token 写入后请求自动带 X-Auth-Token', async () => {
    client.setToken('T1')
    mockJson(200, { ok: true })
    await client.request('/api/device')
    const [, init] = global.fetch.mock.calls[0]
    expect(init.headers['X-Auth-Token']).toBe('T1')
  })

  it('无 token 时不带该头(登录接口前)', async () => {
    mockJson(200, {})
    await client.request('/api/login', { method: 'POST', body: { user: 'a' } })
    const [, init] = global.fetch.mock.calls[0]
    expect(init.headers['X-Auth-Token']).toBeUndefined()
    expect(init.headers['Content-Type']).toBe('application/json')
    expect(init.body).toBe('{"user":"a"}')
  })

  it('params 跳过空值(null/undefined/空串),其余按 URL 编码', async () => {
    mockJson(200, {})
    await client.request('/api/logs', {
      params: { from: '2026-09-01', to: '', user_id: undefined, page: 2 },
    })
    const [url] = global.fetch.mock.calls[0]
    expect(url).toBe('/api/logs?from=2026-09-01&page=2')
  })

  it('401 清空 token、通知订阅者并抛可读错误', async () => {
    client.setToken('T2')
    const seen = []
    client.onUnauthorized((p) => seen.push(p))
    mockJson(401, { msg: '会话已过期,请重新登录' })
    await expect(client.request('/api/device')).rejects.toMatchObject({ status: 401 })
    expect(client.getToken()).toBe('')
    expect(sessionStorage.getItem('dg_token')).toBeNull()
    expect(seen).toHaveLength(1)
  })

  it('非 2xx 用服务端 msg 作为错误文案,缺 msg 时回退状态码', async () => {
    mockJson(429, { msg: '尝试次数过多,请稍后再试' })
    await expect(client.request('/api/login')).rejects.toThrow('尝试次数过多,请稍后再试')
    mockJson(500, {})
    await expect(client.request('/api/device')).rejects.toThrow('请求失败(500)')
  })

  it('网络异常归一为"无法连接",而不是抛出 fetch 的原始错误', async () => {
    global.fetch.mockRejectedValueOnce(new TypeError('Failed to fetch'))
    await expect(client.request('/api/device')).rejects.toMatchObject({
      status: 0,
      message: '无法连接到设备,请检查网络',
    })
  })
})
