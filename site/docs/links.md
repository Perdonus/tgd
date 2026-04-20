# Links

## Official Astrogram links

- Main channel: <https://t.me/astrogramchannel>
- Community chat: <https://t.me/astrogram_chat>
- Documentation: <https://docs.astrogram.su>
- Repository: <https://github.com/Perdonus/tgd>

## Plugin development entry points

- Setup: [/setup](/setup)
- First Plugin: [/first-plugin](/first-plugin)
- Plugin Settings: [/plugin-settings](/plugin-settings)
- Runtime API: [/runtime-api](/runtime-api)

## Support data to collect before reporting a plugin issue

- client build / commit
- plugin version
- `tdata/plugins.log`
- `tdata/plugins.trace.jsonl`
- whether safe mode turned on after the crash

## Distribution notes

- Astrogram client builds and plugin releases may move independently
- `.tgd` plugins should be versioned and shipped with preview metadata
- do not assume a plugin built for one runtime revision will load forever
