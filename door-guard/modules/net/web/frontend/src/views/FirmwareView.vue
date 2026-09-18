<!--
  FirmwareView.vue — 固件升级说明页

  升级包由运维主机推送(udp: 校验 sha256 后落暂存分区,不阻塞门禁业务),
  页面上不做文件上传:浏览器无法在 http 下算 sha256(secure context 限制),
  与其放宽校验,不如让升级走既定脚本。这里只展示当前版本与命令。
-->
<script setup>
import { computed } from 'vue'
import AppCard from '../components/AppCard.vue'
import { device } from '../stores/device'

const version = computed(() => (device.data && device.data.version) || '—')
const port = computed(() => (device.data && device.data.web_port) || 8080)
</script>

<template>
  <AppCard title="固件升级" span2 :index="2">
    <p class="muted">
      升级包由运维主机推送:设备侧流式接收、校验 sha256 后写入暂存分区,不落内存、不阻塞门禁业务。
    </p>
    <pre class="code">dg-ota-upload &lt;设备IP&gt; &lt;升级包.tar.gz&gt;</pre>
    <div class="meta small mono">
      <span>当前版本:{{ version }}</span>
      <span>接收端口:{{ port }}(/api/ota/upload)</span>
      <span>主机脚本:env/bin/dg-ota-upload</span>
    </div>
    <p class="muted small">
      校验失败(sha256/大小不符)会被拒收;升级完成需重启设备生效(A/B 槽位方案见 docs/tech/OTA_PLAN.md)。
    </p>
  </AppCard>
</template>

<style scoped>
.code {
  margin: 8px 0;
  padding: 12px;
  overflow-x: auto;
  border-radius: var(--radius-sm);
  background: #0f172a;
  color: #b3e5fc;
  font-size: 13px;
}
.meta {
  display: flex;
  flex-wrap: wrap;
  gap: 14px;
  color: var(--muted);
}
</style>
