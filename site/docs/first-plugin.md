# First Plugin

## Minimal skeleton

```cpp
#include "plugins/plugins_api.h"

class MyPlugin final : public Plugins::Plugin {
public:
	explicit MyPlugin(Plugins::Host *host) : _host(host) {
	}

	Plugins::PluginInfo info() const override {
		return {
			.id = "example.my_plugin",
			.name = "My Plugin",
			.version = "1.0.0",
			.author = "You",
			.description = "Example plugin",
			.website = "https://example.com",
		};
	}

	void onLoad() override {
		_host->showToast("My Plugin loaded");
	}

	void onUnload() override {
	}

private:
	Plugins::Host *_host = nullptr;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new MyPlugin(host);
}
```

## Add install-preview metadata

```cpp
TGD_PLUGIN_PREVIEW(
	"example.my_plugin",
	"My Plugin",
	"1.0.0",
	"You",
	"Example plugin",
	"https://example.com",
	"")
```

## Example CMake

```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

## Linux quick build

```bash
g++ -std=c++20 -fPIC -shared \
  -I../../SourceFiles \
  -o my_plugin.so my_plugin.cpp \
  $(pkg-config --cflags --libs Qt6Core Qt6Widgets)

mv my_plugin.so my_plugin.tgd
```

## What to test first

- Plugin loads without `load-failed` in `plugins.log`
- `info()` metadata is visible
- `onLoad()` runs
- `onUnload()` cleans up registrations
- Safe mode does not trigger on startup
