# TGD Plugin Examples

These examples build small Telegram Desktop plugins using the native C++ API.

Requirements
- Build with the same compiler and Qt version as Telegram Desktop.
- Link against QtCore (at minimum).
- UI plugins should also link QtWidgets.

Examples
- command_shrug.cpp: registers /shrug.
- hello_menu.cpp: adds a context action with access to the active window.
- panel_demo.cpp: registers a panel that opens a dialog.
- message_observer.cpp: shows a toast on new incoming messages.
- runtime_info.cpp: reads host/system/runtime information from the host API.
- transparent_telegram.cpp: adds a transparency slider for Telegram windows.

Build (Linux)
1) From this folder, compile a shared library:
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o hello_menu.so hello_menu.cpp $(pkg-config --cflags --libs Qt6Core)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o panel_demo.so panel_demo.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o transparent_telegram.so transparent_telegram.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
2) Rename the shared library to .tgd:
   mv hello_menu.so hello_menu.tgd
   mv panel_demo.so panel_demo.tgd
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
