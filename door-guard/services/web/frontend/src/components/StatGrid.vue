<!--
  StatGrid.vue — 概览指标墙

  入参是纯数据 [{key,label,value,mono?,wide?,animated?}];组件不认识"设备"这个概念,
  只负责排版,所以换一套指标不用改组件(视图里组装数组即可)。
-->
<script setup>
import StatValue from './StatValue.vue'

defineProps({
  items: { type: Array, required: true },
})
</script>

<template>
  <div class="stats">
    <div
      v-for="item in items"
      :key="item.key"
      class="stat"
      :class="{ 'stat--wide': item.wide }"
    >
      <span class="stat__k">{{ item.label }}</span>
      <StatValue :value="item.value" :mono="item.mono" :animated="item.animated" />
    </div>
  </div>
</template>

<style scoped>
.stats {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 12px;
}
.stat {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 12px 14px;
  border: 1px solid #e8f1fb;
  border-radius: 12px;
  background: linear-gradient(180deg, #fafcff, var(--primary-light));
}
.stat--wide {
  grid-column: span 2;
}
.stat__k {
  font-size: 12px;
  color: var(--muted);
}
</style>
