<!--
  AppIcon.vue — 内联 SVG 图标集

  为什么不用图标字体/CDN:设备可能在无外网的局域网里,任何外部资源都会 404;
  内联路径也是最省字节的做法。新增图标在 ICONS 里加一条即可。
-->
<script setup>
import { computed } from 'vue'

const props = defineProps({
  name: { type: String, required: true },
  size: { type: [Number, String], default: 18 },
})

const ICONS = {
  door: 'M6 3h9a2 2 0 0 1 2 2v14H6zM15 12h.01',
  gauge: 'M12 14a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM13.5 10.5 19 5M4 20a9 9 0 1 1 16 0',
  list: 'M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01',
  key: 'M15 7a4 4 0 1 0-3.9 5H10l-1 1v2H7l-1 1v2H4v-3l7-7',
  upload: 'M12 16V4m0 0L8 8m4-4 4 4M4 20h16',
  video: 'M3 7a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2zM16 10l5-3v10l-5-3',
  logout: 'M15 12H3m0 0 4-4m-4 4 4 4M13 4h5a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-5',
  refresh: 'M20 12a8 8 0 1 1-2.3-5.7M20 4v4h-4',
  close: 'M6 6l12 12M18 6 6 18',
  warn: 'M12 3 2 20h20zM12 9v5m0 3h.01',
  check: 'M4 13l5 5L20 7',
  left: 'M15 5l-7 7 7 7',
  right: 'M9 5l7 7-7 7',
  clock: 'M12 7v5l3 2M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0z',
  user: 'M4 20a8 8 0 0 1 16 0M12 4a4 4 0 1 1 0 8 4 4 0 0 1 0-8z',
  lock: 'M6 11h12v9H6zM9 11V8a3 3 0 0 1 6 0v3',
  chip: 'M8 8h8v8H8zM4 9V6a2 2 0 0 1 2-2h3M20 15v3a2 2 0 0 1-2 2h-3M20 9V6a2 2 0 0 0-2-2h-3M4 15v3a2 2 0 0 0 2 2h3',
  search: 'M11 19a8 8 0 1 0 0-16 8 8 0 0 0 0 16zM21 21l-4.3-4.3',
  heart: 'M12 20s-7-4.3-7-9a4 4 0 0 1 7-2.6A4 4 0 0 1 19 11c0 4.7-7 9-7 9z',
}

const path = computed(() => ICONS[props.name] || ICONS.door)
</script>

<template>
  <svg
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.8"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
  >
    <path :d="path" />
  </svg>
</template>
