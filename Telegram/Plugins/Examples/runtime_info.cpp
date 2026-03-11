/*
Example plugin for Telegram Desktop.
Shows a short host/system summary using the runtime info API.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QString>

TGD_PLUGIN_PREVIEW(
	"example.runtime_info",
	"Example Runtime Info",
	"1.0",
	"Example",
	"Shows host and system information through the plugin API.",
	"",
	"GusTheDuck/6")

class RuntimeInfoPlugin final : public Plugins::Plugin {
public:
	explicit RuntimeInfoPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.runtime_info");
		_info.name = QStringLiteral("Example Runtime Info");
		_info.version = QStringLiteral("1.0");
		_info.author = QStringLiteral("Example");
		_info.description = QStringLiteral(
			"Shows host and system information through the plugin API.");
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_actionId = _host->registerActionWithContext(
			_info.id,
			QStringLiteral("Show Runtime Info"),
			QStringLiteral("Show a short host/system summary."),
			[=](const Plugins::ActionContext &) {
				const auto host = _host->hostInfo();
				const auto system = _host->systemInfo();
				auto text = QStringLiteral(
					"App %1 | %2 | %3 | API %4")
					.arg(host.appVersion)
					.arg(system.prettyProductName)
					.arg(system.architecture)
					.arg(host.apiVersion);
				if (host.runtimeApiEnabled) {
					text += QStringLiteral(" | ") + host.runtimeApiBaseUrl;
				}
				_host->showToast(text);
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
	return new RuntimeInfoPlugin(host);
}
