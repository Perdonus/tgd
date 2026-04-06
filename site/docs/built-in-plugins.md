# Built-in Plugins

Astrogram keeps example plugins and real plugin packages side by side so new API features can be tested quickly without rebuilding the whole desktop client every time.

## AstroTransparent

Transparency tooling for Astrogram windows. The current direction is separate control over:

- interface opacity
- message/media opacity
- text opacity

This makes it a good reference for:

- host-rendered sliders
- live settings updates
- window callback registration

## AI Chat

`AI Chat` is the reference plugin for command interception and lightweight network-backed actions.

Target behavior:

- intercept `/ai` without sending the command into the current chat
- open the plugin-owned AI entry point instead
- use a user-provided API key
- point the user to `sosiskibot.ru` to obtain the key

## Font Tuner

Typography-oriented plugin work. The long-term goal is not just scaling text, but also loading custom fonts from local files or remote sources.

## Show Logs

`Show Logs` is the diagnostics-oriented plugin reference. It is intended to surface plugin and runtime traces inside Astrogram itself without forcing the user to hunt for files on disk.

## Why this page matters

These plugins are useful reference material for:

- settings page design
- runtime metadata
- safe mode and recovery testing
- window/session APIs
- selective plugin CI workflows
