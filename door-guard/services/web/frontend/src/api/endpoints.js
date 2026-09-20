/*
 * endpoints.js — 接口路径唯一来源(与 web_server.c 路由表一一对应)
 *
 * 为什么单列一个文件:设备端路由一旦改名,前端要在编译前就发现——静态检查
 * (tests/web/frontend_check.py)会把这里的每个路径拿去 web_server.c 里核对,
 * 所以"前端调了不存在的接口"这类错不会等到板上才暴露。
 */
export const PATHS = {
  login: '/api/login',
  logout: '/api/logout',
  device: '/api/device',
  logs: '/api/logs',
  ntp: '/api/ntp',
  account: '/api/account',
  ws: '/api/ws',
}

export const STORAGE_KEY = 'dg_token'
