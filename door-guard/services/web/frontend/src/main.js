/*
 * main.js — 应用入口
 *
 * 顺序:样式(tokens → base → animations)→ 路由 → 挂载。
 * 这里不做任何数据获取:需要登录态的数据都在 AppShell 挂载后按需拉。
 */
import { createApp } from 'vue'
import './styles/tokens.css'
import './styles/base.css'
import './styles/animations.css'
import App from './App.vue'
import { router } from './router'

createApp(App).use(router).mount('#app')
