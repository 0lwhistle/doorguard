/*
 * router/index.js — 路由表 + 登录守卫
 *
 * 视图一律静态导入:构建产物本来就是单 bundle(inlineDynamicImports),
 * 懒加载在运行期没有收益;而异步组件包在 <transition mode=out-in> 里会
 * 触发「导航完成但组件不挂载」的白屏(实测,2026-10-05),静态导入从
 * 根上消除这个组合。
 *
 * 守卫只做"有没有 token"这一件事(不做权限模型:上位机只有管理员一种角色)。
 */
import { createRouter, createWebHashHistory } from 'vue-router'
import LoginView from '../views/LoginView.vue'
import AppShell from '../layouts/AppShell.vue'
import DashboardView from '../views/DashboardView.vue'
import UsersView from '../views/UsersView.vue'
import SettingsView from '../views/SettingsView.vue'
import LogsView from '../views/LogsView.vue'
import AccountView from '../views/AccountView.vue'
import FirmwareView from '../views/FirmwareView.vue'
import VideoView from '../views/VideoView.vue'
import { getToken, onUnauthorized } from '../api/client'

const routes = [
  {
    path: '/login',
    name: 'login',
    component: LoginView,
    meta: { public: true, title: '登录' },
  },
  {
    path: '/',
    component: AppShell,
    children: [
      {
        path: '',
        name: 'dashboard',
        component: DashboardView,
        meta: { title: '设备概览' },
      },
      {
        path: 'users',
        name: 'users',
        component: UsersView,
        meta: { title: '用户管理' },
      },
      {
        path: 'settings',
        name: 'settings',
        component: SettingsView,
        meta: { title: '系统设置' },
      },
      {
        path: 'logs',
        name: 'logs',
        component: LogsView,
        meta: { title: '记录查询' },
      },
      {
        path: 'account',
        name: 'account',
        component: AccountView,
        meta: { title: '账号安全' },
      },
      {
        path: 'firmware',
        name: 'firmware',
        component: FirmwareView,
        meta: { title: '固件升级' },
      },
      {
        path: 'video',
        name: 'video',
        component: VideoView,
        meta: { title: '监控画面' },
      },
    ],
  },
  { path: '/:pathMatch(.*)*', redirect: '/' },
]

/**
 * 建路由。
 * @param {import('vue-router').RouterHistory} [history] 默认 hash 模式(设备端只
 *   提供固定资源表,不需要服务端给前端路由配回退);单测传 memory history,
 *   避免 jsdom 的 hashchange 异步事件干扰导航断言。
 */
export function createAppRouter(history = createWebHashHistory()) {
  const router = createRouter({ history, routes })

  router.beforeEach((to) => {
    const authed = !!getToken()
    if (!to.meta.public && !authed) {
      return { name: 'login', query: to.fullPath !== '/' ? { next: to.fullPath } : undefined }
    }
    if (to.name === 'login' && authed) return { name: 'dashboard' }
    return true
  })

  /* 会话中途失效(比如设备改了口令/会话超时):光清状态不够,得把人送回登录页,
   * 否则用户对着一个不再更新的页面以为一切正常。转跳逻辑放路由层,
   * 避免 api/client 依赖 router(反向依赖)。 */
  onUnauthorized(() => {
    const current = router.currentRoute.value
    if (current.name !== 'login') {
      router.replace({ name: 'login', query: { next: current.fullPath } })
    }
  })

  return router
}

export const router = createAppRouter()

router.afterEach((to) => {

  const t = to.meta && to.meta.title
  document.title = t ? `${t} · door-guard 上位机` : 'door-guard 上位机'
})
