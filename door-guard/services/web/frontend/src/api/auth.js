/*
 * auth.js — 登录/注销/改凭据
 */
import { PATHS } from './endpoints'
import { request } from './client'

/** 登录 → { token, expires_in, user, pwd_default } */
export function login(user, pwd) {
  return request(PATHS.login, { method: 'POST', body: { user, pwd } })
}

export function logout() {
  return request(PATHS.logout, { method: 'POST' })
}

/**
 * 改账号/口令。user 留空表示只改口令;两种都要旧口令(服务端强制)。
 */
export function changeAccount({ user, oldPwd, pwd }) {
  return request(PATHS.account, {
    method: 'POST',
    body: { user: user || '', old_pwd: oldPwd || '', pwd },
  })
}
