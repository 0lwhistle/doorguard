<!--
  SettingsView.vue — 门禁/系统设置(web 侧统一入口)

  范围/步进由设备端 meta 表下发(单一权威),前端只做即时提示;提交是
  整批的——设备端先整批校验再统一应用,任一非法整批拒绝,不会出现"改一半"。
-->
<script setup>
import { onMounted, reactive, ref } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import AppField from '../components/AppField.vue'
import { applyAccessSet, getAccessSet } from '../api/accessset'
import { toast } from '../stores/toast'

const GROUPS = [
  { title: '门禁', keys: ['door_open_ms', 'pwd_fail_lock_n', 'pwd_fail_lock_s'] },
  { title: '人脸识别', keys: ['match_threshold', 'min_face_px', 'blur_min', 'det_score_min', 'det_threshold', 'lost_hold_ms'] },
  { title: '超时', keys: ['standby_timeout_s', 'menu_timeout_s'] },
]

const items = ref([])                 // 设备端下发的全部字段(含范围)
const draft = reactive({})            // key → 输入串
const busy = ref(false)
const loading = ref(false)

function itemOf(key) {
  return items.value.find((i) => i.key === key)
}

/** 单项即时校验:'' = 合法 */
function checkOne(it) {
  const v = Number(draft[it.key])
  if (draft[it.key] === '' || Number.isNaN(v)) return '需为数值'
  if (v < it.min || v > it.max) return `范围 ${it.min}~${it.max}${it.unit ? ' ' + it.unit : ''}`
  if (!it.is_dbl && !Number.isInteger(v)) return '需为整数'
  if (it.is_dbl) {
    const decimals = (String(draft[it.key]).split('.')[1] || '').length
    if (decimals > 2) return '最多两位小数'
  }
  return ''
}

function errText(key) {
  const it = itemOf(key)
  return it ? checkOne(it) : ''
}

function dirtyCount() {
  return items.value.filter((it) => Number(draft[it.key]) !== it.value).length
}

async function load() {
  loading.value = true
  try {
    const res = await getAccessSet()
    items.value = res.items
    for (const it of res.items) draft[it.key] = String(it.value)
  } catch (err) {
    toast.err(err.message)
  } finally {
    loading.value = false
  }
}

async function onSave() {
  for (const it of items.value) {
    const e = checkOne(it)
    if (e) {
      toast.err(`${it.label}:${e}`)
      return
    }
  }
  const values = {}
  for (const it of items.value) values[it.key] = Number(draft[it.key])
  busy.value = true
  try {
    const res = await applyAccessSet(values)
    toast.ok(res.msg || '已保存')
    await load()
  } catch (err) {
    toast.err(err.message)
  } finally {
    busy.value = false
  }
}

onMounted(load)
</script>

<template>
  <AppCard title="门禁与系统设置" :index="0">
    <p class="muted small">
      取值范围由设备端下发并强校验;整批保存,任一项非法则全部不生效。
      <span v-if="dirtyCount()" class="dirty">有 {{ dirtyCount() }} 项未保存</span>
    </p>
    <div v-for="g in GROUPS" :key="g.title" class="group">
      <h3 class="group__title">{{ g.title }}</h3>
      <div class="form">
        <AppField
          v-for="key in g.keys"
          :key="key"
          v-model="draft[key]"
          :label="itemOf(key) ? `${itemOf(key).label}${itemOf(key).unit ? '(' + itemOf(key).unit + ')' : ''}` : key"
          size="sm"
          :error="errText(key)"
          :hint="itemOf(key) ? `范围 ${itemOf(key).min}~${itemOf(key).max}` : ''"
        />
      </div>
    </div>
    <div class="ops">
      <AppButton icon="check" :loading="busy" :disabled="loading" @click="onSave">保存全部</AppButton>
      <AppButton variant="ghost" :disabled="busy" @click="load">放弃修改</AppButton>
    </div>
  </AppCard>
</template>

<style scoped>
.group {
  margin-bottom: 16px;
}
.group__title {
  font-size: 13px;
  font-weight: 600;
  margin: 0 0 8px;
  opacity: 0.7;
}
.form {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 10px 14px;
}
.ops {
  display: flex;
  gap: 8px;
}
.dirty {
  color: #b45309;
  font-weight: 600;
}
</style>
