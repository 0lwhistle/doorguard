<!--
  DashboardView.vue — 概览页:设备指标 + 实时事件 + 时间同步

  三个区块各自独立取数/渲染(指标来自 device store,事件来自 events store,
  时钟是本页局部状态),互不阻塞:某一块数据没回来不影响其它块显示。
-->
<script setup>
import { computed, ref } from 'vue'
import AppCard from '../components/AppCard.vue'
import AppButton from '../components/AppButton.vue'
import EventFeed from '../components/EventFeed.vue'
import NetworkCard from '../components/NetworkCard.vue'
import StatGrid from '../components/StatGrid.vue'
import { useClock } from '../composables/useClock'
import { triggerNtp } from '../api/ntp'
import { rebootSystem } from '../api/system'
import { setNetwork } from '../api/network'
import { address, device, refreshNetwork, refreshQuiet, storageText } from '../stores/device'
import { clearFeed, events } from '../stores/events'
import { toast } from '../stores/toast'

const clock = useClock()
const ntpBusy = ref(false)
const netBusy = ref(false)
const rebootBusy = ref(false)

const statItems = computed(() => {
  const d = device.data || {}
  return [
    { key: 'version', label: '固件版本', value: d.version || '—' },
    { key: 'uptime', label: '运行时长', value: d.uptime_text || '—' },
    { key: 'users', label: '在库用户', value: Number(d.users || 0) },
    { key: 'logs', label: '门禁记录', value: Number(d.log_total || 0) },
    { key: 'addr', label: '局域网地址', value: address.value, mono: true, wide: true },
    { key: 'disk', label: '存储', value: storageText.value, mono: true },
  ]
})

const online = computed(() => !!(device.data && device.data.online))
const ntpText = computed(() => {
  const n = device.data && device.data.ntp
  if (!online.value) return '设备未联网,时间校正不可用'
  if (n && n.ok && n.last_ok_at) return `上次校正成功:${n.last_ok_at}`
  return '尚未校正'
})

async function onNtp() {
  if (ntpBusy.value) return
  ntpBusy.value = true
  try {
    await triggerNtp()
    toast.info('已发起时间校正,结果稍后提示')
  } catch (err) {
    toast.err(err.message)
  } finally {
    /* 服务端异步执行,按钮短暂锁定后放开(结果经 WebSocket 回来) */
    setTimeout(() => {
      ntpBusy.value = false
    }, 1200)
  }
}

/** 应用网络配置:服务端 202 只代表受理,新地址经 WebSocket/重取快照到位 */
async function onApplyNetwork(cfg) {
  if (netBusy.value) return
  netBusy.value = true
  try {
    await setNetwork(cfg)
    toast.info('已受理:配置保存并开始应用' +
      (cfg.mode === 'static' ? ';若地址变化请用新地址重新访问' : ''))
    refreshNetwork()
  } catch (err) {
    toast.err(err.message)
  } finally {
    setTimeout(() => {
      netBusy.value = false
    }, 1500)
  }
}
/** 远程重启:确认后受理;设备约 1s 后断连,重启完成前页面不可用属预期 */
async function onReboot() {
  if (rebootBusy.value) return
  if (!window.confirm('确认重启设备?重启期间门禁与上位机将短暂不可用。')) return
  rebootBusy.value = true
  try {
    await rebootSystem()
    toast.info('已受理:设备正在重启,稍后请刷新页面重新访问')
  } catch (err) {
    toast.err(err.message)
    rebootBusy.value = false
  }
}

</script>

<template>
  <AppCard title="设备概览" span2 :index="0">
    <template #actions>
      <AppButton variant="ghost" size="sm" icon="refresh" @click="refreshQuiet">刷新</AppButton>
      <AppButton
        variant="warn"
        size="sm"
        icon="warn"
        :loading="rebootBusy"
        @click="onReboot"
      >
        重启设备
      </AppButton>
    </template>
    <StatGrid :items="statItems" />
  </AppCard>

  <AppCard title="网络配置" :index="1">
    <NetworkCard
      :info="device.network"
      :pending="netBusy"
      @apply="onApplyNetwork"
      @refresh="refreshNetwork"
    />
  </AppCard>

  <AppCard title="实时门禁事件" :index="2" :badge="events.status === 'open' ? 'LIVE' : ''">
    <EventFeed :items="events.items" />
    <AppButton variant="ghost" size="sm" icon="close" @click="clearFeed">清空</AppButton>
  </AppCard>

  <AppCard title="时间同步" :index="3">
    <p class="muted small">{{ ntpText }}</p>
    <p class="clock">{{ clock }}</p>
    <AppButton
      block
      :icon="online ? 'clock' : 'warn'"
      :disabled="!online"
      :loading="ntpBusy"
      @click="onNtp"
    >
      立即校正时间
    </AppButton>
    <p class="muted small">设备需已联网;校正结果会在此处与实时事件中提示。</p>
  </AppCard>
</template>

<style scoped>
.clock {
  margin: 6px 0 14px;
  font-size: 34px;
  font-weight: 700;
  letter-spacing: 2px;
  color: var(--primary-dark);
  font-variant-numeric: tabular-nums;
}
</style>
