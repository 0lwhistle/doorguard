<!--
  UsersView.vue — 用户管理(列表 / 添加 / 编辑 / 删除 / 改密 / 清人脸)

  与设备端同一套业务规则:字段校验 JS 版先反馈,服务端(storage 层权威)
  兜底,错误文案直接呈现服务端 msg。删除/清人脸是受理制(经 enroll 服务
  事件,DB+特征库+头像一起动):返回后延时刷新列表确认。
-->
<script setup>
import { onMounted, reactive, ref } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import AppField from '../components/AppField.vue'
import AppPager from '../components/AppPager.vue'
import DataTable from '../components/DataTable.vue'
import { PAGE_SIZES, DEFAULT_PAGE_SIZE } from '../api/logs'
import {
  AUTH_FLAGS, NAME_HINT, PWD_HINT, ROLE_TEXT, ROLES, UID_HINT,
  addUser, clearUserFace, deleteUser, listUsers, setUserPwd, updateUser,
  validName, validPwd, validUid,
} from '../api/users'
import { toast } from '../stores/toast'

const COLUMNS = [
  { key: 'uid', label: '用户 ID' },
  { key: 'name', label: '姓名' },
  { key: 'roleText', label: '权限' },
  { key: 'authText', label: '验证方式' },
  { key: 'faceText', label: '人脸' },
]

const rows = ref([])
const loading = ref(false)
const page = ref(1)
const pages = ref(1)
const total = ref(0)
const pageSize = ref(DEFAULT_PAGE_SIZE)
const pageSizeOptions = PAGE_SIZES.map((n) => ({ value: n, label: String(n) }))

/* form: null=收起;add / edit(uid) 两用 */
const form = ref(null)
const busy = ref(false)
const f = reactive({ uid: '', name: '', pwd: '', role: 0, flags: [1, 4] })

const FLAGS_BIT_TEXT = { 1: '人脸', 2: '指纹', 4: '密码', 8: 'IC 卡' }

function authText(row) {
  return AUTH_FLAGS.filter((a) => row.auth_flags & a.bit).map((a) => a.label).join('/') || '—'
}

function decorate(row) {
  return {
    ...row,
    roleText: ROLE_TEXT[row.role] || '?',
    authText: authText(row),
    faceText: row.has_face ? '已录入' : '无',
  }
}

async function load(next = page.value) {
  loading.value = true
  try {
    const res = await listUsers({ page: next, pageSize: pageSize.value })
    rows.value = res.users.map(decorate)
    page.value = res.page
    pages.value = res.pages
    total.value = res.total
  } catch (err) {
    toast.err(err.message)
  } finally {
    loading.value = false
  }
}

function openAdd() {
  form.value = { mode: 'add' }
  f.uid = ''
  f.name = ''
  f.pwd = ''
  f.role = 0
  f.flags = [1, 4]
}

function openEdit(row) {
  form.value = { mode: 'edit', uid: row.uid, has_face: row.has_face }
  f.uid = row.uid
  f.name = row.name
  f.pwd = ''
  f.role = row.role
  f.flags = AUTH_FLAGS.filter((a) => row.auth_flags & a.bit).map((a) => a.bit)
}

function closeForm() {
  form.value = null
}

function flagsValue() {
  return f.flags.reduce((acc, bit) => acc | bit, 0)
}

/** 逐字段校验(与设备端 valid.c 同规则),返回第一个错误文案或 '' */
function validate({ needPwd }) {
  if (form.value.mode === 'add' && !validUid(f.uid.trim())) return UID_HINT
  if (!validName(f.name)) return NAME_HINT
  if (needPwd && !validPwd(f.pwd)) return PWD_HINT
  if (flagsValue() === 0) return '至少勾选一种验证方式'
  return ''
}

async function onSave() {
  const needPwd = form.value.mode === 'add'
  const errText = validate({ needPwd })
  if (errText) {
    toast.err(errText)
    return
  }
  busy.value = true
  try {
    if (form.value.mode === 'add') {
      const res = await addUser({
        uid: f.uid.trim(), name: f.name, pwd: f.pwd,
        role: f.role, authFlags: flagsValue(),
      })
      toast.ok(res.msg || '已添加')
    } else {
      const res = await updateUser({
        uid: form.value.uid, name: f.name,
        role: f.role, authFlags: flagsValue(),
      })
      toast.ok(res.msg || '已保存')
    }
    closeForm()
    await load(1)
  } catch (err) {
    toast.err(err.message)
  } finally {
    busy.value = false
  }
}

async function onResetPwd(row) {
  const pwd = window.prompt(`为用户 ${row.uid}(${row.name})设置新密码,4~31 位可见字符、不含空格:`)
  if (pwd === null) return
  if (!validPwd(pwd)) {
    toast.err(PWD_HINT)
    return
  }
  try {
    const res = await setUserPwd({ uid: row.uid, pwd })
    toast.ok(res.msg || '密码已更新')
  } catch (err) {
    toast.err(err.message)
  }
}

async function onClearFace(row) {
  if (!row.has_face) {
    toast.info('该用户没有已录人脸')
    return
  }
  if (!window.confirm(`确认清除 ${row.uid}(${row.name})的已录人脸?`)) return
  try {
    const res = await clearUserFace({ uid: row.uid })
    toast.ok(res.msg || '清除请求已受理')
    setTimeout(() => load(), 600)      // 受理制:稍候刷新看结果
  } catch (err) {
    toast.err(err.message)
  }
}

async function onDelete(row) {
  if (!window.confirm(`确认删除用户 ${row.uid}(${row.name})?该操作不可恢复。`)) return
  try {
    const res = await deleteUser({ uid: row.uid })
    toast.ok(res.msg || '删除请求已受理')
    if (form.value && form.value.mode === 'edit' && form.value.uid === row.uid)
      closeForm()
    setTimeout(() => load(), 600)
  } catch (err) {
    toast.err(err.message)
  }
}

onMounted(() => load())
</script>

<template>
  <AppCard v-if="!form" title="用户管理" :index="0">
    <div class="bar">
      <span class="muted small">共 {{ total }} 人;录入/重录人脸在设备端拍摄页完成</span>
      <AppButton icon="check" size="sm" @click="openAdd">添加用户</AppButton>
    </div>
    <DataTable :columns="COLUMNS" :rows="rows" row-key="uid" :loading="loading">
      <template #cell="{ row, col }">
        <span v-if="col.key === 'faceText'" :class="{ dim: !row.has_face }">{{ row.faceText }}</span>
        <span v-else>{{ row[col.key] }}</span>
      </template>
    </DataTable>
    <div class="rowops">
      <template v-for="row in rows" :key="row.uid">
        <div class="rowops__item">
          <span class="small">{{ row.uid }}</span>
          <span class="rowops__btns">
            <AppButton size="sm" variant="ghost" @click="openEdit(row)">编辑</AppButton>
            <AppButton size="sm" variant="ghost" @click="onResetPwd(row)">改密</AppButton>
            <AppButton size="sm" variant="ghost" @click="onClearFace(row)">清人脸</AppButton>
            <AppButton size="sm" variant="warn" @click="onDelete(row)">删除</AppButton>
          </span>
        </div>
      </template>
    </div>
    <AppPager :page="page" :pages="pages" :total="total" @change="load" />
  </AppCard>

  <AppCard v-else :title="form.mode === 'add' ? '添加用户' : `编辑用户 ${form.uid}`" :index="0">
    <div class="form">
      <AppField
        v-if="form.mode === 'add'"
        v-model="f.uid"
        label="用户 ID"
        size="sm"
        :hint="UID_HINT"
      />
      <AppField v-else label="用户 ID" :model-value="form.uid" size="sm" disabled />
      <AppField v-model="f.name" label="姓名" size="sm" :hint="NAME_HINT" />
      <AppField v-model="f.pwd" label="初始密码" type="password" size="sm" :hint="PWD_HINT" />
      <AppField v-model="f.role" label="权限" size="sm" :options="ROLES" />
      <div class="flags">
        <span class="small muted">验证方式</span>
        <label v-for="a in AUTH_FLAGS" :key="a.bit" class="flags__item">
          <input v-model="f.flags" type="checkbox" :value="a.bit" />
          <span>{{ a.label }}</span>
        </label>
      </div>
    </div>
    <div class="ops">
      <AppButton icon="check" :loading="busy" @click="onSave">保存</AppButton>
      <AppButton variant="ghost" @click="closeForm">取消</AppButton>
    </div>
    <p class="muted small">新用户必须设密码;人脸/指纹/IC 卡特征在设备端录入。权限与验证方式保存后立即生效。</p>
  </AppCard>
</template>

<style scoped>
.bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 10px;
  margin-bottom: 10px;
}
.rowops {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-top: 10px;
}
.rowops__item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-top: 1px dashed var(--line, #e5e7eb);
  padding-top: 4px;
}
.rowops__btns {
  display: flex;
  gap: 6px;
}
.form {
  display: flex;
  flex-direction: column;
  gap: 10px;
  margin-bottom: 14px;
}
.flags {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}
.flags__item {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-size: 13px;
}
.ops {
  display: flex;
  gap: 8px;
  margin-bottom: 10px;
}
.dim {
  opacity: 0.55;
}
</style>
