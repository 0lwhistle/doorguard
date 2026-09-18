<!--
  App.vue — 根组件

  只做两件事:挂提示容器、给路由一个挂载点。业务全在 views/stores 里,
  保证根组件在换布局/加页面时不用动。
-->
<script setup>
import { RouterView } from 'vue-router'
import ToastHost from './components/ToastHost.vue'
import { toasts } from './stores/toast'
</script>

<template>
  <RouterView />
  <ToastHost :items="toasts.items" />
</template>
