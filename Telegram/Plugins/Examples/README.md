# TGD Plugin Examples

These examples build small Telegram Desktop plugins using the native C++ API.

Requirements
- Build with the same compiler and Qt version as Telegram Desktop.
- Link against QtCore (at minimum).
- UI plugins should also link QtWidgets.

Examples
- ai_chat.cpp: intercepts `/ai` and opens a built-in AI chat window backed by `sosiskibot.ru/api`.
- accent_color.cpp: applies a custom accent tint to common Astrogram palette roles.
- ayu_safe.cpp: adds Ayu-inspired visual tweaks and a local message-safety cache.
- blur_telegram.cpp: applies blur effects to interface and message surfaces where Qt effects are available.
- font_tuner.cpp: changes the app font scale and loads custom fonts from a file or URL.
- transparent_telegram.cpp: adds separate transparency sliders for the interface, message surfaces, and widget text layers.

Build (Linux)
1) From this folder, compile a shared library:
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o ai_chat.so ai_chat.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets Qt6Network)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o ayu_safe.so ayu_safe.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o transparent_telegram.so transparent_telegram.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
2) Rename the shared library to .tgd:
   mv ai_chat.so ai_chat.tgd
   mv ayu_safe.so ayu_safe.tgd
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
