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
  <div class="docs-sidebar-control-wrap">
    <div class="docs-sidebar-control">
      <button
        class="docs-sidebar-control__button"
        type="button"
        :aria-label="collapsed ? 'Open sidebar' : 'Collapse sidebar'"
        :title="collapsed ? 'Open sidebar' : 'Collapse sidebar'"
        @click="toggleSidebar"
      >
        <svg viewBox="0 0 20 20" aria-hidden="true">
          <rect x="2.5" y="3" width="15" height="14" rx="2.5" fill="none" stroke="currentColor" stroke-width="1.5" />
          <path d="M7 4v12" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" />
          <path
            :d="collapsed
              ? 'M10.65 13.95a.7.7 0 0 1 0-.99L13.62 10l-2.97-2.96a.7.7 0 1 1 .99-.99l3.46 3.45a.7.7 0 0 1 0 .99l-3.46 3.45a.7.7 0 0 1-.99 0Z'
              : 'M14.35 6.05a.7.7 0 0 1 0 .99L11.38 10l2.97 2.96a.7.7 0 0 1-.99.99L9.9 10.5a.7.7 0 0 1 0-.99l3.46-3.45a.7.7 0 0 1 .99 0Z'"
            fill="currentColor"
          />
        </svg>
        <span class="docs-visually-hidden">
          {{ collapsed ? 'Open sidebar' : 'Collapse sidebar' }}
        </span>
      </button>
    </div>

    <button
      v-if="collapsed"
      class="docs-sidebar-reopen"
      type="button"
      aria-label="Open sidebar"
      title="Open sidebar"
      @click="toggleSidebar"
    >
      <svg viewBox="0 0 20 20" aria-hidden="true">
        <rect x="2.5" y="3" width="15" height="14" rx="2.5" fill="none" stroke="currentColor" stroke-width="1.5" />
        <path d="M7 4v12" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" />
        <path
          d="M10.65 13.95a.7.7 0 0 1 0-.99L13.62 10l-2.97-2.96a.7.7 0 1 1 .99-.99l3.46 3.45a.7.7 0 0 1 0 .99l-3.46 3.45a.7.7 0 0 1-.99 0Z"
          fill="currentColor"
        />
      </svg>
      <span class="docs-visually-hidden">Open sidebar</span>
    </button>
  </div>
</template>
