# Available Libraries

Astrogram plugins are native C++ modules. There is no embedded Python or JavaScript runtime in the desktop client.

## Standard stack

You can depend on:

- C++20
- QtCore
- QtGui
- QtWidgets
- QtNetwork

depending on what your plugin does.

## Examples in the tree

- `Telegram/Plugins/Examples/ai_chat.cpp`
  uses `QtNetwork`, `QtWidgets`, host commands, outgoing interception, and a settings page.
- `Telegram/Plugins/Examples/transparent_telegram.cpp`
  uses host-rendered settings plus window widget callbacks for UI effects.

## Important limitation

The plugin must still match the exact ABI of the client build:

- platform
- compiler
- pointer size
- Qt major/minor
- Astrogram plugin API version

Simply renaming a random shared library to `.tgd` does not make it loadable.
