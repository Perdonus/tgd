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

## Blur Telegram

A visual effect plugin focused on blur and frosted layers for the desktop client.

## Accent Color

A theming-oriented plugin for Astrogram-specific accent colors and palette tweaks.

## Font Tuner

Typography-oriented plugin work. The long-term goal is not just scaling text, but also loading custom fonts from local files or remote sources.

## AyuSafe

`AyuSafe` started as a plugin home for Ayu-style privacy and utility ideas. The larger targets are gradually being moved into the Astrogram client itself where deeper functionality belongs.

## Why this page matters

These plugins are useful reference material for:

- settings page design
- runtime metadata
- safe mode and recovery testing
- window/session APIs
- selective plugin CI workflows
