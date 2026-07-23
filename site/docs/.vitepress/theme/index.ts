import type { Theme } from 'vitepress'
import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import SidebarToggleControl from './components/SidebarToggleControl.vue'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout: () =>
    h('div', { class: 'docs-theme-shell' }, [
      h(DefaultTheme.Layout, null, {
        'nav-bar-content-before': () => h(SidebarToggleControl),
      }),
    ]),
  enhanceApp() {},
} satisfies Theme
