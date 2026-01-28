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

Build (Linux)
1) From this folder, compile a shared library:
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o hello_menu.so hello_menu.cpp $(pkg-config --cflags --libs Qt6Core)
   g++ -std=c++20 -fPIC -shared -I../../SourceFiles \
     -o panel_demo.so panel_demo.cpp \
     $(pkg-config --cflags --libs Qt6Core Qt6Widgets)
2) Rename the shared library to .tgd:
   mv hello_menu.so hello_menu.tgd
   mv panel_demo.so panel_demo.tgd
3) Copy the .tgd file into:
   <working dir>/tdata/plugins
4) Open Settings > Plugins and click Reload Plugins.

Notes
- The entry symbol is `TgdPluginEntry`.
- The plugin API lives in `Telegram/SourceFiles/plugins/plugins_api.h`.
