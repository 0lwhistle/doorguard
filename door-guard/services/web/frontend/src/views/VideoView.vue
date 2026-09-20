<!--
  VideoView.vue — 监控画面(占位)

  预览链路(RTSP/MJPEG)要与 capture_service 对接后才有画面;这里给出明确的
  等待态而不是"空白页",避免运维以为页面坏了。
-->
<script setup>
import AppCard from '../components/AppCard.vue'
</script>

<template>
  <AppCard title="监控画面" span2 :index="2">
    <div class="video">
      <div class="video__scan" aria-hidden="true"></div>
      <div class="video__msg">摄像头实时画面接入中…</div>
      <p class="video__hint small">
        预览链路(RTSP 拉流或 HTTP-MJPEG)与取流服务对接后,此处直接显示画面,不额外占用摄像头。
      </p>
    </div>
  </AppCard>
</template>

<style scoped>
.video {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 6px;
  height: 260px;
  padding: 16px;
  overflow: hidden;
  border-radius: 12px;
  text-align: center;
  color: #cfe8ff;
  background: linear-gradient(135deg, #0f2027, #203a43, #2c5364);
}
.video__scan {
  position: absolute;
  left: 0;
  right: 0;
  height: 40px;
  background: linear-gradient(180deg, transparent, rgba(66, 165, 245, 0.35), transparent);
  animation: scan 3.4s linear infinite;
}
.video__msg {
  z-index: 1;
  font-size: 15px;
  font-weight: 600;
}
.video__hint {
  z-index: 1;
  max-width: 520px;
  color: #9fc7e8;
}
</style>
