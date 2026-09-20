<!--
  AccountView.vue — 账号安全(改账号/口令)

  提交前只做"两次口令一致"这种纯前端能判断的检查;账号/口令的合法性由服务端
  权威校验(与设备端同一份规则),错误文案直接呈现服务端返回的 msg。
  成功后本地一律登出(服务端已吊销全部会话),强制用新凭据登录。
-->
<script setup>
import { reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import AppButton from '../components/AppButton.vue'
import AppCard from '../components/AppCard.vue'
import AppField from '../components/AppField.vue'
import { changeAccount } from '../api/auth'
import { afterCredentialChange, session } from '../stores/session'
import { toast } from '../stores/toast'

const router = useRouter()
const form = reactive({ user: session.user || '', oldPwd: '', pwd: '', confirm: '' })
const busy = ref(false)

async function onSave() {
  if (form.pwd !== form.confirm) {
    toast.err('两次输入的新口令不一致')
    return
  }
  if (!form.oldPwd) {
    toast.err('请先输入原口令')
    return
  }
  busy.value = true
  try {
    const res = await changeAccount({ user: form.user.trim(), oldPwd: form.oldPwd, pwd: form.pwd })
    toast.ok(res.msg || '已保存')
    form.oldPwd = form.pwd = form.confirm = ''
    afterCredentialChange()
    router.replace({ name: 'login' })
  } catch (err) {
    toast.err(err.message)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <AppCard title="账号安全" :index="2">
    <div class="form">
      <AppField v-model="form.user" label="账号" size="sm" hint="3~31 位,字母/数字/-/_" />
      <AppField v-model="form.oldPwd" label="原口令" type="password" size="sm" autocomplete="current-password" />
      <AppField v-model="form.pwd" label="新口令" type="password" size="sm" autocomplete="new-password" hint="4~31 位,不能含空格" />
      <AppField v-model="form.confirm" label="确认新口令" type="password" size="sm" autocomplete="new-password" />
    </div>
    <AppButton block icon="check" :loading="busy" @click="onSave">保存并重新登录</AppButton>
    <p class="muted small">账号与口令的合法性由设备端校验(与门禁用户同一份规则);修改成功后所有会话失效。</p>
  </AppCard>
</template>

<style scoped>
.form {
  display: flex;
  flex-direction: column;
  gap: 10px;
  margin-bottom: 14px;
}
</style>
