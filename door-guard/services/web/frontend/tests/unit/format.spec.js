/*
 * format.spec.js — 展示格式化纯函数(边界值逐个锁死)
 */
import { describe, expect, it } from 'vitest'
import { connText, dateStr, daysAgo, isPass } from '../../src/utils/format'

describe('utils/format', () => {
  it('dateStr 用本地时区补零,不受 UTC 偏移影响', () => {
    expect(dateStr(new Date(2026, 0, 5))).toBe('2026-01-05')
    expect(dateStr(new Date(2026, 11, 31))).toBe('2026-12-31')
  })

  it('daysAgo 跨月/跨年正确(近 7 天筛选用)', () => {
    const now = new Date(2026, 2, 3) // 3 月 3 日
    expect(daysAgo(0, now)).toBe('2026-03-03')
    expect(daysAgo(6, now)).toBe('2026-02-25')
    expect(daysAgo(30, now)).toBe('2026-02-01')
  })

  it('isPass 只认 0(与服务端 result=0 一致)', () => {
    expect(isPass(0)).toBe(true)
    expect(isPass('0')).toBe(true)
    expect(isPass(1)).toBe(false)
    expect(isPass(null)).toBe(false)
  })

  it('connText 覆盖三种连接状态', () => {
    expect(connText('open')).toBe('实时连接')
    expect(connText('connecting')).toBe('连接中…')
    expect(connText('closed')).toBe('已断开')
    expect(connText(undefined)).toBe('已断开')
  })
})
