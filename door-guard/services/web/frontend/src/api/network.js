/*
 * network.js — 设备网络配置(读快照 / 应用配置)
 *
 * 写是异步的:POST 回 202 只代表"已受理并开始应用"——真正的结果经
 * WebSocket 的 net 事件推送(应用静态地址可能切走本连接,响应先出门)。
 */
import { PATHS } from './endpoints'
import { request } from './client'

/** 网络快照:接口/IP/掩码/网关(未拿到=0.0.0.0)/模式/在线状态 */
export function getNetwork() {
  return request(PATHS.network)
}

/**
 * 应用网络配置。
 * @param {{mode:'dhcp'}|{mode:'static',ip:string,netmask:string,gateway:string}} cfg
 */
export function setNetwork(cfg) {
  return request(PATHS.network, { method: 'POST', body: cfg })
}
