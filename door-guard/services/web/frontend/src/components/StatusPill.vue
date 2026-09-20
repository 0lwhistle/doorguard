<!--
  StatusPill.vue — 状态徽章(通过/拒绝/连接点)
-->
<script setup>
const props = defineProps({
  kind: { type: String, default: 'pass' }, // pass | deny | neutral
  label: { type: String, required: true },
})
</script>

<template>
  <span class="res" :class="`res--${kind}`">{{ label }}</span>
</template>

<style scoped>
.res {
  display: inline-block;
  padding: 1px 9px;
  border-radius: var(--radius-pill);
  font-size: 12px;
  font-weight: 600;
}
.res--pass {
  background: var(--ok-bg);
  color: var(--ok);
}
.res--deny {
  background: var(--err-bg);
  color: var(--err);
}
.res--neutral {
  background: var(--primary-light);
  color: var(--primary-dark);
}
</style>
