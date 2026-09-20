/*
 * logs.spec.js — 查询参数拼装(分页/过滤是这页最容易出错的地方)
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'

let logsApi

beforeEach(async () => {
  vi.resetModules()
  global.fetch = vi.fn().mockResolvedValue({ status: 200, ok: true, json: async () => ({ logs: [] }) })
  logsApi = await import('../../src/api/logs')
})

describe('api/logs', () => {
  it('默认参数:第 1 页、每页 20,不带过滤', async () => {
    await logsApi.queryLogs()
    expect(global.fetch.mock.calls[0][0]).toBe('/api/logs?page=1&page_size=20')
  })

  it('全部过滤条件都带上', async () => {
    await logsApi.queryLogs({
      from: '2026-09-01',
      to: '2026-09-18',
      userId: '10001',
      page: 3,
      pageSize: 50,
    })
    expect(global.fetch.mock.calls[0][0]).toBe(
      '/api/logs?from=2026-09-01&to=2026-09-18&user_id=10001&page=3&page_size=50',
    )
  })

  it('每页可选值与默认值(与服务端上限 100 对齐)', () => {
    expect(logsApi.PAGE_SIZES).toEqual([20, 50, 100])
    expect(logsApi.DEFAULT_PAGE_SIZE).toBe(20)
    expect(Math.max(...logsApi.PAGE_SIZES)).toBeLessThanOrEqual(100)
  })
})
