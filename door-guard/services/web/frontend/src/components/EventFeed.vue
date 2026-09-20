<!--
  EventFeed.vue — 实时门禁事件列表

  只负责渲染传入的事件数组(最新的在最前),不做数据获取:数据来自
  stores/events(WebSocket),保证"演示组件"可单独测、可复用。
-->
<script setup>
import StatusPill from './StatusPill.vue'
import { isPass } from '../utils/format'

defineProps({
  items: { type: Array, required: true },
  emptyText: { type: String, default: '等待验证事件…' },
})
</script>

<template>
  <div class="feed" aria-live="polite">
    <p v-if="!items.length" class="feed__empty">{{ emptyText }}</p>
    <article
      v-for="ev in items"
      :key="ev.id ?? ev.ts"
      class="ev"
      :class="isPass(ev.result) ? 'ev--ok' : 'ev--err'"
    >
      <span class="ev__time">{{ ev.time }}</span>
      <span class="ev__name">{{ ev.user_name || '陌生人' }}</span>
      <span class="ev__meta small muted">
        {{ ev.user_id ? `ID ${ev.user_id} · ` : '' }}{{ ev.method_name }}
      </span>
      <StatusPill
        class="ev__pill"
        :kind="isPass(ev.result) ? 'pass' : 'deny'"
        :label="isPass(ev.result) ? '通过' : '拒绝'"
      />
    </article>
  </div>
</template>

<style scoped>
.feed {
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-height: 320px;
  overflow: auto;
  margin-bottom: 10px;
}
.feed__empty {
  margin: 0;
  padding: 18px 0;
  text-align: center;
  color: var(--muted);
}
.ev {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 9px 12px;
  border-radius: var(--radius-sm);
  border-left: 4px solid var(--primary);
  background: #f7fbff;
  font-size: 13px;
  animation: slide-in-left 0.38s cubic-bezier(0.2, 0.8, 0.3, 1) both;
}
.ev--ok {
  border-left-color: var(--ok);
  background: #f2fbf3;
}
.ev--err {
  border-left-color: var(--err);
  background: #fdf3f3;
}
.ev__time {
  min-width: 132px;
  font-size: 12px;
  color: var(--muted);
}
.ev__name {
  font-weight: 700;
}
.ev__pill {
  margin-left: auto;
}
</style>
