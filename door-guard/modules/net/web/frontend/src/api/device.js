/*
 * device.js — 设备信息
 */
import { PATHS } from './endpoints'
import { request } from './client'

/** 设备快照:版本/运行时长/用户数/日志数/地址/mDNS/账号/NTP/存储 */
export function getDevice() {
  return request(PATHS.device)
}
