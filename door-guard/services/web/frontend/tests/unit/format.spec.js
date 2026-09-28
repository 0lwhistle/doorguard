/*
 * format.spec.js — 展示格式化纯函数(边界值逐个锁死)
 */
import { describe, expect, it } from 'vitest'
import {
  connText,
  dateStr,
  daysAgo,
  ipv4Autodot,
  ipv4Complete,
  ipv4Normalize,
  ipv4Octets,
  ipv4Pad,
  isPass,
} from '../../src/utils/format'

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

describe('utils/format ipv4(点号固定输入,与设备端同语义)', () => {
  it('ipv4Octets:形态 4 段 0~255(允许前导零),非法 null', () => {
    expect(ipv4Octets('192.168.1.10')).toEqual([192, 168, 1, 10])
    expect(ipv4Octets('192.168.001.010')).toEqual([192, 168, 1, 10])
    expect(ipv4Octets('255.255.255.255')).toEqual([255, 255, 255, 255])
    expect(ipv4Octets('256.1.1.1')).toBeNull()
    expect(ipv4Octets('1.2.3')).toBeNull()
    expect(ipv4Octets('1.2.3.4.5')).toBeNull()
    expect(ipv4Octets('1..2.3')).toBeNull()
    expect(ipv4Octets('abc')).toBeNull()
    expect(ipv4Octets('')).toBeNull()
    expect(ipv4Octets(null)).toBeNull()
  })

  it('ipv4Autodot:抽数字按 3 位一组重插点号,12 位封顶', () => {
    expect(ipv4Autodot('192168001010')).toBe('192.168.001.010')
    expect(ipv4Autodot('192168')).toBe('192.168')
    expect(ipv4Autodot('192.168.001.010')).toBe('192.168.001.010') // 恒等
    expect(ipv4Autodot('192x168')).toBe('192.168')                 // 非数字剥掉
    expect(ipv4Autodot('1234567890123')).toBe('123.456.789.012')   // 13 位截 12
    expect(ipv4Autodot('')).toBe('')
    expect(ipv4Autodot(null)).toBe('')
  })

  it('ipv4Complete:恰 12 位数字', () => {
    expect(ipv4Complete('192.168.001.010')).toBe(true)
    expect(ipv4Complete('192.168.1.10')).toBe(false)   // 9 位(规范形不算满)
    expect(ipv4Complete('192.168.001')).toBe(false)
    expect(ipv4Complete('')).toBe(false)
  })

  it('ipv4Normalize:去段内前导零;非法原样返回', () => {
    expect(ipv4Normalize('192.168.001.010')).toBe('192.168.1.10')
    expect(ipv4Normalize('0.0.0.0')).toBe('0.0.0.0')
    expect(ipv4Normalize('1.2.3')).toBe('1.2.3')
    expect(ipv4Normalize('')).toBe('')
  })

  it('ipv4Pad:补零 12 位;空入空出(网关可空);非法原样', () => {
    expect(ipv4Pad('192.168.1.10')).toBe('192.168.001.010')
    expect(ipv4Pad('1.2.3.4')).toBe('001.002.003.004')
    expect(ipv4Pad('')).toBe('')
    expect(ipv4Pad('300.1.1.1')).toBe('300.1.1.1')
  })
})
