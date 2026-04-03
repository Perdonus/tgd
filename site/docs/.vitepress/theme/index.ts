import type { Theme } from 'vitepress'
import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import DocsScrollSlider from './components/DocsScrollSlider.vue'
import SidebarToggleControl from './components/SidebarToggleControl.vue'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout: () =>
    h('div', { class: 'docs-theme-shell' }, [
      h(DefaultTheme.Layout, null, {
        'sidebar-nav-before': () => h(SidebarToggleControl),
      }),
      h(DocsScrollSlider),
    ]),
  enhanceApp() {},
} satisfies Theme
