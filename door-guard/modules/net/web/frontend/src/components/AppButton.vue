<!--
  AppButton.vue — 统一按钮(图标 + 文字 + 水波纹 + loading)

  纪律:所有可点元素都经此组件或 .chip(AppButton 的轻量变体),配色一律取
  token,组件内不写裸色值;动效只在 transform/opacity 上做。
-->
<script setup>
import AppIcon from './AppIcon.vue'

const props = defineProps({
  variant: { type: String, default: 'primary' }, // primary | ghost | warn
  size: { type: String, default: 'md' }, // md | sm
  icon: { type: String, default: '' },
  block: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  loading: { type: Boolean, default: false },
  type: { type: String, default: 'button' },
})

const emit = defineEmits(['click'])

function onPointerDown(e) {
  if (props.disabled || props.loading) return
  const el = e.currentTarget
  const rect = el.getBoundingClientRect()
  const size = Math.max(rect.width, rect.height)
  const span = document.createElement('span')
  span.className = 'ripple'
  span.style.width = `${size}px`
  span.style.height = `${size}px`
  span.style.left = `${e.clientX - rect.left - size / 2}px`
  span.style.top = `${e.clientY - rect.top - size / 2}px`
  el.appendChild(span)
  setTimeout(() => span.remove(), 600)
}

function onClick(e) {
  if (props.disabled || props.loading) return
  emit('click', e)
}
</script>

<template>
  <button
    class="btn"
    :class="[`btn--${variant}`, `btn--${size}`, { 'btn--block': block }]"
    :disabled="disabled || loading"
    :type="type"
    @pointerdown="onPointerDown"
    @click="onClick"
  >
    <span v-if="loading" class="spinner" aria-hidden="true"></span>
    <AppIcon v-else-if="icon" :name="icon" :size="size === 'sm' ? 15 : 18" />
    <span class="btn__label"><slot /></span>
  </button>
</template>

<style scoped>
.btn {
  position: relative;
  overflow: hidden;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  border: 0;
  border-radius: var(--radius-sm);
  font: inherit;
  font-weight: 600;
  cursor: pointer;
  color: #fff;
  background: linear-gradient(180deg, var(--primary), var(--primary-dark));
  transition:
    transform 0.12s,
    box-shadow 0.2s,
    filter 0.2s;
}
.btn--md {
  padding: 10px 18px;
  font-size: 14px;
}
.btn--sm {
  padding: 6px 12px;
  font-size: 13px;
}
.btn--block {
  width: 100%;
}
.btn:hover:not(:disabled) {
  box-shadow: var(--shadow-hover);
  filter: brightness(1.04);
}
.btn:active:not(:disabled) {
  transform: translateY(1px) scale(0.99);
}
.btn:disabled {
  opacity: 0.55;
  cursor: not-allowed;
  transform: none;
}
.btn--ghost {
  background: transparent;
  color: var(--primary-dark);
  border: 1px solid var(--line);
}
.btn--ghost:hover:not(:disabled) {
  background: var(--primary-light);
}
.btn--warn {
  background: linear-gradient(180deg, #ffb300, #f57f17);
}
.btn__label:empty {
  display: none;
}
.spinner {
  width: 15px;
  height: 15px;
  border-radius: 50%;
  border: 2px solid rgba(255, 255, 255, 0.45);
  border-top-color: #fff;
  animation: spin 0.8s linear infinite;
}
.ripple {
  position: absolute;
  border-radius: 50%;
  transform: scale(0);
  background: rgba(255, 255, 255, 0.55);
  pointer-events: none;
  animation: ripple 0.55s linear;
}
</style>
