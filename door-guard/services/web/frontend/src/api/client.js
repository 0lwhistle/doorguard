/*
 * client.js — HTTP 客户端(全站唯一的 fetch 出口)
 *
 * 分层纪律:视图与组件不直接调 fetch,一律经 api/*.js;token 注入、401 处理、
 * 错误归一都只在这里做一次。401 不做跳转(那是路由层的事),只通知订阅者。
 */
import { STORAGE_KEY } from './endpoints'

export class ApiError extends Error {
  constructor(message, status, payload) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.payload = payload || null
  }
}

let token = sessionStorage.getItem(STORAGE_KEY) || ''
const unauthorizedHandlers = new Set()

/** 当前 token(只读) */
export function getToken() {
  return token
}

/** 写入/清除 token(sessionStorage:关标签页即失效,门禁运维场景够用) */
export function setToken(value) {
  token = value || ''
  if (token) sessionStorage.setItem(STORAGE_KEY, token)
  else sessionStorage.removeItem(STORAGE_KEY)
}

/** 订阅"会话失效"(401):session store 用它回登录页 */
export function onUnauthorized(handler) {
  unauthorizedHandlers.add(handler)
  return () => unauthorizedHandlers.delete(handler)
}

function notifyUnauthorized(payload) {
  unauthorizedHandlers.forEach((h) => h(payload))
}

/**
 * 发起请求。
 * @param {string} path 接口路径(见 endpoints.js)
 * @param {{method?:string, body?:object, params?:object, signal?:AbortSignal}} opts
 * @returns {Promise<object>} 解析后的 JSON
 * @throws {ApiError} 网络失败/非 2xx
 */
export async function request(path, opts = {}) {
  const { method = 'GET', body, params, signal } = opts
  let url = path
  if (params) {
    const qs = new URLSearchParams()
    Object.entries(params).forEach(([k, v]) => {
      if (v !== undefined && v !== null && v !== '') qs.set(k, String(v))
    })
    const q = qs.toString()
    if (q) url += `?${q}`
  }

  const headers = {}
  if (token) headers['X-Auth-Token'] = token
  if (body !== undefined) headers['Content-Type'] = 'application/json'

  let res
  try {
    res = await fetch(url, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      signal,
    })
  } catch (err) {
    if (err.name === 'AbortError') throw err
    throw new ApiError('无法连接到设备,请检查网络', 0, null)
  }

  let payload = null
  try {
    payload = await res.json()
  } catch {
    payload = null
  }

  if (res.status === 401) {
    setToken('')
    notifyUnauthorized(payload)
    throw new ApiError((payload && payload.msg) || '会话已过期,请重新登录', 401, payload)
  }
  if (!res.ok) {
    throw new ApiError(
      (payload && payload.msg) || `请求失败(${res.status})`,
      res.status,
      payload,
    )
  }
  return payload || {}
}
