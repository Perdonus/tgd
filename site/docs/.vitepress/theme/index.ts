import type { Theme } from 'vitepress'
import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import HomeSliderShowcase from './components/HomeSliderShowcase.vue'
import SidebarToggleControl from './components/SidebarToggleControl.vue'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout: () =>
    h(DefaultTheme.Layout, null, {
      'sidebar-nav-before': () => h(SidebarToggleControl),
    }),
  enhanceApp({ app }) {
    app.component('HomeSliderShowcase', HomeSliderShowcase)
  },
} satisfies Theme
