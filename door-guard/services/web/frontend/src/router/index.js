/*
 * router/index.js — 路由表 + 登录守卫
 *
 * 视图用 () => import() 声明(源码层面按需加载、模块边界清晰);构建时配置了
 * inlineDynamicImports,最终会合并进单个 app.js——固件里少一个 chunk 维度,
 * 代码里仍保持模块化写法。
 *
 * 守卫只做"有没有 token"这一件事(不做权限模型:上位机只有管理员一种角色)。
 */
import { createRouter, createWebHashHistory } from 'vue-router'
import { getToken, onUnauthorized } from '../api/client'

const routes = [
  {
    path: '/login',
    name: 'login',
    component: () => import('../views/LoginView.vue'),
    meta: { public: true, title: '登录' },
  },
  {
    path: '/',
    component: () => import('../layouts/AppShell.vue'),
    children: [
      {
        path: '',
        name: 'dashboard',
        component: () => import('../views/DashboardView.vue'),
        meta: { title: '设备概览' },
      },
      {
        path: 'logs',
        name: 'logs',
        component: () => import('../views/LogsView.vue'),
        meta: { title: '记录查询' },
      },
      {
        path: 'account',
        name: 'account',
        component: () => import('../views/AccountView.vue'),
        meta: { title: '账号安全' },
      },
      {
        path: 'firmware',
        name: 'firmware',
        component: () => import('../views/FirmwareView.vue'),
        meta: { title: '固件升级' },
      },
      {
        path: 'video',
        name: 'video',
        component: () => import('../views/VideoView.vue'),
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
