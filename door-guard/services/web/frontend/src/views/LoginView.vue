<!--
  LoginView.vue — 登录页

  行为与旧版一致(回车提交、错误抖动、默认凭据提示),换成组件化实现:
  表单字段用 AppField,提示走 toast,本页只管"提交 → 成功去首页/失败留原地"。
-->
<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import AppButton from '../components/AppButton.vue'
import AppField from '../components/AppField.vue'
import AppIcon from '../components/AppIcon.vue'
import { doLogin, isLoggedIn, session } from '../stores/session'

const router = useRouter()
const user = ref('admin')
const pwd = ref('')
const shake = ref(false)

async function onSubmit() {
  const ok = await doLogin(user.value.trim(), pwd.value)
  if (ok) {
    pwd.value = ''
    await router.replace({ name: 'dashboard' })
    return
  }
  shake.value = false
  /* 强制重排,让同一个抖动动画能连续触发 */
  requestAnimationFrame(() => {
    shake.value = true
  })
}

if (isLoggedIn.value) router.replace({ name: 'dashboard' })
</script>

<template>
  <div class="login">
    <form class="login__card anim-rise" :class="{ 'anim-shake': shake }" @submit.prevent="onSubmit">
      <div class="login__logo" aria-hidden="true">
        <AppIcon name="door" :size="52" />
      </div>
      <h1>door-guard</h1>
      <p class="muted small center">人脸识别门禁 · 上位机</p>

      <AppField v-model="user" label="账号" autocomplete="username" />
      <AppField v-model="pwd" label="密码" type="password" autocomplete="current-password" @enter="onSubmit" />

      <p class="login__err" role="alert">{{ session.error }}</p>
      <AppButton block :loading="session.busy" type="submit">登录</AppButton>
      <p class="muted small center">
        设备默认账号 admin / admin,首次登录后请立即在「账号安全」中修改。
      </p>
    </form>
  </div>
</template>

<style scoped>
.login {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  background: radial-gradient(1200px 600px at 50% -10%, #dcebfa 0%, var(--bg) 60%);
}
.login__card {
  display: flex;
  flex-direction: column;
  gap: 12px;
  width: 100%;
  max-width: 360px;
  padding: 28px 26px;
  border-radius: 18px;
  background: var(--card);
  box-shadow: var(--shadow-float);
}
.login__logo {
  display: flex;
  justify-content: center;
  color: var(--primary);
  filter: drop-shadow(0 6px 12px rgba(30, 136, 229, 0.32));
}
.login__card h1 {
  margin: 0;
  text-align: center;
  font-size: 24px;
  letter-spacing: 0.5px;
  color: var(--primary-dark);
}
.login__err {
  min-height: 18px;
  margin: 0;
  font-size: 13px;
  color: var(--err);
}
</style>
