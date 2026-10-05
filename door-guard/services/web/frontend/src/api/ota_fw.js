/*
 * ota_fw.js — web 固件升级(升级包槽)接口
 *
 * 全链路三段:前端预检(parseOtaHeader,结构级;浏览器 http 非安全上下文
 * 没有 crypto.subtle,算不了 sha256,摘要复核在板端两道:收包后入槽时 +
 * apply 提取时)→ 流式上传(带进度/断点续传)→ apply 交装(S60 装槽重启)。
 */
import { request, uploadWithProgress } from './client'

/** 解析 .ota 包头(结构级校验;返回 {version,date,payloadSize,sha256Hex}) */
export function parseOtaHeader(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onerror = () => reject(new Error('读取文件失败'))
    reader.onload = () => {
      const buf = new Uint8Array(reader.result)
      if (buf.length < 96) {
        reject(new Error('文件比 .ota 包头(96B)还小'))
        return
      }
      const magic = String.fromCharCode(...buf.slice(0, 6))
      if (magic !== 'DGOTA1' || buf[6] !== 0 || buf[7] !== 0) {
        reject(new Error('不是 door-guard .ota 升级包(magic 不符)'))
        return
      }
      const be32 = (o) => ((buf[o] << 24) | (buf[o+1] << 16) | (buf[o+2] << 8) | buf[o+3]) >>> 0
      const headerLen = be32(8)
      const payloadSize = be32(12)
      if (headerLen !== 96) {
        reject(new Error('包头版本不支持(header_len=' + headerLen + ')'))
        return
      }
      if (payloadSize === 0) {
        reject(new Error('包头载荷大小为 0'))
        return
      }
      const str = (o, cap) => {
        let end = o
        while (end < o + cap && buf[end] !== 0) end++
        return new TextDecoder().decode(buf.slice(o, end))
      }
      const version = str(16, 32)
      const date = str(48, 16)
      let hex = ''
      for (let i = 64; i < 96; i++) hex += buf[i].toString(16).padStart(2, '0')
      if (!version) {
        reject(new Error('包头缺版本号(打包工具损坏?)'))
        return
      }
      if (file.size !== headerLen + payloadSize) {
        reject(new Error('文件大小与包头不符(应为 ' + (headerLen + payloadSize) +
                         'B,实际 ' + file.size + 'B;包损坏或下载不完整)'))
        return
      }
      resolve({ version, date, payloadSize, sha256Hex: hex })
    }
    reader.readAsArrayBuffer(file.slice(0, 96))
  })
}

/** 槽位状态(GET /api/ota/fw) */
export function getFwSlot() {
  return request('/api/ota/fw')
}

/** 上传到槽(整个 .ota;offset 续传;onProgress 0~100) */
export function uploadFw(file, { offset = 0, onProgress, signal } = {}) {
  return uploadWithProgress('/api/ota/fw/upload', file.slice(offset), {
    headers: {
      'X-OTA-Size': String(file.size),
      ...(offset > 0 ? { 'X-OTA-Offset': String(offset) } : {}),
    },
    onProgress,
    signal,
  })
}

/** 删除槽内包(DELETE /api/ota/fw) */
export function deleteFw() {
  return request('/api/ota/fw', { method: 'DELETE' })
}

/** 提取校验并交装(POST /api/ota/fw/apply;202 = 受理) */
export function applyFw() {
  return request('/api/ota/fw/apply', { method: 'POST' })
}
