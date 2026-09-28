<!--
  NetworkCard.vue — 网络配置卡片(纯展示)

  入参 info = /api/network 快照,pending = 应用进行中;组件不认识接口与
  store,应用动作原样 emit 给视图层(分层纪律:components 不碰 api/store)。
  未拿到地址的项服务端已统一给 0.0.0.0,这里只做视觉上的降权显示。

  点号固定输入(2026-09-28,与设备端 net_set 同交互):地址框只敲数字,
  每 3 位自动插点号;完整度门禁 = 恰 12 位数字;提交前规范化去段内前导零
  (设备侧解析拒 "001" 形态)。规则实现在 utils/format.js(纯函数,可单测)。
-->
<script setup>
import { computed, reactive, watch } from 'vue'
import AppButton from './AppButton.vue'
import { ipv4Autodot, ipv4Complete, ipv4Normalize, ipv4Octets, ipv4Pad } from '../utils/format'

const props = defineProps({
  info: { type: Object, default: null },
  pending: { type: Boolean, default: false },
})
const emit = defineEmits(['apply', 'refresh'])

const form = reactive({ mode: 'dhcp', ip: '', netmask: '', gateway: '' })
const touched = reactive({ ip: false, netmask: false, gateway: false })
let edited = false                /* 用户动过表单后,轮询回填不得再覆盖输入 */

/* 有快照且用户还没动过表单时,用设备当前值回填(首次进页/切设备)。
 * 回填走补零形态(与设备端弹窗预填一致,否则 12 位门禁会拦住"没动过
 * 就应用");"0.0.0.0" = 未配置/未取址,视作空 */
watch(
  () => props.info,
  (n) => {
    if (!n || edited) return
    form.mode = n.mode === 'static' ? 'static' : 'dhcp'
    const conf = n.configured || {}
    const pick = (v) => (v && v !== '0.0.0.0' ? v : '')
    form.ip = ipv4Pad(pick(conf.ip) || (n.have_ip ? pick(n.ip) : ''))
    form.netmask = ipv4Pad(pick(conf.netmask) || (n.have_ip ? pick(n.netmask) : ''))
    form.gateway = ipv4Pad(pick(conf.gateway) || (n.have_ip ? pick(n.gateway) : ''))
  },
  { immediate: true },
)

/* 输入即格式化:抽数字按 3 位一组重插点号(用户敲的点号也会被剥掉重排) */
function onAddrInput(key) {
  edited = true
  form[key] = ipv4Autodot(form[key])
}

const isStatic = computed(() => form.mode === 'static')



function validIp(v) {
  const a = ipv4Octets(v)
  if (!a) return false
  if (a[0] === 0 || a[0] === 127 || (a[0] === 169 && a[1] === 254)) return false
  return !(a[0] === 0 && a[1] === 0 && a[2] === 0 && a[3] === 0)
}
function validMask(v) {
  const a = ipv4Octets(v)
  if (!a) return false
  /* 连续掩码:n 加最低置位位回绕到 2^32(与设备端 mask_continuous 同款)。
   * JS 位运算是 int32——旧写法 (n|(n+1))>>>0===0xffffffff 除 /31 外全拒
   * (255.255.255.0 也拒,潜伏 bug,2026-09-28 修复) */
  const n = ((a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]) >>> 0
  if (n === 0) return false
  return n + (n & -n) === 0x100000000
}

const errors = computed(() => {
  const e = {}
  if (!isStatic.value) return e
  const hint = '请输满 12 位数字,点号自动补全'
  if (touched.ip && !ipv4Complete(form.ip)) e.ip = hint
  else if (touched.ip && !validIp(form.ip)) e.ip = 'IP 不合法(不能是 0/环回/链路本地)'
  if (touched.netmask && !ipv4Complete(form.netmask)) e.netmask = hint
  else if (touched.netmask && !validMask(form.netmask)) e.netmask = '掩码不合法(须为连续掩码)'
  if (touched.gateway && form.gateway !== '' && !ipv4Complete(form.gateway)) e.gateway = hint
  else if (touched.gateway && form.gateway !== '' && !ipv4Octets(form.gateway))
    e.gateway = '网关格式不合法(可留空 = 不设网关)'
  return e
})

const canApply = computed(() => {
  if (!isStatic.value) return true
  return (
    ipv4Complete(form.ip) &&
    validIp(form.ip) &&
    ipv4Complete(form.netmask) &&
    validMask(form.netmask) &&
    (form.gateway === '' || (ipv4Complete(form.gateway) && ipv4Octets(form.gateway)))
  )
})

function markTouched() {
  edited = true
  touched.ip = touched.netmask = touched.gateway = true
}

/* 提交载荷:规范化去段内前导零("192.168.001.010"→"192.168.1.10") */
function applyPayload() {
  return {
    mode: form.mode,
    ip: ipv4Normalize(form.ip),
    netmask: ipv4Normalize(form.netmask),
    gateway: form.gateway === '' ? '' : ipv4Normalize(form.gateway),
  }
}

function zero(v) {
  return v === '0.0.0.0'
}

defineExpose({ markTouched, form, canApply })
</script>

<template>
  <div>
    <p class="muted small" v-if="!info">加载中…</p>
    <template v-else>
      <div class="rows">
        <div class="row"><span class="k">接口</span><span class="mono">{{ info.ifname || '—' }}</span></div>
        <div class="row">
          <span class="k">IP 地址</span>
          <span class="mono" :class="{ zero: !info.have_ip }">{{ info.ip }}</span>
        </div>
        <div class="row">
          <span class="k">子网掩码</span>
          <span class="mono" :class="{ zero: !info.have_ip }">{{ info.netmask }}</span>
        </div>
        <div class="row">
          <span class="k">默认网关</span>
          <span class="mono" :class="{ zero: !info.have_ip }">{{ info.gateway }}</span>
        </div>
        <div class="row">
          <span class="k">状态</span>
          <span :class="info.online ? 'ok' : 'bad'">{{ info.online ? '已联网' : '未联网' }}</span>
        </div>
      </div>

      <div class="mode">
        <label :class="{ active: form.mode === 'dhcp' }">
          <input type="radio" value="dhcp" v-model="form.mode" /> DHCP 自动获取
        </label>
        <label :class="{ active: form.mode === 'static' }">
          <input type="radio" value="static" v-model="form.mode" /> 静态地址
        </label>
      </div>

      <div v-if="isStatic" class="fields">
        <label>
          <span>IP 地址</span>
          <input v-model.trim="form.ip" @input="onAddrInput('ip')" @blur="touched.ip = true"
            placeholder="只输数字,如 192168001050" inputmode="numeric"
            :class="{ err: errors.ip }" />
          <em v-if="errors.ip">{{ errors.ip }}</em>
        </label>
        <label>
          <span>子网掩码</span>
          <input v-model.trim="form.netmask" @input="onAddrInput('netmask')"
            @blur="touched.netmask = true" placeholder="只输数字,如 255255255000" inputmode="numeric"
            :class="{ err: errors.netmask }" />
          <em v-if="errors.netmask">{{ errors.netmask }}</em>
        </label>
        <label>
          <span>默认网关</span>
          <input v-model.trim="form.gateway" @input="onAddrInput('gateway')"
            @blur="touched.gateway = true" placeholder="只输数字,可留空" inputmode="numeric"
            :class="{ err: errors.gateway }" />
          <em v-if="errors.gateway">{{ errors.gateway }}</em>
        </label>
      </div>

      <p class="muted small">
        地址只输数字,点号自动补全(12 位 = 四段);应用静态地址后,请用<b>新地址</b>重新访问本页;配置会持久化,重启后自动恢复。
      </p>
      <div class="actions">
        <AppButton :disabled="!canApply" :loading="pending" @click="markTouched(); emit('apply', applyPayload())">
          应用配置
        </AppButton>
        <AppButton variant="ghost" size="sm" icon="refresh" @click="emit('refresh')">刷新</AppButton>
      </div>
    </template>
  </div>
</template>

<style scoped>
.rows {
  display: grid;
  gap: 6px;
  margin-bottom: 14px;
}
.row {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  font-size: 14px;
}
.k {
  color: var(--muted, #5b7186);
}
.mono {
  font-variant-numeric: tabular-nums;
}
.zero {
  opacity: 0.45;
}
.ok {
  color: var(--ok, #1a7f37);
}
.bad {
  color: var(--bad, #c62828);
}
.mode {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
}
.mode label {
  flex: 1;
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 8px 10px;
  border: 1px solid var(--line, #e8f1fb);
  border-radius: 8px;
  cursor: pointer;
  font-size: 14px;
}
.mode label.active {
  border-color: var(--primary, #1976d2);
  background: rgba(25, 118, 210, 0.06);
}
.fields {
  display: grid;
  gap: 10px;
  margin-bottom: 10px;
}
.fields label {
  display: grid;
  gap: 4px;
  font-size: 13px;
}
.fields input {
  padding: 8px 10px;
  border: 1px solid var(--line, #d7e3ee);
  border-radius: 8px;
  font-family: inherit;
  font-variant-numeric: tabular-nums;
}
.fields input.err {
  border-color: var(--bad, #c62828);
}
.fields em {
  color: var(--bad, #c62828);
  font-style: normal;
  font-size: 12px;
}
.actions {
  display: flex;
  gap: 10px;
  align-items: center;
}
</style>
