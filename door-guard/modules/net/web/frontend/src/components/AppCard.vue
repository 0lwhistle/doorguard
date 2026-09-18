<!--
  AppCard.vue — 内容卡片(标题 + 可选角标/操作 + 默认插槽)

  index 用于错峰入场:列表里第 n 张卡延迟 n*70ms 出现,页面"铺开"的感觉来自这里。
-->
<script setup>
const props = defineProps({
  title: { type: String, default: '' },
  index: { type: Number, default: 0 },
  span2: { type: Boolean, default: false },
  badge: { type: String, default: '' },
})

const style = { '--i': props.index }
</script>

<template>
  <section class="card anim-rise" :class="{ 'card--span2': span2 }" :style="style">
    <header v-if="title" class="card__head">
      <h2>{{ title }}</h2>
      <span v-if="badge" class="card__badge">{{ badge }}</span>
      <div class="card__actions"><slot name="actions" /></div>
    </header>
    <slot />
  </section>
</template>

<style scoped>
.card {
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  padding: 18px;
  box-shadow: var(--shadow);
  animation-delay: calc(var(--i, 0) * 70ms);
  transition:
    box-shadow 0.25s,
    transform 0.25s;
}
.card:hover {
  box-shadow: var(--shadow-hover);
  transform: translateY(-2px);
}
.card--span2 {
  grid-column: span 2;
}
.card__head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 12px;
}
.card__head h2 {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 16px;
}
.card__head h2::before {
  content: '';
  width: 4px;
  height: 16px;
  border-radius: 2px;
  background: var(--primary);
}
.card__badge {
  font-size: 10px;
  letter-spacing: 0.5px;
  padding: 2px 7px;
  border-radius: var(--radius-pill);
  background: var(--err);
  color: #fff;
  animation: blink 1.6s ease-in-out infinite;
}
.card__actions {
  margin-left: auto;
}
@media (max-width: 820px) {
  .card--span2 {
    grid-column: span 1;
  }
}
</style>
