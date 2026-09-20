/*
 * ntp.js — 时间校正触发
 *
 * 服务端是异步的:POST 立刻回 202,真正的结果经 WebSocket 推回来
 * (chronyc waitsync 会阻塞十余秒,不能占住 HTTP 工作线程)。
 */
import { PATHS } from './endpoints'
import { request } from './client'

export function triggerNtp() {
  return request(PATHS.ntp, { method: 'POST' })
}
