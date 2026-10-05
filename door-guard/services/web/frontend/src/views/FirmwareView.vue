<!--
  FirmwareView.vue — 固件升级(2026-10-05 重做:.ota 全量包三段式)

  流程:选包(前端结构预检:magic/版本/大小一致性)→「上传与校验」
  (XHR 进度条;断网/断电后可续传,板端收完做载荷摘要复核)→ 槽位就绪 →
  「升级重启」(提取校验 → 交系统安装器 → 设备重启 → 轮询确认新版本)。
  槽内包可删除或重新上传覆盖;升级是重大动作,触发前二次确认。
-->
<script setup>
import { computed, onMounted, onUnmounted, reactive } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import { device, fmtBytes } from '../stores/device'
import { ApiError } from '../api/client'
import { applyFw, deleteFw, getFwSlot, parseOtaHeader, uploadFw } from '../api/ota_fw'

/* ---- 槽位状态(轮询刷新) ---- */
const slot = reactive({
  loaded: false,
  present: false,
  version: '',
  date: '',
  size: 0,
  receivedAt: 0,
  staged: false,          // 已提取交装(等待重启)
  applying: false,
  uploading: false,
  partialActive: false,
  partialReceived: 0,
})
let pollTimer = null

/* ---- 上传状态机 ---- */
const up = reactive({
  file: null,
  meta: null,             // 前端预检结果 {version,date,payloadSize}
  preErr: '',
  phase: 'idle',          // idle | uploading | checking | done | error
  pct: 0,
  err: '',
  offset: 0,              // 续传起点
})
let abortCtl = null

/* ---- 升级状态机 ---- */
const go = reactive({
  phase: 'idle',          // idle | confirming | extracting | rebooting | done | fail
  err: '',
  newVersion: '',
})
const curVersion = computed(() => (device.data && device.data.version) || '—')
const busy = computed(() => up.phase === 'uploading' || up.phase === 'checking' ||
                            (go.phase !== 'idle' && go.phase !== 'fail'))

async function refreshSlot() {
  try {
    const d = await getFwSlot()
    slot.loaded = true
    slot.present = !!d.present
    slot.version = d.version || ''
    slot.date = d.date || ''
    slot.size = d.size || 0
    slot.receivedAt = d.received_at || 0
    slot.staged = !!d.staged
    slot.applying = !!d.applying
    slot.uploading = !!d.uploading
    slot.partialActive = !!(d.partial && d.partial.active)
    slot.partialReceived = (d.partial && d.partial.received) || 0
    /* 上传中途刷新过页面:按服务器实收字节恢复续传起点 */
    if (slot.partialActive && up.phase === 'idle' && up.file) {
      up.offset = slot.partialReceived
    }
  } catch { /* 401 由全局处理;其余静默(下轮再取) */ }
}

/* ---- 选包:前端结构预检(上板前第一道检查) ---- */
async function onPickFile(e) {
  up.file = e.target.files && e.target.files[0]
  up.meta = null
  up.preErr = ''
  up.err = ''
  up.phase = 'idle'
  if (!up.file) return
  try {
    up.meta = await parseOtaHeader(up.file)
  } catch (err) {
    up.preErr = err.message
  }
}

function clearFile() {
  up.file = null
  up.meta = null
  up.preErr = ''
  up.err = ''
  up.phase = 'idle'
  up.offset = 0
  const input = document.getElementById('fw-file')
  if (input) input.value = ''
}

/* ---- 上传与校验 ---- */
async function startUpload() {
  if (!up.file || !up.meta || up.phase === 'uploading' || up.phase === 'checking') return
  up.phase = 'uploading'
  up.err = ''
  abortCtl = new AbortController()
  try {
    const resp = await uploadFw(up.file, {
      offset: up.offset,
      onProgress: (pct) => { up.pct = pct },
      signal: abortCtl.signal,
    })
    up.phase = 'done'
    up.pct = 100
    up.offset = 0
    slot.present = true
    slot.version = resp.version || up.meta.version
    slot.date = resp.date || up.meta.date
    slot.size = resp.size || up.meta.payloadSize
    clearFile()
  } catch (err) {
    if (err instanceof ApiError && err.payload && err.payload.aborted) {
      up.phase = 'idle'                       /* 用户主动取消 */
    } else if (err instanceof ApiError && err.status === 409 &&
               /偏移/.test(err.message)) {
      up.offset = 0
      up.phase = 'error'
      up.err = '续传偏移与设备不符(设备侧已重置),请重新上传'
    } else {
      up.phase = 'error'
      up.err = err.message || '上传失败'
      /* 网络中断:保留文件与已传进度,再次点「上传与校验」自动续传 */
    }
  }
  refreshSlot()
}

function cancelUpload() {
  if (abortCtl) abortCtl.abort()
}

/* ---- 升级重启 ---- */
function askApply() {
  if (go.phase === 'idle' || go.phase === 'fail') go.phase = 'confirming'
}
function cancelApply() {
  if (go.phase === 'confirming') go.phase = 'idle'
}

let rebootPoll = null
async function doApply() {
  go.phase = 'extracting'
  go.err = ''
  try {
    await applyFw()
  } catch (err) {
    go.phase = 'fail'
    go.err = err.message || '触发失败'
    return
  }
  /* 轮询:提取(applying)→ 交装(staged)→ 设备失联(重启中)→
   * 回来且版本变化(完成);6 分钟兜底超时 */
  const startVer = curVersion.value
  const t0 = Date.now()
  rebootPoll = setInterval(async () => {
    try {
      const d = await getFwSlot()
      slot.staged = !!d.staged
      slot.applying = !!d.applying
      if (d.applying) return                       /* 还在提取 */
      if (go.phase === 'extracting') {
        if (!d.staged) {
          go.phase = 'fail'
          go.err = '提取校验未通过(详见设备日志);槽内包保留,可重试或删除'
          clearInterval(rebootPoll)
          rebootPoll = null
          return
        }
        go.phase = 'rebooting'
        go.newVersion = slot.version
        return
      }
    } catch { /* 设备重启中:请求失败是预期 */ }
    if (go.phase !== 'rebooting') return
    try {
      const v = curVersion.value
      if (v && v !== '—' && v !== startVer) {
        go.phase = 'done'
        clearInterval(rebootPoll)
        rebootPoll = null
        refreshSlot()
      } else if (Date.now() - t0 > 6 * 60 * 1000) {
        go.phase = 'fail'
        go.err = '等待设备重启超时,请确认设备状态后刷新本页'
        clearInterval(rebootPoll)
        rebootPoll = null
      }
    } catch { /* 仍失联 */ }
  }, 2000)
}

async function doDelete() {
  try {
    await deleteFw()
    slot.present = false
    slot.version = ''
    if (go.phase === 'done') go.phase = 'idle'
  } catch (err) {
    go.err = err.message || '删除失败'
  }
}

onMounted(() => {
  refreshSlot()
  pollTimer = setInterval(refreshSlot, 3000)
})
onUnmounted(() => {
  if (pollTimer) clearInterval(pollTimer)
  if (rebootPoll) clearInterval(rebootPoll)
  if (abortCtl) abortCtl.abort()
})

function fmtTime(ts) {
  if (!ts) return '—'
  const d = new Date(ts * 1000)
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
         `${p(d.getHours())}:${p(d.getMinutes())}`
}
</script>

<template>
  <div class="page-grid">
    <!-- 卡 1:当前状态 -->
    <AppCard title="固件信息" :index="0">
      <div class="rows">
        <div class="row"><span class="k">当前固件</span><span class="v mono">{{ curVersion }}</span></div>
        <div class="row">
          <span class="k">升级包槽</span>
          <span v-if="!slot.loaded" class="v muted">加载中…</span>
          <span v-else-if="!slot.present" class="v muted">空(未上传升级包)</span>
          <span v-else class="v mono">
            {{ slot.version }}
            <span v-if="slot.staged" class="tag">已交装</span>
          </span>
        </div>
        <div v-if="slot.present" class="row sub">
          <span class="k">包信息</span>
          <span class="v small muted">
            发布 {{ slot.date || '—' }} · 载荷 {{ fmtBytes(slot.size) }} ·
            入槽 {{ fmtTime(slot.receivedAt) }}
          </span>
        </div>
      </div>
    </AppCard>

    <!-- 卡 2:上传与校验 -->
    <AppCard title="上传与校验" :index="1">
      <p class="muted small">
        仅接受全量编译生成的 .ota 升级包(打包:env/bin/dg-ota-pack)。
        上传前本页先做结构预检,设备收完再做摘要复核,两道检查都过才入槽。
      </p>
      <div class="pick">
        <input id="fw-file" type="file" accept=".ota" :disabled="busy" @change="onPickFile" />
        <span v-if="up.file" class="small">{{ up.file.name }}({{ fmtBytes(up.file.size) }})</span>
        <AppButton v-if="up.file" variant="ghost" size="sm" :disabled="busy" @click="clearFile">重选</AppButton>
      </div>
      <p v-if="up.preErr" class="err small">预检失败:{{ up.preErr }}</p>
      <p v-else-if="up.meta" class="small">
        包内版本 <span class="mono">{{ up.meta.version }}</span>
        ({{ up.meta.date || '无日期' }} · 载荷 {{ fmtBytes(up.meta.payloadSize) }})
      </p>

      <!-- 上传进度条 -->
      <div v-if="up.phase === 'uploading' || up.phase === 'done'" class="bar">
        <div class="bar__fill" :class="{ 'bar__fill--ok': up.phase === 'done' }"
             :style="{ width: up.pct + '%' }"></div>
        <span class="bar__label small mono">{{ up.pct }}%</span>
      </div>
      <p v-if="up.phase === 'uploading'" class="small muted">
        上传中…(中断后再次上传会从已传位置续传)
      </p>
      <p v-if="up.phase === 'checking'" class="small muted">设备端校验中…</p>
      <p v-if="up.phase === 'done'" class="ok small">校验通过,已入槽。确认无误后可在「升级重启」中安装。</p>
      <p v-if="up.phase === 'error'" class="err small">{{ up.err }}</p>
      <p v-if="up.offset > 0 && up.phase === 'idle'" class="small muted">
        检测到上次已传 {{ fmtBytes(up.offset) }},本次将从断点续传。
      </p>

      <div class="acts">
        <AppButton variant="primary" :disabled="!up.file || !!up.preErr || busy"
                   @click="startUpload">
          {{ up.offset > 0 ? '续传' : '上传与校验' }}
        </AppButton>
        <AppButton v-if="up.phase === 'uploading'" variant="ghost" @click="cancelUpload">取消</AppButton>
      </div>
    </AppCard>

    <!-- 卡 3:升级重启 -->
    <AppCard title="升级重启" :index="2">
      <template v-if="!slot.loaded"><p class="muted small">加载中…</p></template>
      <template v-else-if="!slot.present">
        <p class="muted small">槽内没有升级包。请先在「上传与校验」中上传 .ota 包。</p>
      </template>
      <template v-else>
        <p class="small">
          将把槽内包 <span class="mono">{{ slot.version }}</span> 写入备用槽并重启设备,
          全程约 1~2 分钟,期间门禁业务中断;升级失败设备会自动回滚到当前版本。
        </p>
        <p v-if="slot.staged && go.phase === 'idle'" class="ok small">
          该包已交装,设备即将升级重启,请勿断电。
        </p>
        <p v-if="go.phase === 'fail'" class="err small">{{ go.err }}</p>
        <p v-if="go.phase === 'done'" class="ok small">
          升级完成,当前固件 <span class="mono">{{ curVersion }}</span>。
        </p>

        <!-- 升级阶段进度条(提取/重启为不确定时长,用流动条) -->
        <div v-if="go.phase === 'extracting' || go.phase === 'rebooting'" class="bar">
          <div class="bar__fill bar__fill--flow"></div>
        </div>
        <p v-if="go.phase === 'extracting'" class="small muted">提取校验中(载荷摘要复核)…</p>
        <p v-if="go.phase === 'rebooting'" class="small muted">
          已交系统安装器,设备升级重启中…请勿断电(完成后本页自动确认新版本)。
        </p>

        <div v-if="go.phase === 'confirming'" class="confirm">
          <p class="small">
            确认升级到 <span class="mono">{{ slot.version }}</span> 并立即重启设备?
          </p>
          <div class="acts">
            <AppButton variant="warn" @click="doApply">确认升级重启</AppButton>
            <AppButton variant="ghost" @click="cancelApply">再想想</AppButton>
          </div>
        </div>
        <div v-else class="acts">
          <AppButton variant="primary" :disabled="busy || slot.staged || go.phase === 'done'"
                     @click="askApply">升级重启</AppButton>
          <AppButton variant="ghost" :disabled="busy && go.phase !== 'fail'"
                     @click="doDelete">删除包</AppButton>
        </div>
      </template>
    </AppCard>
  </div>
</template>

<style scoped>
.page-grid {
  display: grid;
  gap: var(--gap);
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  align-content: start;
}
.rows { display: grid; gap: 8px; }
.row { display: flex; align-items: baseline; gap: 10px; }
.row.sub { margin-top: -4px; }
.k { flex: 0 0 5em; color: var(--muted); font-size: 13px; }
.v { word-break: break-all; }
.pick { display: flex; align-items: center; gap: 10px; margin: 10px 0; flex-wrap: wrap; }
.acts { display: flex; gap: 10px; margin-top: 12px; flex-wrap: wrap; }
.confirm {
  margin-top: 12px;
  padding: 10px 12px;
  border: 1px solid var(--warn, #e65100);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--warn, #e65100) 6%, transparent);
}
.err { color: var(--err); }
.ok { color: var(--ok); }
.tag {
  display: inline-block;
  margin-left: 6px;
  padding: 1px 7px;
  border-radius: var(--radius-pill);
  font-size: 11px;
  color: #fff;
  background: var(--ok);
}
.bar {
  position: relative;
  height: 18px;
  margin: 10px 0 4px;
  border-radius: 999px;
  background: var(--line);
  overflow: hidden;
}
.bar__fill {
  height: 100%;
  border-radius: 999px;
  background: var(--primary);
  transition: width 0.2s ease;
}
.bar__fill--ok { background: var(--ok); }
.bar__label {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 11px;
  color: #fff;
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.4);
}
.bar__fill--flow {
  width: 34%;
  animation: fw-flow 1.2s ease-in-out infinite;
  transition: none;
}
@keyframes fw-flow {
  0% { transform: translateX(-110%); }
  100% { transform: translateX(330%); }
}
</style>
