<!--
  AppShell.vue — 登录后的外壳(侧栏导航 + 顶栏 + 内容区)

  只做布局与导航,不取业务数据:设备快照/实时连接状态都来自 stores,
  这样换布局不影响任何视图。
-->
<script setup>
import { computed, onMounted, onUnmounted } from 'vue'
import { RouterLink, RouterView, useRouter } from 'vue-router'
import AppIcon from '../components/AppIcon.vue'
import AppButton from '../components/AppButton.vue'
import { device, refresh, startPolling, stopPolling } from '../stores/device'
import { events, isLive, start as startEvents, stop as stopEvents } from '../stores/events'
import { doLogout, session } from '../stores/session'
import { connText } from '../utils/format'

const router = useRouter()

const NAV = [
  { to: '/', name: 'dashboard', label: '设备概览', icon: 'gauge' },
  { to: '/logs', name: 'logs', label: '记录查询', icon: 'list' },
  { to: '/account', name: 'account', label: '账号安全', icon: 'key' },
  { to: '/firmware', name: 'firmware', label: '固件升级', icon: 'upload' },
  { to: '/video', name: 'video', label: '监控画面', icon: 'video' },
]

const version = computed(() => (device.data ? device.data.version : '—'))

async function onLogout() {
  stopEvents()
  stopPolling()
  await doLogout()
  router.replace({ name: 'login' })
}

onMounted(() => {
  refresh().catch((e) => {
    /* 401 已由 client 处理回登录页;其它错误给出可读提示 */
    if (e.status !== 401) session.error = e.message
  })
  startEvents()
  startPolling()
})

onUnmounted(() => {
  stopEvents()
  stopPolling()
})
</script>

<template>
  <div class="shell">
    <aside class="side">
      <div class="side__brand">
        <span class="side__mark" aria-hidden="true"></span>
        <span>door-guard</span>
      </div>
      <nav class="side__nav">
        <RouterLink
          v-for="item in NAV"
          :key="item.name"
          class="nav"
          active-class="nav--on"
          :to="item.to"
        >
          <AppIcon :name="item.icon" :size="17" />
          <span>{{ item.label }}</span>
        </RouterLink>
      </nav>
      <p class="side__foot muted small">固件 {{ version }}</p>
    </aside>

    <div class="main">
      <header class="topbar">
        <span class="brand">门禁上位机</span>
        <div class="topbar__right">
          <span class="conn" :class="isLive ? 'conn--on' : 'conn--off'">
            <i class="pulse" aria-hidden="true"></i>
            <span class="small">{{ connText(events.status) }}</span>
          </span>
          <span class="who small">{{ session.user ? `管理员 ${session.user}` : '' }}</span>
          <AppButton variant="ghost" size="sm" icon="logout" @click="onLogout">退出</AppButton>
        </div>
      </header>

      <div v-if="session.pwdDefault" class="banner">
        <span class="banner__ico" aria-hidden="true">!</span>
        <span>当前仍是出厂默认口令,局域网内任何人都能登录。请立即修改账号与口令。</span>
        <RouterLink class="banner__link" :to="{ name: 'account' }">去修改</RouterLink>
      </div>

      <main class="content">
        <RouterView v-slot="{ Component }">
          <transition name="view" mode="out-in">
            <component :is="Component" />
          </transition>
        </RouterView>
      </main>
    </div>
  </div>
</template>

<style scoped>
.shell {
  display: flex;
  min-height: 100vh;
}
/* ---- 侧栏 ---- */
.side {
  display: flex;
  flex-direction: column;
  width: var(--sidebar-w);
  flex: 0 0 var(--sidebar-w);
  padding: 18px 14px;
  background: #fff;
  border-right: 1px solid var(--line);
}
.side__brand {
  display: flex;
  align-items: center;
  gap: 9px;
  font-weight: 700;
  font-size: 16px;
  color: var(--primary-dark);
  padding: 4px 8px 18px;
}
.side__mark {
  width: 14px;
  height: 14px;
  border-radius: 4px;
  background: linear-gradient(135deg, var(--primary), var(--primary-dark));
}
.side__nav {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.nav {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 12px;
  border-radius: var(--radius-sm);
  color: var(--text);
  text-decoration: none;
  font-size: 14px;
  transition:
    background 0.18s,
    color 0.18s,
    transform 0.18s;
}
.nav:hover {
  background: var(--primary-light);
  transform: translateX(2px);
}
.nav--on {
  background: linear-gradient(90deg, var(--primary), var(--primary-dark));
  color: #fff;
  box-shadow: var(--shadow);
}
.side__foot {
  margin-top: auto;
  padding: 8px;
}
/* ---- 主区 ---- */
.main {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}
.topbar {
  position: sticky;
  top: 0;
  z-index: 20;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 12px 18px;
  color: #fff;
  background: linear-gradient(115deg, #1565c0, #1e88e5 45%, #42a5f5);
  background-size: 200% 100%;
  animation: flow 14s ease-in-out infinite;
  box-shadow: 0 2px 12px rgba(21, 101, 192, 0.25);
}
.brand {
  font-weight: 700;
}
.topbar__right {
  display: flex;
  align-items: center;
  gap: 12px;
}
.conn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 10px;
  border-radius: var(--radius-pill);
  background: rgba(255, 255, 255, 0.16);
}
.pulse {
  position: relative;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #9e9e9e;
}
.conn--on .pulse {
  background: #7cfc9a;
}
.conn--on .pulse::after {
  content: '';
  position: absolute;
  inset: -4px;
  border-radius: 50%;
  border: 2px solid #7cfc9a;
  animation: ring 1.6s ease-out infinite;
}
.conn--off .pulse {
  background: #ffab91;
}
.who {
  opacity: 0.9;
}
/* ---- 默认口令横幅 ---- */
.banner {
  display: flex;
  align-items: center;
  gap: 12px;
  margin: 14px auto 0;
  width: 100%;
  max-width: var(--content-max);
  padding: 12px 16px;
  border: 1px solid var(--warn-line);
  border-radius: 12px;
  background: var(--warn-bg);
  color: var(--warn-text);
  font-size: 13px;
  animation: rise 0.4s both;
}
.banner__ico {
  display: grid;
  place-items: center;
  flex: 0 0 auto;
  width: 22px;
  height: 22px;
  border-radius: 50%;
  background: var(--warn);
  color: #fff;
  font-weight: 700;
}
.banner__link {
  margin-left: auto;
  font-weight: 600;
}
.content {
  width: 100%;
  max-width: var(--content-max);
  margin: 0 auto;
  padding: var(--gap);
  display: grid;
  gap: var(--gap);
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  align-content: start;
}
/* 窄屏:侧栏变顶部横向导航(手机上单手可用) */
@media (max-width: 860px) {
  .shell {
    flex-direction: column;
  }
  .side {
    width: 100%;
    flex: none;
    flex-direction: row;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
    border-right: 0;
    border-bottom: 1px solid var(--line);
    overflow-x: auto;
  }
  .side__brand,
  .side__foot {
    display: none;
  }
  .side__nav {
    flex-direction: row;
    gap: 6px;
  }
  .nav {
    white-space: nowrap;
    padding: 8px 12px;
  }
  .nav:hover {
    transform: none;
  }
}
</style>
