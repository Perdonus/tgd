import { defineConfig } from 'vitepress'

const base = (
	process.env.BASE_URL
	&& process.env.BASE_URL !== '/'
	&& process.env.BASE_URL !== ''
) ? process.env.BASE_URL : '/'

export default defineConfig({
  base,
  title: 'Astrogram Docs',
  description: 'Native C++ plugin development for Astrogram Desktop.',
  cleanUrls: true,
  lastUpdated: true,
  head: [
    ['meta', { name: 'theme-color', content: '#ff6a3d' }],
    ['meta', { property: 'og:title', content: 'Astrogram Docs' }],
    ['meta', { property: 'og:description', content: 'Native C++ plugin development for Astrogram Desktop.' }],
    ['meta', { property: 'og:image', content: 'https://docs.astrogram.su/astrogram-avatar.jpg' }],
    ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
    ['link', { rel: 'icon', type: 'image/jpeg', href: '/astrogram-avatar.jpg' }]
  ],
  themeConfig: {
    logo: '/astrogram-avatar.jpg',
    siteTitle: 'Astrogram Docs',
    nav: [],
    socialLinks: [
      {
        icon: {
          svg: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M21.5 4.5 2.9 11.7c-1.3.5-1.2 1.2-.2 1.5l4.8 1.5 1.8 5.8c.2.7.1 1 1 .6l2.7-2.6 5.5 4.1c1 .6 1.8.3 2.1-1L23.9 6c.4-1.6-.6-2.3-2.4-1.5ZM8.2 14.3l11.1-7c.5-.3 1-.1.6.3l-9.2 8.3-.4 4.3-2.1-5.9Z"/></svg>',
        },
        link: 'https://t.me/astrogramchannel',
        ariaLabel: 'telegram',
      },
      { icon: 'github', link: 'https://github.com/Perdonus/tgd' }
    ],
    outline: {
      level: [2, 3]
    },
    outlineTitle: 'On this page',
    sidebarMenuLabel: 'Menu',
    returnToTopLabel: 'Back to top',
    footer: {
      message: 'Astrogram Desktop plugin documentation and runtime notes.',
      copyright: 'Copyright © 2026 Astrogram'
    },
    sidebar: [
      {
        text: 'Getting Started',
        items: [
          { text: 'Introduction', link: '/Introduction' },
          { text: 'Astrogram Features', link: '/astrogram-features' },
          { text: 'Setup', link: '/setup' },
          { text: 'First Plugin', link: '/first-plugin' },
          { text: 'Troubleshooting', link: '/troubleshooting' }
        ]
      },
      {
        text: 'Basics',
        items: [
          { text: 'Plugin Class', link: '/plugin-class' },
          { text: 'Plugin Settings', link: '/plugin-settings' },
          { text: 'Commands & Interceptors', link: '/commands-and-interceptors' },
          { text: 'Built-in Plugins', link: '/built-in-plugins' },
          { text: 'Astrogram Features', link: '/astrogram-features' }
        ]
      },
      {
        text: 'Utils',
        items: [
          { text: 'Window & Session API', link: '/window-and-session' },
          { text: 'Runtime API', link: '/runtime-api' },
          { text: 'File Utilities', link: '/file-utilities' },
          { text: 'System Info', link: '/system-info' },
          { text: 'Safe Mode & Recovery', link: '/safe-mode' }
        ]
      },
      {
        text: 'Other',
        items: [
          { text: 'Packaging & Release', link: '/packaging-and-release' },
          { text: 'Changelog', link: '/changelog' },
          { text: 'Links', link: '/links' },
          { text: 'Available Libraries', link: '/available-libraries' },
          { text: 'Common Classes', link: '/common-classes' }
        ]
      }
    ]
  }
})
