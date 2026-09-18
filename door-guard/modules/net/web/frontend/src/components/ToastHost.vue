<!--
  ToastHost.vue — 提示容器(纯展示:只按 props 渲染)

  数据接线在 App.vue(它才是允许碰 store 的地方):组件层一律 props/emits,
  这条纪律由 tests/web/frontend_check.py 自动检查。
-->
<script setup>
defineProps({
  items: { type: Array, required: true },
})
</script>

<template>
  <div class="toasts" aria-live="polite">
    <transition-group name="toast">
      <div
        v-for="t in items"
        :key="t.id"
        class="toast"
        :class="[`toast--${t.kind}`, { 'toast--out': t.leaving }]"
      >
        {{ t.message }}
      </div>
    </transition-group>
  </div>
</template>

<style scoped>
.toasts {
  position: fixed;
  right: 18px;
  bottom: 18px;
  z-index: 50;
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.toast {
  min-width: 220px;
  padding: 11px 16px;
  border-left: 4px solid var(--primary);
  border-radius: var(--radius-sm);
  background: #fff;
  box-shadow: 0 8px 24px rgba(21, 101, 192, 0.18);
  font-size: 13px;
  animation: toast-in 0.32s cubic-bezier(0.2, 0.8, 0.3, 1) both;
}
.toast--ok {
  border-left-color: var(--ok);
}
.toast--err {
  border-left-color: var(--err);
}
.toast--out {
  animation: toast-out 0.28s both;
}
</style>
