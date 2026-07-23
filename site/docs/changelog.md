# Changelog

Astrogram Desktop uses this page as the public changelog entry point.

## Release flow

- Windows desktop builds are produced by GitHub Actions from the `main` branch.
- Public announcements land in the Astrogram channel: <https://t.me/astrogramchannel>
- Plugin-specific releases are published separately from client builds.

## What to expect here

This page is intentionally lightweight for now:

- latest public build references
- major client/runtime changes
- plugin API milestones
- important migration notes for plugin authors

## Current focus areas

- Astrogram branding across the client and build artifacts
- plugin runtime stability and host-rendered plugin settings
- plugin manager UX and safe mode recovery
- broader desktop-native feature parity work

## Verify the latest build

For the newest public build and announcement history, use:

- Channel: <https://t.me/astrogramchannel>
- Community chat: <https://t.me/astrogram_chat>
- Repository: <https://github.com/Perdonus/tgd>

## Notes for plugin developers

If your plugin suddenly stops loading after a client update, first check:

1. plugin API version compatibility
2. Qt / compiler ABI compatibility
3. `tdata/plugins.log`
4. whether the client entered safe mode after a crash
