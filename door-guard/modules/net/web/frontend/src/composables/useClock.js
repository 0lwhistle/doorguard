/*
 * useClock.js — 每秒走字的时钟(时间同步卡片)
 */
import { onUnmounted, ref } from 'vue'

export function useClock() {
  const text = ref(formatTime(new Date()))
  const timer = setInterval(() => {
    text.value = formatTime(new Date())
  }, 1000)
  onUnmounted(() => clearInterval(timer))
  return text
}

function formatTime(d) {
  return [d.getHours(), d.getMinutes(), d.getSeconds()]
    .map((v) => String(v).padStart(2, '0'))
    .join(':')
}
