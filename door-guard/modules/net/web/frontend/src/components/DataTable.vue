<!--
  DataTable.vue — 轻量数据表

  columns: [{key,label,width?}];行内容默认直出,需要徽章这类富渲染时用作用域插槽
  #cell-<key>(例如 <template #cell-result="{row}">)。
  加载/空态也在组件内,避免每个页面各写一遍骨架。
-->
<script setup>
defineProps({
  columns: { type: Array, required: true },
  rows: { type: Array, required: true },
  rowKey: { type: String, default: 'id' },
  loading: { type: Boolean, default: false },
  emptyText: { type: String, default: '没有数据' },
})
</script>

<template>
  <div class="wrap">
    <table class="tbl">
      <thead>
        <tr>
          <th v-for="c in columns" :key="c.key" :style="c.width ? { width: c.width } : null">
            {{ c.label }}
          </th>
        </tr>
      </thead>
      <tbody>
        <tr v-if="loading">
          <td :colspan="columns.length" class="tbl__empty">查询中…</td>
        </tr>
        <tr v-else-if="!rows.length">
          <td :colspan="columns.length" class="tbl__empty">{{ emptyText }}</td>
        </tr>
        <tr
          v-for="(row, i) in rows"
          v-else
          :key="row[rowKey] ?? i"
          class="tbl__row"
          :style="{ animationDelay: `${i * 18}ms` }"
        >
          <td v-for="c in columns" :key="c.key">
            <slot :name="`cell-${c.key}`" :row="row">{{ row[c.key] }}</slot>
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<style scoped>
.wrap {
  overflow-x: auto;
  border: 1px solid var(--line);
  border-radius: var(--radius-sm);
}
.tbl {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}
.tbl th {
  position: sticky;
  top: 0;
  padding: 10px;
  text-align: left;
  font-weight: 700;
  color: var(--primary-dark);
  background: var(--primary-light);
}
.tbl td {
  padding: 9px 10px;
  border-top: 1px solid var(--line);
}
.tbl__row {
  animation: fade-in 0.3s both;
}
.tbl__row:hover {
  background: #f5faff;
}
.tbl__empty {
  padding: 18px;
  text-align: center;
  color: var(--muted);
}
</style>
