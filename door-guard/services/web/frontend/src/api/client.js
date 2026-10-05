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
 * 带上传进度的原始体上传(XHR;fetch 拿不到 upload progress)。
 * @param {string} path 接口路径
 * @param {Blob|File} blob 原始请求体
 * @param {{headers?:object, onProgress?:(pct:number)=>void,
 *          signal?:AbortSignal}} opts
 * @returns {Promise<object>} 解析后的 JSON
 * @throws {ApiError} 网络失败/非 2xx/中止
 */
export function uploadWithProgress(path, blob, opts = {}) {
  const { headers = {}, onProgress, signal } = opts
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', path, true)
    xhr.responseType = 'text'
    if (token) xhr.setRequestHeader('X-Auth-Token', token)
    xhr.setRequestHeader('Content-Type', 'application/octet-stream')
    Object.entries(headers).forEach(([k, v]) => xhr.setRequestHeader(k, v))
    if (signal) {
      const abort = () => xhr.abort()
      signal.addEventListener('abort', abort, { once: true })
      xhr.addEventListener('loadend', () => signal.removeEventListener('abort', abort))
    }
    if (onProgress) {
      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) onProgress(Math.round((e.loaded / e.total) * 100))
      })
    }
    xhr.addEventListener('load', () => {
      let payload = null
      try { payload = JSON.parse(xhr.responseText) } catch { payload = null }
      if (xhr.status === 401) {
        setToken('')
        notifyUnauthorized(payload)
        reject(new ApiError((payload && payload.msg) || '会话已过期,请重新登录', 401, payload))
        return
      }
      if (xhr.status < 200 || xhr.status >= 300) {
        reject(new ApiError((payload && payload.msg) || `请求失败(${xhr.status})`, xhr.status, payload))
        return
      }
      resolve(payload || {})
    })
    xhr.addEventListener('error', () => reject(new ApiError('网络中断,上传未完成', 0, null)))
    xhr.addEventListener('abort', () => reject(new ApiError('上传已取消', 0, { aborted: true })))
    xhr.send(blob)
  })
}

/**
 * 发起请求。
 * @param {string} path 接口路径(见 endpoints.js)
 * @param {{method?:string, body?:object, rawBody?:Blob|string, rawType?:string,
 *          params?:object, signal?:AbortSignal}} opts
 *     rawBody:原始请求体(如人脸 JPEG 上传),与 body 互斥;
 *     rawType 为其 Content-Type(默认 application/octet-stream)。
 * @returns {Promise<object>} 解析后的 JSON
 * @throws {ApiError} 网络失败/非 2xx
 */
export async function request(path, opts = {}) {
  const { method = 'GET', body, rawBody, rawType, params, signal } = opts
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
  if (rawBody !== undefined) headers['Content-Type'] = rawType || 'application/octet-stream'
  else if (body !== undefined) headers['Content-Type'] = 'application/json'

  let res
  try {
    res = await fetch(url, {
      method,
      headers,
      body:
        rawBody !== undefined ? rawBody
        : body === undefined ? undefined
        : JSON.stringify(body),
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
