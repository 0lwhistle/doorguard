<!--
  AppPager.vue — 分页控件(上一页/下一页 + 位置提示)
-->
<script setup>
import AppButton from './AppButton.vue'

const props = defineProps({
  page: { type: Number, required: true },
  pages: { type: Number, required: true },
  total: { type: Number, default: 0 },
})
const emit = defineEmits(['change'])
</script>

<template>
  <div class="pager">
    <AppButton
      variant="ghost"
      size="sm"
      icon="left"
      :disabled="page <= 1"
      @click="emit('change', page - 1)"
    >
      上一页
    </AppButton>
    <span class="muted small">第 {{ page }} / {{ pages || 1 }} 页 · 共 {{ total }} 条</span>
    <AppButton
      variant="ghost"
      size="sm"
      :disabled="page >= (pages || 1)"
      @click="emit('change', page + 1)"
    >
      下一页
    </AppButton>
  </div>
</template>

<style scoped>
.pager {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  margin-top: 12px;
}
</style>
