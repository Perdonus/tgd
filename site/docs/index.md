---
layout: home

hero:
  name: Astrogram Plugins
  text: Desktop-native plugins for Astrogram
  tagline: An easy way to extend Astrogram with `.tgd` packages, C++, Qt, host-rendered settings, command hooks and a real desktop runtime.
  actions:
    - theme: brand
      text: Get Started
      link: /setup
    - theme: alt
      text: Runtime API
      link: /runtime-api

features:
  - title: Native runtime
    details: Build native `.tgd` libraries and work directly with Qt plus Astrogram host APIs.
  - title: Stable settings
    details: Register host-rendered settings pages instead of shipping fragile plugin-owned windows.
  - title: Hooks and windows
    details: Intercept commands, observe messages, and work with real windows and sessions.
---

<div class="home-pill-strip">
  <span>C++20</span>
  <span>Qt</span>
  <span>API v5</span>
  <span>Host UI</span>
  <span>Safe mode</span>
</div>

<HomeSliderShowcase />

## Native desktop plugins

Astrogram plugins are native `.tgd` libraries that run inside the Astrogram Desktop process. They are fast, powerful, and desktop-native, but they are not sandboxed. A bad ABI mismatch or a native crash can still take the client down.

That is why the runtime keeps detailed logs, tracks crash-prone operations, exposes host-rendered settings, and can automatically switch Astrogram into safe mode when a plugin breaks load or UI flow.

## Start here

<div class="home-minimal-grid">
  <div class="home-minimal-card">
    <p class="home-minimal-card__index">01</p>
    <h3>Setup</h3>
    <p>Toolchain, ABI rules, build layout, plugin folder structure, package suffix and release basics.</p>
    <a href="/setup">Open setup</a>
  </div>
  <div class="home-minimal-card">
    <p class="home-minimal-card__index">02</p>
    <h3>First plugin</h3>
    <p>Minimal working plugin, exported entry points, `manifest.json`, and the expected package layout.</p>
    <a href="/first-plugin">Open example</a>
  </div>
  <div class="home-minimal-card">
    <p class="home-minimal-card__index">03</p>
    <h3>Plugin settings</h3>
    <p>Host-rendered sliders, toggles, actions and text inputs without custom dialog ownership inside the plugin.</p>
    <a href="/plugin-settings">Open settings docs</a>
  </div>
  <div class="home-minimal-card">
    <p class="home-minimal-card__index">04</p>
    <h3>Runtime API</h3>
    <p>Window access, session context, command interceptors, message hooks and runtime metadata that stay visible by default.</p>
    <a href="/runtime-api">Open runtime API</a>
  </div>
</div>

## Why this runtime holds up

<div class="home-note">
  <p><strong>No scripting bridge.</strong> Plugins compile into native code and talk to the client through the real runtime API.</p>
  <p><strong>No hidden runtime docs.</strong> Runtime surfaces stay documented and visible by default.</p>
  <p><strong>No plugin-owned settings maze.</strong> The client renders settings pages, safe mode state, diagnostics and package metadata.</p>
</div>

## Quick facts

- Plugin API version: `5`
- Language: `C++20`
- UI toolkit: `Qt`
- Package format: shared library with `.tgd` suffix
- Primary header: `Telegram/SourceFiles/plugins/plugins_api.h`
