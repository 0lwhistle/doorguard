/*
 * session.js — 会话与登录状态
 *
 * 负责:登录/注销、token 持久化(经 api/client)、默认口令标记、以及"会话失效
 * 回登录页"的接线。它是唯一知道"用户是谁/是否已登录"的地方。
 */
import { computed, reactive } from 'vue'
import * as authApi from '../api/auth'
import { getToken, onUnauthorized, setToken } from '../api/client'
import { toast } from './toast'

const state = reactive({
  token: getToken(),
  user: '',
  pwdDefault: false,
  busy: false,
  error: '',
})

/* 对外只读视图:组件读 session.user / session.pwdDefault;改状态一律经下面的函数 */
export const session = state

export const isLoggedIn = computed(() => !!state.token)

export async function doLogin(user, pwd) {
  state.busy = true
  state.error = ''
  try {
    const res = await authApi.login(user, pwd)
    setToken(res.token)
    state.token = res.token
    state.user = res.user || ''
    state.pwdDefault = !!res.pwd_default
    toast.ok(`登录成功,欢迎 ${state.user}`)
    return true
  } catch (err) {
    state.error = err.message
    return false
  } finally {
    state.busy = false
  }
}

export async function doLogout({ notify = true } = {}) {
  const had = !!state.token
  try {
    if (had) await authApi.logout()
  } catch {
    /* 注销失败不影响本地登出:token 已被服务端视为无效或本就过期 */
  }
  setToken('')
  state.token = ''
  state.user = ''
  state.pwdDefault = false
  if (notify && had) toast.info('已退出登录')
}

/** 服务端 401:清本地态并由路由守卫送到登录页 */
onUnauthorized((payload) => {
  if (!state.token) return
  state.token = ''
  state.error = (payload && payload.msg) || '会话已过期,请重新登录'
  toast.err(state.error)
})

/** 凭据修改成功后:本地一律登出,强制用新凭据登录 */
export function afterCredentialChange() {
  setToken('')
  state.token = ''
  state.user = ''
}

export function markPwdDefault(flag) {
  state.pwdDefault = !!flag
}
