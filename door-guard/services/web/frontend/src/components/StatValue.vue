<!--
  StatValue.vue — 单个指标值(数字走滚动,文本直出)

  拆成独立组件(而不是在 StatGrid 里内联):数字动画靠 watch 单值,
  一个指标一个实例最直观,也便于单测。
-->
<script setup>
import { computed } from 'vue'
import { useCountUp } from '../composables/useCountUp'

const props = defineProps({
  value: { type: [String, Number], default: '—' },
  mono: { type: Boolean, default: false },
  animated: { type: Boolean, default: true },
})

const isNumber = computed(() => props.animated && typeof props.value === 'number')
const shown = useCountUp(computed(() => (isNumber.value ? props.value : 0)))
const text = computed(() => (isNumber.value ? String(shown.value) : String(props.value ?? '—')))
</script>

<template>
  <span class="stat__v" :class="{ mono }">{{ text }}</span>
</template>

<style scoped>
.stat__v {
  font-size: 19px;
  font-weight: 700;
  color: var(--primary-dark);
  word-break: break-all;
}
.stat__v.mono {
  font-size: 14px;
  font-weight: 600;
  font-family: var(--mono);
}
</style>
