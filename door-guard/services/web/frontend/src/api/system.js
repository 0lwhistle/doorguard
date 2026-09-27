/*
 * system.js — 系统操作(远程重启)
 *
 * 服务端 202 只代表受理:回执后约 1s 设备才真重启(HTTP 回执先落),
 * 重启期间门禁与上位机短暂不可用,恢复后原地址可重新访问。
 */
import { PATHS } from './endpoints'
import { request } from './client'

export function rebootSystem() {
  return request(PATHS.systemReboot, { method: 'POST' })
}
