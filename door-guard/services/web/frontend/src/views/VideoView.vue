<!--
  VideoView.vue — 监控画面(2026-10-14 实装;用户拍板:点开始才推,平时不跑)

  链路:点「开始」→ WS 订阅(video_on)→ 服务端 10fps 推二进制 JPEG → Blob
  objectURL 刷 <img>;点「停止」退订并清帧。订阅意图(videoDesired)由
  stores/events 管理,连接重连后自动重发——按钮状态即意图状态。设备端配置
  关闭推流时,订阅回执 ok=false,按钮弹回并显示设备给的说法。
-->
<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import { onVideoControl, onVideoFrame, setVideoSubscribe } from '../stores/events'

const started = ref(false) // 是否已点开始(= 订阅意图)
const waiting = ref(false) // 已开始但没帧(首帧前/断流中)
const errMsg = ref('')
const frameUrl = ref('')
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
  /* 断流看门狗:2s 无新帧回等待态(相机未就绪/编码停拍都收敛到这里) */
  clearTimeout(stallTimer)
  stallTimer = setTimeout(() => {
    waiting.value = true
  }, 2000)
}

function start() {
  started.value = true
  waiting.value = true
  errMsg.value = ''
  setVideoSubscribe(true)
}

function stop() {
  started.value = false
  waiting.value = false
  errMsg.value = ''
  clearTimeout(stallTimer)
  setVideoSubscribe(false)
  if (lastUrl) {
    URL.revokeObjectURL(lastUrl)
    lastUrl = null
  }
  frameUrl.value = ''
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
      /* 设备侧拒绝(推流被配置关闭/订阅超限):按钮弹回开始态 */
      started.value = false
      waiting.value = false
      errMsg.value = m.msg || '监控不可用'
    }
  })
})

onBeforeUnmount(() => {
  setVideoSubscribe(false) // 离开页面必退订(即使从未开始也只是空操作)
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
      <div v-if="!started && !errMsg" class="video__msg">监控未开启,点「开始监控」查看实时画面</div>
      <div v-if="started && waiting && !errMsg" class="video__msg">
        <span class="video__dot" aria-hidden="true"></span>
        等待画面…
      </div>
      <div v-if="errMsg" class="video__msg video__err">{{ errMsg }}</div>
    </div>
    <div class="video__bar">
      <span class="small">
        {{ started ? '实时画面由设备以约 10 帧/秒推送;停止后设备侧完全停拍。' : '平时不占用摄像头:只有点开始后设备才取帧推送。' }}
      </span>
      <div class="video__actions">
        <AppButton v-if="frameUrl" variant="ghost" @click="snapshot">截图</AppButton>
        <AppButton v-if="!started" @click="start">开始监控</AppButton>
        <AppButton v-else variant="warn" @click="stop">停止</AppButton>
      </div>
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
.video__actions {
  display: flex;
  gap: 8px;
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
