/*
Example plugin for Telegram Desktop.
Registers a menu action that shows a toast.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QString>

TGD_PLUGIN_PREVIEW(
	"example.hello_menu",
	"Example Hello Menu",
	"1.1",
	"Example",
	"Adds a menu action that shows a toast.",
	"",
	"GusTheDuck/1")

class HelloMenuPlugin final : public Plugins::Plugin {
public:
	explicit HelloMenuPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.hello_menu");
		_info.name = QStringLiteral("Example Hello Menu");
		_info.version = QStringLiteral("1.1");
		_info.author = QStringLiteral("Example");
		_info.description = QStringLiteral("Adds a menu action that shows a toast.");
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_actionId = _host->registerActionWithContext(
			_info.id,
			QStringLiteral("Say Hello"),
			QStringLiteral("Show a hello toast."),
			[=](const Plugins::ActionContext &) {
				_host->showToast(QStringLiteral("Hello from plugin."));
			});
	}

	void onUnload() override {
		if (_actionId) {
			_host->unregisterAction(_actionId);
			_actionId = 0;
		}
	}

private:
	Plugins::Host *_host = nullptr;
	Plugins::ActionId _actionId = 0;
	Plugins::PluginInfo _info;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new HelloMenuPlugin(host);
}
