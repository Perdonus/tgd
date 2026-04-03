import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'Astrogram Docs',
  description: 'Native C++ plugin development for Astrogram Desktop.',
  cleanUrls: true,
  lastUpdated: true,
  head: [
    ['meta', { name: 'theme-color', content: '#ff6a3d' }],
    ['meta', { property: 'og:title', content: 'Astrogram Docs' }],
    ['meta', { property: 'og:description', content: 'Native C++ plugin development for Astrogram Desktop.' }]
  ],
  themeConfig: {
    logo: '/logo.png',
    siteTitle: 'Astrogram Docs',
    nav: [
      { text: 'Documentation', link: '/' },
      { text: 'Channel', link: 'https://t.me/astrogramchannel' },
      { text: 'Community Chat', link: 'https://t.me/astrogram_chat' }
    ],
    search: {
      provider: 'local'
    },
    socialLinks: [
      { icon: 'github', link: 'https://github.com/Perdonus/tgd' }
    ],
    outline: {
      level: [2, 3]
    },
    sidebar: [
      {
        text: 'Getting Started',
        items: [
          { text: 'Introduction', link: '/' },
          { text: 'Setup', link: '/setup' },
          { text: 'First Plugin', link: '/first-plugin' }
        ]
      },
      {
        text: 'Basics',
        items: [
          { text: 'Plugin Class', link: '/plugin-class' },
          { text: 'Plugin Settings', link: '/plugin-settings' },
          { text: 'Commands & Interceptors', link: '/commands-and-interceptors' }
        ]
      },
      {
        text: 'Utils',
        items: [
          { text: 'Window & Session API', link: '/window-and-session' },
          { text: 'Runtime API', link: '/runtime-api' },
          { text: 'File Utilities', link: '/file-utilities' },
          { text: 'System Info', link: '/system-info' }
        ]
      },
      {
        text: 'Other',
        items: [
          { text: 'Available Libraries', link: '/available-libraries' },
          { text: 'Common Classes', link: '/common-classes' }
        ]
      }
    ]
  }
})
