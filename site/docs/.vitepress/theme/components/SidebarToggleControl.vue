<script setup lang="ts">
import { onMounted, ref } from 'vue'

const collapsed = ref(false)
const storageKey = 'astrogram-docs-sidebar-collapsed'

const applyCollapsed = (value: boolean) => {
  collapsed.value = value
  document.documentElement.classList.toggle('docs-sidebar-collapsed', value)
}

const toggleSidebar = () => {
  const next = !collapsed.value
  applyCollapsed(next)
  localStorage.setItem(storageKey, next ? '1' : '0')
}

onMounted(() => {
  applyCollapsed(localStorage.getItem(storageKey) === '1')
})
</script>

<template>
  <div class="docs-sidebar-control">
    <span class="docs-sidebar-control__label">Navigation</span>
    <button class="docs-sidebar-control__button" type="button" @click="toggleSidebar">
      <span>{{ collapsed ? 'Expand menu' : 'Collapse menu' }}</span>
    </button>
  </div>
</template>
