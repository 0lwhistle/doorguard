/*
 * session.spec.js — 会话状态机
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'

let session, api

beforeEach(async () => {
  vi.resetModules()
  sessionStorage.clear()
  global.fetch = vi.fn()
  api = await import('../../src/api/client')
  session = await import('../../src/stores/session')
})

describe('stores/session', () => {
  it('登录成功:存 token + 账号 + 默认口令标记', async () => {
    global.fetch.mockResolvedValueOnce({
      status: 200,
      ok: true,
      json: async () => ({ token: 'abc', user: 'guard01', pwd_default: true }),
    })
    const ok = await session.doLogin('guard01', 'pw')
    expect(ok).toBe(true)
    expect(session.session.token).toBe('abc')
    expect(session.session.user).toBe('guard01')
    expect(session.session.pwdDefault).toBe(true)
    expect(api.getToken()).toBe('abc')
  })

  it('登录失败:留在登录页并保留服务端文案', async () => {
    global.fetch.mockResolvedValueOnce({
      status: 401,
      ok: false,
      json: async () => ({ msg: '账号或密码错误' }),
    })
    const ok = await session.doLogin('guard01', 'bad')
    expect(ok).toBe(false)
    expect(session.session.error).toBe('账号或密码错误')
    expect(session.session.token).toBe('')
  })

  it('登出清空本地态(即使注销接口失败)', async () => {
    session.session.token = 'abc'
    api.setToken('abc')
    global.fetch.mockRejectedValueOnce(new Error('boom'))
    await session.doLogout({ notify: false })
    expect(session.session.token).toBe('')
    expect(api.getToken()).toBe('')
  })

  it('改凭据后本地强制登出(服务端已吊销全部会话)', () => {
    session.session.token = 'abc'
    api.setToken('abc')
    session.afterCredentialChange()
    expect(session.session.token).toBe('')
    expect(api.getToken()).toBe('')
  })

  it('服务端 401 会把本地会话清掉(路由守卫据此送回登录页)', async () => {
    session.session.token = 'abc'
    api.setToken('abc')
    global.fetch.mockResolvedValueOnce({
      status: 401,
      ok: false,
      json: async () => ({ msg: '会话已过期,请重新登录' }),
    })
    await expect(api.request('/api/device')).rejects.toBeTruthy()
    expect(session.session.token).toBe('')
  })
})
