<!--
  UsersView.vue — 用户管理(列表 / 添加 / 编辑 / 删除 / 改密 / 人脸与 IC 卡录入)

  与设备端同一套业务规则:字段校验 JS 版先反馈,服务端(storage 层权威)
  兜底,错误文案直接呈现服务端 msg。删除/清人脸/人脸录入是受理制(经
  enroll 服务与视觉后端,DB+特征库+头像一起动),结果经 WS enroll 消息
  (seq 配对)回推后刷新确认;IC 卡绑定是同步落库(卡号直输,无提取链路)。
  验证方式勾选受「方式位 ⇒ 已录凭据」约束:未录入的方式在服务端会被拒,
  前端直接禁用并标注。
-->
<script setup>
import { onMounted, onUnmounted, reactive, ref } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import AppField from '../components/AppField.vue'
import AppPager from '../components/AppPager.vue'
import DataTable from '../components/DataTable.vue'
import { PAGE_SIZES, DEFAULT_PAGE_SIZE } from '../api/logs'
import { onEnrollResult } from '../stores/events'
import {
  AUTH_FLAGS, CARD_NO_HINT, NAME_HINT, PWD_HINT, ROLE_TEXT, ROLES, UID_HINT,
  addUser, clearUserFace, clearUserIc, deleteUser, downscaleJpeg, listUsers,
  normalizeCardNo, setUserFace, setUserIc, setUserPwd, updateUser,
  validCardNo, validName, validPwd, validUid,
} from '../api/users'
import { toast } from '../stores/toast'

const COLUMNS = [
  { key: 'uid', label: '用户 ID', width: '18%' },
  { key: 'name', label: '姓名' },
  { key: 'roleText', label: '权限', width: '10%' },
  { key: 'authText', label: '验证方式' },
  { key: 'faceText', label: '人脸', width: '9%' },
  { key: 'icText', label: 'IC 卡', width: '12%' },
  { key: 'ops', label: '操作', width: '24%' },
]

const rows = ref([])
const loading = ref(false)
const page = ref(1)
const pages = ref(1)
const total = ref(0)
const pageSize = ref(DEFAULT_PAGE_SIZE)
const pageSizeOptions = PAGE_SIZES.map((n) => ({ value: n, label: String(n) }))

/* form: null=收起;add / edit(uid) 两用。creds = 该用户已录凭据
 * (has_face/has_finger/has_ic),验证方式勾选据此禁用 */
const form = ref(null)
const busy = ref(false)
const f = reactive({ uid: '', name: '', pwd: '', role: 0, flags: [4] })

/* 人脸上传状态(编辑表单内;受理制,seq 配对 WS 回执) */
const fileInput = ref(null)
const previewUrl = ref('')
const uploading = ref(false)
let pendingSeq = null

const FLAGS_BIT_TEXT = { 1: '人脸', 2: '指纹', 4: '密码', 8: 'IC 卡' }

/* 各方式位是否可勾:编辑 = 对应凭据已录;添加 = 仅密码(密码必填恒有) */
function bitEnabled(bit) {
  if (!form.value) return false
  if (form.value.mode === 'add') return bit === 4
  const c = form.value.creds
  if (bit === 1) return !!c.has_face
  if (bit === 2) return !!c.has_finger
  if (bit === 8) return !!c.has_ic
  return true
}

function authText(row) {
  return AUTH_FLAGS.filter((a) => row.auth_flags & a.bit).map((a) => a.label).join('/') || '—'
}

function decorate(row) {
  return {
    ...row,
    roleText: ROLE_TEXT[row.role] || '?',
    authText: authText(row),
    faceText: row.has_face ? '已录入' : '无',
    icText: row.ic_mask || '无',
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
  f.flags = [4]
  previewUrl.value = ''
}

function openEdit(row) {
  form.value = {
    mode: 'edit',
    uid: row.uid,
    creds: {
      has_face: row.has_face, has_finger: row.has_finger, has_ic: row.has_ic,
      ic_mask: row.ic_mask || '',
    },
  }
  f.uid = row.uid
  f.name = row.name
  f.pwd = ''
  f.role = row.role
  /* 初始勾选 = 库值;未录入的方式直接不勾(服务端会拒,前端禁用) */
  f.flags = AUTH_FLAGS.filter((a) => (row.auth_flags & a.bit) && bitEnabled(a.bit)).map((a) => a.bit)
  previewUrl.value = ''
}

function closeForm() {
  form.value = null
  previewUrl.value = ''
  pendingSeq = null
  if (fileInput.value) fileInput.value.value = ''
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

/* ---- 人脸照片录入/重录(受理制) ---- */

async function onPickFile(ev) {
  const file = ev.target.files && ev.target.files[0]
  if (!file) return
  if (!/^image\//.test(file.type)) {
    toast.err('请选择图片文件')
    return
  }
  try {
    /* 先降采样再上传:最长边 1024,q85(实测 50~200KB,远低于服务端
     * 512KB 硬顶);canvas 重编码同时抹掉 EXIF/方向 */
    const { blob, width, height } = await downscaleJpeg(file)
    if (blob.size > 512 * 1024) {
      toast.err('图片过大,请换一张尺寸更小的照片')
      return
    }
    previewUrl.value = URL.createObjectURL(blob)
    uploading.value = true
    const res = await setUserFace({ uid: form.value.uid, blob })
    pendingSeq = res.seq ?? null
    toast.info(res.msg || '已受理,正在提取人脸特征')
  } catch (err) {
    toast.err(err.message || '上传失败')
  } finally {
    uploading.value = false
    if (fileInput.value) fileInput.value.value = ''
  }
}

/* WS enroll 回执:kind=0(人脸)且 seq 配对(或非本人上传的提示落地)→ 刷新 */
const unsubEnroll = onEnrollResult((msg) => {
  if (msg.kind !== 0) return
  if (pendingSeq !== null && msg.seq === pendingSeq) {
    pendingSeq = null
    load(page.value)
  }
})

/* ---- IC 卡绑定/解绑(同步落库;卡号直输,与设备端刷卡同一服务端实现) ---- */

async function onBindIc() {
  const raw = window.prompt(
    `为用户 ${form.value.uid} 绑定 IC 卡\n输入卡号(8~30 位十六进制,设备端录入页可读出):`,
  )
  if (raw === null) return
  const no = normalizeCardNo(raw)
  if (!no) return
  if (!validCardNo(no)) {
    toast.err(CARD_NO_HINT)
    return
  }
  try {
    const res = await setUserIc({ uid: form.value.uid, cardNo: no })
    toast.ok(res.msg || 'IC 卡已绑定')
    form.value.creds.has_ic = true
    form.value.creds.ic_mask = res.ic_mask || '********'
    load(page.value)
  } catch (err) {
    toast.err(err.message)
  }
}

async function onUnbindIc() {
  if (!window.confirm(`确认解绑 ${form.value.uid} 的 IC 卡?解绑后刷卡验证立即失效。`)) return
  try {
    await clearUserIc({ uid: form.value.uid })
    toast.ok('IC 卡已解绑')
    form.value.creds.has_ic = false
    form.value.creds.ic_mask = ''
    load(page.value)
  } catch (err) {
    toast.err(err.message)
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

onMounted(() => {
  load()
})

onUnmounted(() => {
  unsubEnroll()
})
</script>

<template>
  <div class="page-grid">

    <AppCard v-if="!form" title="用户管理" :index="0">
      <div class="bar">
        <span class="muted small">共 {{ total }} 人;人脸可编辑页上传,IC 卡编辑页直输卡号或设备端刷卡录入,指纹在设备端录入</span>
        <AppButton icon="check" size="sm" @click="openAdd">添加用户</AppButton>
      </div>
      <DataTable :columns="COLUMNS" :rows="rows" row-key="uid" :loading="loading">
        <template #cell-faceText="{ row }">
          <span :class="{ dim: !row.has_face }">{{ row.faceText }}</span>
        </template>
        <template #cell-icText="{ row }">
          <span :class="{ dim: !row.ic_mask }">{{ row.icText }}</span>
        </template>
        <template #cell-ops="{ row }">
          <span class="cell-ops">
            <AppButton size="sm" variant="ghost" @click="openEdit(row)">编辑</AppButton>
            <AppButton size="sm" variant="ghost" @click="onResetPwd(row)">改密</AppButton>
            <AppButton size="sm" variant="ghost" @click="onClearFace(row)">清人脸</AppButton>
            <AppButton size="sm" variant="warn" @click="onDelete(row)">删除</AppButton>
          </span>
        </template>
      </DataTable>
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
          <label
            v-for="a in AUTH_FLAGS"
            :key="a.bit"
            class="flags__item"
            :class="{ disabled: !bitEnabled(a.bit) }"
          >
            <input
              v-model="f.flags"
              type="checkbox"
              :value="a.bit"
              :disabled="!bitEnabled(a.bit)"
            />
            <span>{{ a.label }}<template v-if="!bitEnabled(a.bit)">(未录入)</template></span>
          </label>
        </div>
        <div v-if="form.mode === 'edit'" class="face-upload">
          <span class="small muted">人脸照片({{ form.creds.has_face ? '重录将覆盖已录人脸' : '尚未录入' }})</span>
          <div class="face-upload__row">
            <img v-if="previewUrl" :src="previewUrl" class="face-upload__preview" alt="预览" />
            <input
              ref="fileInput"
              type="file"
              accept="image/*"
              style="display: none"
              @change="onPickFile"
            />
            <AppButton
              size="sm"
              variant="ghost"
              :loading="uploading"
              @click="fileInput && fileInput.click()"
            >
              {{ form.creds.has_face ? '重录人脸' : '上传人脸' }}
            </AppButton>
          </div>
          <span class="small muted">选图后自动降采样(最长边 1024)上传;提取在设备端后台进行,完成后提示结果</span>
        </div>
        <div v-if="form.mode === 'edit'" class="ic-bind">
          <span class="small muted">
            IC 卡({{ form.creds.ic_mask ? `已绑定 ${form.creds.ic_mask}` : '尚未绑定' }})
          </span>
          <div class="face-upload__row">
            <AppButton size="sm" variant="ghost" @click="onBindIc">
              {{ form.creds.ic_mask ? '重绑 IC 卡' : '录入 IC 卡' }}
            </AppButton>
            <AppButton v-if="form.creds.ic_mask" size="sm" variant="ghost" @click="onUnbindIc">
              解绑
            </AppButton>
          </div>
          <span class="small muted">卡号直输(设备端录入页读出或卡面印刷);也可在设备端编辑页刷卡录入</span>
        </div>
      </div>
      <div class="ops">
        <AppButton icon="check" :loading="busy" @click="onSave">保存</AppButton>
        <AppButton variant="ghost" @click="closeForm">取消</AppButton>
      </div>
      <p class="muted small">新用户必须设密码;未录入的方式不能勾选(人脸可本页上传、IC 卡本页直输卡号,指纹在设备端录入)。</p>
    </AppCard>

  </div>
</template>

<style scoped>
.bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 10px;
  margin-bottom: 10px;
}
.cell-ops {
  display: inline-flex;
  gap: 6px;
  white-space: nowrap;
}
.dim {
  opacity: 0.55;
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
.flags__item.disabled {
  opacity: 0.5;
}
.face-upload {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.ic-bind {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.face-upload__row {
  display: flex;
  align-items: center;
  gap: 10px;
}
.face-upload__preview {
  width: 64px;
  height: 64px;
  object-fit: cover;
  border-radius: 8px;
  border: 1px solid var(--line, #e5e7eb);
}
.ops {
  display: flex;
  gap: 8px;
  margin-bottom: 10px;
}
.page-grid {
  display: grid;
  gap: var(--gap);
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  align-content: start;
}
</style>
