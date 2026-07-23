---
title: Introduction
description: Desktop-native plugins for Astrogram with host-rendered settings, recovery and real client hooks.
---

# Introduction

Build native plugins for Astrogram Desktop with C++20, Qt and a host runtime that already knows how to render settings, recover from bad loads and expose real desktop windows or session state.

## What Astrogram plugins are

Astrogram plugins are compiled `.tgd` modules loaded directly by the desktop client.

- they use the same native toolchain as the client
- they talk to a stable plugin API instead of patching random memory
- they can register settings, commands, interceptors, metadata previews and window handlers

## What the host already gives you

The runtime is designed so plugins do not have to reinvent the same plumbing every time.

### Host-rendered settings

Settings pages are rendered by Astrogram itself, so a plugin can expose sliders, toggles, text fields and actions without building fragile standalone dialogs.

### Real desktop hooks

Plugins can work with actual Astrogram windows, active sessions, commands and interceptors instead of only static metadata.

### Recovery-aware loading

When a plugin breaks load or UI flow, Astrogram can attribute the crash, enter safe mode, disable the bad plugin and let the user recover instead of looping forever.

## Build model

To build a working plugin, it must match the client runtime:

- same platform
- same architecture
- same compiler ABI
- same Qt major and minor version
- same Astrogram plugin API version

If one of these does not match, the loader will reject the binary instead of pretending it is safe.

## Typical workflow

1. Start from `Telegram/Plugins/Examples`.
2. Include `Telegram/SourceFiles/plugins/plugins_api.h`.
3. Export preview metadata with `TGD_PLUGIN_PREVIEW(...)`.
4. Build the plugin and rename the output to `.tgd`.
5. Drop it into `tdata/plugins`.
6. Open `Settings > Astrogram > Plugins`.

## Where to go next

- [Setup](/setup) for folders, ABI expectations and install basics
- [First Plugin](/first-plugin) for the minimal plugin skeleton
- [Plugin Settings](/plugin-settings) for host-side controls
- [Runtime API](/runtime-api) for windows, sessions and helper calls
