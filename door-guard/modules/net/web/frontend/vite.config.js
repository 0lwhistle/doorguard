/*
 * vite.config.js — 构建配置(产物要嵌进固件,所以处处以"稳定、可预期"为先)
 *
 * 关键决定:
 *   1) 产物文件名**固定不带 hash**(assets/app.js、assets/app.css):设备端把资源
 *      编成静态表按路径提供,文件名稳定 = 固件路由稳定;页面与资源同版本发布,
 *      服务端又是 no-store,不需要 hash 破缓存。
 *   2) 单入口单 bundle(inlineDynamicImports):路由仍按 `() => import()` 写(源码
 *      保持模块化/可懒加载),但构建期合并进 app.js,避免固件里出现一堆 chunk;
 *      全站体积 ~100KB,拆包收益远小于资源表复杂度。
 *   3) base 用默认 '/':与 web_server 的资源表路径(/assets/...)一一对应。
 */
import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  base: '/',
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2020',
    cssCodeSplit: false,
    assetsDir: 'assets',
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
        entryFileNames: 'assets/app.js',
        /* CSS 名固定为 app.css:与 app.js 对称,固件资源表与静态检查都按这两个名字断言 */
        assetFileNames: (info) =>
          (info.name || '').endsWith('.css') ? 'assets/app.css' : 'assets/[name][extname]',
      },
    },
  },
})
