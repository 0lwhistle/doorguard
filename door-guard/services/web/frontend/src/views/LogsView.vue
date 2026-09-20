<!--
  LogsView.vue — 门禁记录查询(筛选 + 分页)

  查询条件、分页游标都是本页局部状态(不放进全局 store:离开页面即丢弃,
  符合"筛选是页面的事");请求经 api/logs,错误统一走 toast。
-->
<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import AppField from '../components/AppField.vue'
import AppPager from '../components/AppPager.vue'
import DataTable from '../components/DataTable.vue'
import StatusPill from '../components/StatusPill.vue'
import { DEFAULT_PAGE_SIZE, PAGE_SIZES, queryLogs } from '../api/logs'
import { dateStr, daysAgo, isPass } from '../utils/format'
import { toast } from '../stores/toast'

const COLUMNS = [
  { key: 'time', label: '时间' },
  { key: 'user_id', label: '用户 ID' },
  { key: 'user_name', label: '姓名' },
  { key: 'method_name', label: '方式' },
  { key: 'result', label: '结果' },
]

const filters = reactive({
  from: '',
  to: '',
  userId: '',
  pageSize: DEFAULT_PAGE_SIZE,
})
const pageSizeOptions = PAGE_SIZES.map((n) => ({ value: n, label: String(n) }))

const rows = ref([])
const loading = ref(false)
const page = ref(1)
const pages = ref(1)
const total = ref(0)
const preset = ref('')

const rangeText = computed(() => (filters.from || filters.to ? `${filters.from || '…'} ~ ${filters.to || '…'}` : '全部时间'))

function applyPreset(name) {
  preset.value = name
  const today = new Date()
  if (name === 'today') {
    filters.from = dateStr(today)
    filters.to = dateStr(today)
  } else if (name === '7d') {
    filters.from = daysAgo(6, today)
    filters.to = dateStr(today)
  } else {
    filters.from = ''
    filters.to = ''
  }
  load(1)
}

async function load(next = page.value) {
  loading.value = true
  try {
    const res = await queryLogs({
      from: filters.from,
      to: filters.to,
      userId: filters.userId.trim(),
      page: next,
      pageSize: filters.pageSize,
    })
    rows.value = res.logs || []
    page.value = res.page || 1
    pages.value = res.pages || 1
    total.value = res.total || 0
  } catch (err) {
    rows.value = []
    toast.err(err.message)
  } finally {
    loading.value = false
  }
}

onMounted(() => load(1))
</script>

<template>
  <AppCard title="门禁记录查询" span2 :index="0">
    <div class="filters">
      <AppField v-model="filters.from" label="开始" type="date" size="sm" />
      <AppField v-model="filters.to" label="结束" type="date" size="sm" />
      <AppField v-model="filters.userId" label="用户 ID" size="sm" placeholder="可留空" @enter="load(1)" />
      <AppField v-model="filters.pageSize" label="每页" type="select" size="sm" :options="pageSizeOptions" />
      <div class="quick">
        <button
          v-for="p in [
            { key: 'today', label: '今天' },
            { key: '7d', label: '近 7 天' },
            { key: 'all', label: '全部' },
          ]"
          :key="p.key"
          class="chip"
          :class="{ 'chip--on': preset === p.key }"
          type="button"
          @click="applyPreset(p.key)"
        >
          {{ p.label }}
        </button>
      </div>
      <AppButton icon="search" :loading="loading" @click="load(1)">查询</AppButton>
    </div>

    <p class="muted small">当前范围:{{ rangeText }}</p>

    <DataTable
      :columns="COLUMNS"
      :rows="rows"
      :loading="loading"
      row-key="ts"
      :empty-text="loading ? '查询中…' : '该条件下没有记录'"
    >
      <template #cell-user_id="{ row }">{{ row.user_id || '—' }}</template>
      <template #cell-result="{ row }">
        <StatusPill :kind="isPass(row.result) ? 'pass' : 'deny'" :label="isPass(row.result) ? '通过' : '拒绝'" />
      </template>
    </DataTable>

    <AppPager :page="page" :pages="pages" :total="total" @change="load" />
  </AppCard>
</template>

<style scoped>
.filters {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-end;
  gap: 10px;
  margin-bottom: 12px;
}
.filters :deep(.field) {
  min-width: 130px;
}
.quick {
  display: flex;
  gap: 6px;
}
.chip {
  padding: 5px 12px;
  border: 1px solid var(--line);
  border-radius: var(--radius-pill);
  background: #fff;
  color: var(--primary-dark);
  font: inherit;
  font-size: 12px;
  cursor: pointer;
  transition: all 0.18s;
}
.chip:hover {
  border-color: var(--primary);
  background: var(--primary-light);
}
.chip--on {
  border-color: var(--primary);
  background: var(--primary);
  color: #fff;
}
</style>
