# Packaging & Release

## Output format

Astrogram plugins are native shared libraries distributed with the `.tgd` suffix.

Typical flow:

1. build your plugin library
2. rename or emit it as `.tgd`
3. place it into the plugins directory
4. let Astrogram rescan it automatically

## Preview metadata

Use preview metadata so the client can show install information before plugin code runs:

- name
- version
- author
- description
- website
- optional icon reference

This is what powers the install/update preview in chat-driven plugin installation flows.

## Versioning recommendations

- keep a stable plugin ID forever
- bump versions on every shipped binary
- avoid in-place replacement of unrelated plugin IDs
- keep changelog notes for users when behavior changes significantly

## Separate plugin CI

Astrogram is moving toward selective plugin runners so a medium plugin change does not rebuild the whole catalog every time.

Recommended pipeline split:

- plugin-specific build workflow
- catalog sanity workflow
- client workflow kept separate from plugin packaging

## Release checklist

- build passes on the target platform
- preview metadata matches the shipped binary
- settings page works without crashes
- install / update / disable / re-enable flow works
- `plugins.log` stays clean during normal load
