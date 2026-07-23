<script setup lang="ts">
import { useRoute } from 'vitepress'
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'

const route = useRoute()
const collapsed = ref(false)
const storageKey = 'astrogram-docs-sidebar-collapsed'
let observer: MutationObserver | null = null
const showToggle = computed(() => route.path !== '/')

const icons = new Map<string, string>([
  ['/Introduction', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M16 16h6"/><path d="M19 13v6"/><path d="M21 10V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l2-1.14"/><path d="m7.5 4.27 9 5.15"/><polyline points="3.29 7 12 12 20.71 7"/><line x1="12" x2="12" y1="22" y2="12"/></svg>'],
  ['/', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M16 16h6"/><path d="M19 13v6"/><path d="M21 10V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l2-1.14"/><path d="m7.5 4.27 9 5.15"/><polyline points="3.29 7 12 12 20.71 7"/><line x1="12" x2="12" y1="22" y2="12"/></svg>'],
  ['/astrogram-features', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m12 3-1.9 3.86L6 8.76l2.95 2.87-.7 4.08L12 13.9l3.75 1.81-.7-4.08L18 8.76l-4.1-1.9z"/><path d="M19 3v4"/><path d="M21 5h-4"/><path d="M5 16v3"/><path d="M6.5 17.5h-3"/></svg>'],
  ['/setup', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/><path d="M8 12h8"/><path d="M10 11v2"/><path d="M8 17h8"/><path d="M14 16v2"/></svg>'],
  ['/packaging-and-release', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z"/><path d="m3.3 7 8.7 5 8.7-5"/><path d="M12 22V12"/></svg>'],
  ['/first-plugin', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 22v-5"/><path d="M9 8V2"/><path d="M15 8V2"/><path d="M18 8a3 3 0 1 1-6 0"/><path d="M12 13a4 4 0 0 1 4 4v1H8v-1a4 4 0 0 1 4-4Z"/><path d="M6 22v-5"/></svg>'],
  ['/plugin-class', '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="8" height="8" rx="2"/><rect x="13" y="4" width="8" height="8" rx="2"/><rect x="8" y="14" width="8" height="8" rx="2"/><path d="M12 12v2"/></svg>'],
  ['/built-in-plugins', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 2v6"/><path d="M9 5h6"/><path d="M5 12h14"/><path d="M7 12v7a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2v-7"/><path d="M9 12V9a3 3 0 0 1 6 0v3"/></svg>'],
  ['/plugin-settings', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 21v-7"/><path d="M4 10V3"/><path d="M12 21v-9"/><path d="M12 8V3"/><path d="M20 21v-5"/><path d="M20 12V3"/><path d="M2 14h4"/><path d="M10 8h4"/><path d="M18 16h4"/></svg>'],
  ['/window-and-session', '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="14" rx="2"/><path d="M8 4v14"/><path d="M3 9h18"/><path d="M8 20h8"/></svg>'],
  ['/runtime-api', '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="4" y="4" width="16" height="16" rx="3"/><path d="M9 9h6"/><path d="M9 12h6"/><path d="M9 15h3"/></svg>'],
  ['/commands-and-interceptors', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m8 9 3 3-3 3"/><path d="M13 15h3"/><rect x="3" y="4" width="18" height="16" rx="2"/></svg>'],
  ['/troubleshooting', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m10.29 3.86-7 12.14A2 2 0 0 0 5 19h14a2 2 0 0 0 1.71-3l-7-12.14a2 2 0 0 0-3.42 0Z"/><path d="M12 9v4"/><path d="M12 17h.01"/></svg>'],
  ['/safe-mode', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 2 4 5v6c0 5 3.44 9.53 8 11 4.56-1.47 8-6 8-11V5Z"/><path d="M9.5 12.5 11 14l3.5-4"/></svg>'],
  ['/file-utilities', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z"/><path d="M14 2v6h6"/><path d="M8 13h8"/><path d="M8 17h5"/></svg>'],
  ['/system-info', '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="4" y="4" width="16" height="12" rx="2"/><path d="M8 20h8"/><path d="M12 16v4"/><path d="M9 8h6"/><path d="M9 11h4"/></svg>'],
  ['/links', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M10 13a5 5 0 0 0 7.54.54l3-3A5 5 0 0 0 13.47 3.47l-1.71 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3A5 5 0 0 0 10.53 20.53l1.71-1.71"/></svg>'],
  ['/changelog', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 8v4l3 3"/><circle cx="12" cy="12" r="9"/></svg>'],
  ['/available-libraries', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2Z"/></svg>'],
  ['/common-classes', '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 10h10"/><path d="M7 14h7"/><rect x="4" y="3" width="16" height="18" rx="2"/></svg>'],
])

const normalizePath = (href: string) => {
  try {
    const url = new URL(href, window.location.origin)
    return url.pathname.replace(/\/$/, '') || '/'
  } catch {
    return '/'
  }
}

const iconForPath = (path: string) => {
  return icons.get(path)
    ?? '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="3"/><path d="M12 2v3"/><path d="M12 19v3"/><path d="M4.93 4.93l2.12 2.12"/><path d="M16.95 16.95l2.12 2.12"/><path d="M2 12h3"/><path d="M19 12h3"/><path d="m4.93 19.07 2.12-2.12"/><path d="m16.95 7.05 2.12-2.12"/></svg>'
}

const decorateSidebarLinks = () => {
  document.querySelectorAll<HTMLAnchorElement>('.VPSidebar .item > .link').forEach((link) => {
    const text = link.querySelector<HTMLElement>('.text')
    if (!text) {
      return
    }
    let icon = text.querySelector<HTMLElement>('.docs-sidebar-link-icon')
    if (!icon) {
      icon = document.createElement('span')
      icon.className = 'docs-sidebar-link-icon'
      text.prepend(icon)
    }
    icon.innerHTML = iconForPath(normalizePath(link.getAttribute('href') || link.href))
  })
}

const refreshSidebar = () => {
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      decorateSidebarLinks()
    })
  })
}

const applyCollapsed = (value: boolean) => {
  collapsed.value = value
  document.documentElement.classList.toggle('docs-sidebar-collapsed', value)
}

const toggleSidebar = (event?: MouseEvent) => {
  event?.preventDefault()
  event?.stopPropagation()
  const next = !collapsed.value
  applyCollapsed(next)
  localStorage.setItem(storageKey, next ? '1' : '0')
}

watch(() => route.path, async () => {
  await nextTick()
  refreshSidebar()
})

onMounted(() => {
  applyCollapsed(localStorage.getItem(storageKey) === '1')
  refreshSidebar()
  observer = new MutationObserver(() => {
    refreshSidebar()
  })
  observer.observe(document.body, { childList: true, subtree: true })
})

onBeforeUnmount(() => {
  observer?.disconnect()
})
</script>

<template>
  <button
    v-if="showToggle"
    :class="['docs-nav-sidebar-toggle', { 'is-collapsed': collapsed }]"
    type="button"
    :aria-label="collapsed ? 'Open sidebar' : 'Collapse sidebar'"
    :title="collapsed ? 'Open sidebar' : 'Collapse sidebar'"
    @click="toggleSidebar"
  >
    <svg viewBox="0 0 24 24" aria-hidden="true">
      <rect width="18" height="18" x="3" y="3" rx="2" />
      <path d="M9 3v18" />
    </svg>
    <span class="docs-visually-hidden">
      {{ collapsed ? 'Open sidebar' : 'Collapse sidebar' }}
    </span>
  </button>
</template>
