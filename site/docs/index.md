---
layout: home

hero:
  name: Astrogram Plugins
  text: Native C++ plugins for Astrogram Desktop
  tagline: A simple way to extend Astrogram with `.tgd` plugins, Qt, host-rendered settings, command hooks and desktop-native runtime APIs.
  actions:
    - theme: brand
      text: Get Started
      link: /setup
    - theme: alt
      text: First Plugin
      link: /first-plugin

features:
  - title: Native runtime
    details: Compile native `.tgd` libraries and use Qt plus host APIs without a scripting bridge.
  - title: Stable settings
    details: Register host-rendered settings pages instead of building fragile plugin-owned dialogs.
  - title: Hooks and windows
    details: Intercept commands, observe messages, and work with real Astrogram windows and sessions.
---

## What Astrogram plugins are

Astrogram plugins are native `.tgd` libraries that run inside the Astrogram Desktop process. They are powerful, but they are not sandboxed. A bad ABI mismatch or a native crash inside a plugin can crash the client too.

The plugin manager tracks load failures, writes detailed logs, and can automatically switch the client into safe mode if a plugin crashes during a tracked operation.

## Quick facts

- Plugin API version: `5`
- Language: `C++20`
- UI/toolkit: `Qt`
- Binary format: shared library with `.tgd` suffix
- Primary header: `Telegram/SourceFiles/plugins/plugins_api.h`

## Where to look next

- Start with [Setup](/setup) to understand ABI requirements and directory layout.
- Read [Astrogram Features](/astrogram-features) for the split between built-in client features and plugin-side features.
- Jump to [First Plugin](/first-plugin) if you want a minimal working example.
- Open [Plugin Settings](/plugin-settings) for the host-rendered settings system.
- Use [Commands & Interceptors](/commands-and-interceptors) for `/command` handling and outgoing text hooks.
- Browse [Built-in Plugins](/built-in-plugins) to see which reference plugins ship with the tree.
- Check [Troubleshooting](/troubleshooting) if a plugin installs but does not load or forces safe mode.
- Read [Safe Mode & Recovery](/safe-mode) for the crash-handling model and the recovery files Astrogram writes.
