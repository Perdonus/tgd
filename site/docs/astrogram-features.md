# Astrogram Features

Astrogram is not only a plugin host. Part of the project lives directly inside the desktop client.

## Settings home

Astrogram-specific client functionality is intended to live under the Astrogram settings area instead of being scattered through unrelated Telegram settings pages.

Current direction:

- Astrogram entry point in settings
- plugin entry point
- links and support entry points
- privacy and experimental desktop-native features

## Client-side features vs plugin-side features

Some things belong in plugins:

- optional visual experiments
- network-backed helpers
- user-installable utilities
- rapidly changing prototypes

Some things belong in the client:

- anti-recall behavior inside the message list
- ghost mode and deeper privacy hooks
- local premium UI overrides
- settings shells and global branded entry points

## Why this split matters

If a feature needs:

- MTProto/session-level interception
- message rendering changes
- global navigation changes
- guaranteed startup behavior

it is usually a client feature, not a plugin feature.

## Current built-in focus areas

- branded Astrogram settings home
- plugin manager UX
- safe mode and recovery handling
- anti-recall data retention and tooling
- deeper privacy toggles

## Relationship to donor projects

Astrogram can borrow ideas from AyuGram and other Telegram forks, but desktop-native integration still needs to fit the Astrogram client architecture and plugin runtime.

That is why some ideas are shipped as built-in features while others remain plugins or examples first.
