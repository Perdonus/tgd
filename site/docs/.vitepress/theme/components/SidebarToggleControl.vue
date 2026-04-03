<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'

const collapsed = ref(false)
const storageKey = 'astrogram-docs-sidebar-collapsed'

const label = computed(() => (collapsed.value ? 'Show menu' : 'Hide menu'))

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
      <span class="docs-sidebar-control__label">Menu</span>
      <button class="docs-sidebar-control__button" type="button" @click="toggleSidebar">
        <svg viewBox="0 0 20 20" aria-hidden="true">
          <path
            :d="collapsed
              ? 'M7.2 15.6a.75.75 0 0 1 0-1.06L11.74 10 7.2 5.46A.75.75 0 0 1 8.26 4.4l5.07 5.07a.75.75 0 0 1 0 1.06l-5.07 5.07a.75.75 0 0 1-1.06 0Z'
              : 'M12.8 4.4a.75.75 0 0 1 0 1.06L8.26 10l4.54 4.54a.75.75 0 1 1-1.06 1.06l-5.07-5.07a.75.75 0 0 1 0-1.06l5.07-5.07a.75.75 0 0 1 1.06 0Z'"
            fill="currentColor"
          />
        </svg>
        <span>{{ label }}</span>
      </button>
    </div>

    <button
      v-if="collapsed"
      class="docs-sidebar-reopen"
      type="button"
      aria-label="Show menu"
      @click="toggleSidebar"
    >
      <svg viewBox="0 0 20 20" aria-hidden="true">
        <path
          d="M7.2 15.6a.75.75 0 0 1 0-1.06L11.74 10 7.2 5.46A.75.75 0 0 1 8.26 4.4l5.07 5.07a.75.75 0 0 1 0 1.06l-5.07 5.07a.75.75 0 0 1-1.06 0Z"
          fill="currentColor"
        />
      </svg>
      <span>Show menu</span>
    </button>
  </div>
</template>
