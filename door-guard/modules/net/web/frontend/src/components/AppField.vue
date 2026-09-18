<!--
  AppField.vue — 表单字段(label + input/select + 错误/提示)

  受控组件:modelValue + update:modelValue,错误文案由调用方给(error),
  不在这里做业务校验(校验规则在服务端,前端只呈现结果)。
-->
<script setup>
import { computed } from 'vue'

const props = defineProps({
  modelValue: { type: [String, Number], default: '' },
  label: { type: String, default: '' },
  type: { type: String, default: 'text' },
  placeholder: { type: String, default: '' },
  autocomplete: { type: String, default: 'off' },
  options: { type: Array, default: () => [] }, // [{value,label}] → 渲染 select
  size: { type: String, default: 'md' },
  error: { type: String, default: '' },
  hint: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
})

const emit = defineEmits(['update:modelValue', 'enter'])
const isSelect = computed(() => props.type === 'select')
const value = computed({
  get: () => props.modelValue,
  set: (v) => emit('update:modelValue', v),
})
</script>

<template>
  <label class="field" :class="`field--${size}`">
    <span v-if="label" class="field__label">{{ label }}</span>
    <select v-if="isSelect" v-model="value" :disabled="disabled">
      <option v-for="o in options" :key="o.value" :value="o.value">{{ o.label }}</option>
    </select>
    <input
      v-else
      v-model="value"
      :type="type"
      :placeholder="placeholder"
      :autocomplete="autocomplete"
      :disabled="disabled"
      @keydown.enter="emit('enter')"
    />
    <span v-if="error" class="field__error">{{ error }}</span>
    <span v-else-if="hint" class="field__hint">{{ hint }}</span>
  </label>
</template>

<style scoped>
.field {
  display: flex;
  flex-direction: column;
  gap: 4px;
  font-size: 13px;
  color: var(--muted);
}
.field__label {
  font-weight: 600;
}
.field input,
.field select {
  width: 100%;
  padding: 10px 12px;
  border: 1px solid var(--line);
  border-radius: var(--radius-sm);
  font: inherit;
  font-size: 14px;
  color: var(--text);
  background: #fbfdff;
  transition:
    border-color 0.18s,
    box-shadow 0.18s,
    background 0.18s;
}
.field--sm input,
.field--sm select {
  padding: 7px 10px;
  font-size: 13px;
}
.field input:focus,
.field select:focus {
  outline: none;
  border-color: var(--primary);
  background: #fff;
  box-shadow: 0 0 0 3px rgba(30, 136, 229, 0.15);
}
.field__error {
  color: var(--err);
}
.field__hint {
  color: var(--muted);
  font-size: 12px;
}
</style>
