---
layout: home

hero:
  name: Astrogram Docs
  text: Native plugins and desktop runtime notes
  tagline: Build `.tgd` plugins with Qt, host-rendered settings, command hooks, observers, window/session callbacks, and Astrogram-specific client integration.
  image:
    src: /logo.png
    alt: Astrogram
  actions:
    - theme: brand
      text: Get Started
      link: /setup
    - theme: alt
      text: First Plugin
      link: /first-plugin
    - theme: alt
      text: Changelog
      link: /changelog

features:
  - title: Native runtime
    details: Plugins are compiled shared libraries loaded directly into Astrogram Desktop. You get Qt, native widgets, and host APIs without a scripting bridge.
  - title: Stable settings UI
    details: Register settings pages and let the host render sliders, toggles, text inputs, and action buttons inside Settings > Plugins.
  - title: Built-in plugin references
    details: Study AstroTransparent, AI Chat, Blur, Accent Color and other in-tree plugins as real examples of the runtime.
  - title: Command and message hooks
    details: Intercept slash commands, outgoing text, and message events. Build utility plugins without patching the whole client.
  - title: Window and session access
    details: React to real Telegram windows and sessions through the host API instead of guessing top-level widgets blindly.
  - title: Public release entry points
    details: Astrogram uses docs.astrogram.su for docs, runtime notes, troubleshooting, and changelog links used directly by the client.
---

## What Astrogram plugins are

Astrogram plugins are native `.tgd` libraries that run inside the Astrogram Desktop process. They are powerful, but they are not sandboxed. A bad ABI mismatch or a native crash inside a plugin can crash the client too.

The plugin manager tracks load failures, writes detailed logs, and can automatically switch the client into safe mode if a plugin crashes during a tracked operation.

## Current API target

- Plugin API version: `5`
- Language: `C++20`
- UI/toolkit: `Qt`
- Binary format: shared library with `.tgd` suffix
- Primary header: `Telegram/SourceFiles/plugins/plugins_api.h`

## Where to look next

- Start with [Setup](/setup) to understand ABI requirements and directory layout.
- Jump to [First Plugin](/first-plugin) if you want a minimal working example.
- Open [Plugin Settings](/plugin-settings) for the host-rendered settings system.
- Use [Commands & Interceptors](/commands-and-interceptors) for `/command` handling and outgoing text hooks.
- Browse [Built-in Plugins](/built-in-plugins) to see which reference plugins ship with the tree.
- Check [Troubleshooting](/troubleshooting) if a plugin installs but does not load or forces safe mode.
- Read [Safe Mode & Recovery](/safe-mode) for the crash-handling model and the recovery files Astrogram writes.
