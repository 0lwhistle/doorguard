/*
 * accessset.js — 门禁/系统设置 API
 */
import { PATHS } from './endpoints'
import { request } from './client'

/** 读全部可编辑项(含当前值/范围/步进,范围由设备端 meta 表给出) */
export function getAccessSet() {
  return request(PATHS.accessSet)
}

/** 批量应用(设备端整批校验通过才落,任一非法整批拒绝) */
export function applyAccessSet(values) {
  return request(PATHS.accessSet, { method: 'POST', body: values })
}
