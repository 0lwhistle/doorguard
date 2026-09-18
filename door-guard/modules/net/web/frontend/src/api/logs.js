/*
 * logs.js — 门禁记录查询
 */
import { PATHS } from './endpoints'
import { request } from './client'

export const PAGE_SIZES = [20, 50, 100]
export const DEFAULT_PAGE_SIZE = 20

/**
 * 查询记录(参数留空即不过滤)。
 * @param {{from?:string,to?:string,userId?:string,page?:number,pageSize?:number}} q
 */
export function queryLogs({ from, to, userId, page = 1, pageSize = DEFAULT_PAGE_SIZE } = {}) {
  return request(PATHS.logs, {
    params: {
      from,
      to,
      user_id: userId,
      page,
      page_size: pageSize,
    },
  })
}
