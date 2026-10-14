<!--
  VideoView.vue — 监控画面(2026-10-14 实装)

  链路:WS 订阅(video_on)→ 服务端 10fps 推二进制 JPEG → Blob objectURL 刷
  <img>。挂载订阅/卸载退订由 stores/events 的 videoDesired 意图管理(重连自动
  重发);相机未就绪或推流被配置关闭时,画面区保持等待态/给出来自设备的文案。
  「截图」直接保存当前帧(canvas 不参与,objectURL 即 JPEG 原始字节)。
-->
<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import { onVideoControl, onVideoFrame, setVideoSubscribe } from '../stores/events'

const frameUrl = ref('')
const waiting = ref(true) // 首帧前/断流中
const errMsg = ref('')
let lastUrl = null
let offFrame = null
let offCtl = null
let stallTimer = null

function feed(blob) {
  waiting.value = false
  errMsg.value = ''
  const url = URL.createObjectURL(blob)
  if (lastUrl) URL.revokeObjectURL(lastUrl)
  lastUrl = url
  frameUrl.value = url
  /* 断流看门狗:2s 无新帧回等待态(相机未就绪/编码线程停拍都收敛到这里) */
  clearTimeout(stallTimer)
  stallTimer = setTimeout(() => {
    waiting.value = true
  }, 2000)
}

function snapshot() {
  if (!frameUrl.value) return
  const a = document.createElement('a')
  a.href = frameUrl.value
  a.download = `doorguard-${new Date().toISOString().replace(/[:.]/g, '-')}.jpg`
  a.click()
}

onMounted(() => {
  offFrame = onVideoFrame(feed)
  offCtl = onVideoControl((m) => {
    if (!m.ok) {
      errMsg.value = m.msg || '监控不可用'
      waiting.value = false
    }
  })
  setVideoSubscribe(true)
})

onBeforeUnmount(() => {
  setVideoSubscribe(false)
  if (offFrame) offFrame()
  if (offCtl) offCtl()
  clearTimeout(stallTimer)
  if (lastUrl) URL.revokeObjectURL(lastUrl)
})
</script>

<template>
  <AppCard title="监控画面" span2 :index="2">
    <div class="video">
      <img v-if="frameUrl" class="video__img" :src="frameUrl" alt="门口实时画面" />
      <div v-if="waiting && !errMsg" class="video__msg">
        <span class="video__dot" aria-hidden="true"></span>
        等待画面…
      </div>
      <div v-if="errMsg" class="video__msg video__err">{{ errMsg }}</div>
    </div>
    <div class="video__bar">
      <span class="small">实时画面由设备以约 10 帧/秒推送,仅当前浏览器标签接收。</span>
      <AppButton :disabled="!frameUrl" @click="snapshot">截图</AppButton>
    </div>
  </AppCard>
</template>

<style scoped>
.video {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 260px;
  overflow: hidden;
  border-radius: 12px;
  background: #0b1622;
}
.video__img {
  max-width: 100%;
  max-height: 480px;
  display: block;
}
.video__msg {
  position: absolute;
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  border-radius: 999px;
  font-size: 14px;
  font-weight: 600;
  color: #cfe8ff;
  background: rgba(11, 22, 34, 0.72);
}
.video__err {
  color: #ffd7d7;
}
.video__dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #64b5f6;
  animation: pulse 1.2s ease-in-out infinite;
}
.video__bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-top: 8px;
  color: var(--dg-text-dim, #6b7a8c);
}
@keyframes pulse {
  0%,
  100% {
    opacity: 0.35;
  }
  50% {
    opacity: 1;
  }
}
</style>
