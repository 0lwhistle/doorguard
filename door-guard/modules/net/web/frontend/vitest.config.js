/*
 * vitest.config.js — 单测配置(与构建共用别名,但独立成文件:
 * 构建配置只关心产物形态,测试配置只关心 jsdom 环境,互不干扰)
 */
import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['tests/**/*.spec.js'],
    restoreMocks: true,
  },
})
