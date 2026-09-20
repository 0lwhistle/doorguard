/*
 * useCountUp.js — 数字滚动(概览指标)
 *
 * 只对"变了的值"做动画:轮询每 30s 刷新一次,如果每次都从 0 跳,概览会一直
 * 在抖;这里以上一次显示值为起点,并且尊重 prefers-reduced-motion。
 */
import { ref, watch } from 'vue'

const STEPS = 18
const STEP_MS = 24

export function useCountUp(source, { duration = STEPS * STEP_MS } = {}) {
  const display = ref(Number(source.value) || 0)
  let timer = null

  function animate(to) {
    const from = Number(display.value) || 0
    if (from === to) return
    if (window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      display.value = to
      return
    }
    if (timer) clearInterval(timer)
    const startedAt = Date.now()
    timer = setInterval(() => {
      const p = Math.min(1, (Date.now() - startedAt) / duration)
      display.value = Math.round(from + (to - from) * p)
      if (p >= 1) {
        clearInterval(timer)
        timer = null
      }
    }, STEP_MS)
  }

  watch(source, (v) => animate(Number(v) || 0))
  return display
}
