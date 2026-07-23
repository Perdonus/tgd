# TGD Plugin Examples

These examples build small Telegram Desktop plugins using the native C++ API.

Requirements
- Build with the same compiler and Qt version as Telegram Desktop.
- Link against QtCore (at minimum).
- UI plugins should also link QtWidgets.

Examples
- ai_chat.cpp: intercepts `/ai`, opens a plugin chat dialog and talks to `sosiskibot.ru`.
- font_tuner.cpp: tunes Astrogram fonts, loads a local font file and applies changes live.
- show_logs.cpp: opens a translucent log overlay with filtering, copy and clear actions.
- transparent_telegram.cpp: adds a transparency slider for Telegram windows.

Build (Linux)
1) From this folder, compile a shared library:
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o ai_chat.so ai_chat.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets Qt6Network)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o font_tuner.so font_tuner.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o show_logs.so show_logs.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o transparent_telegram.so transparent_telegram.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
2) Rename the shared library to .tgd:
   mv ai_chat.so ai_chat.tgd
   mv font_tuner.so font_tuner.tgd
   mv show_logs.so show_logs.tgd
   mv transparent_telegram.so transparent_telegram.tgd
3) Copy the .tgd file into:
   <working dir>/tdata/plugins
4) Open Settings > Plugins. The plugin manager reloads automatically after install/update.

Notes
- The entry symbol is `TgdPluginEntry`.
- The metadata symbol is `TgdPluginBinaryInfo`.
- The install-preview symbol is `TgdPluginPreviewInfo`.
- In-chat plugin install/update uses the static preview metadata before load.
- Preview icons use the format `StickerPackShortName/index`.
- The plugin API lives in `Telegram/SourceFiles/plugins/plugins_api.h`.
- `Host::hostInfo()` and `Host::systemInfo()` expose app/runtime/system metadata.
- Plugins must match the app ABI exactly: same API version, platform, pointer size, compiler ABI and Qt major/minor.
- CI touch: keep this file unchanged functionally.
