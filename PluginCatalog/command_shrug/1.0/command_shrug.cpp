/*
Example plugin for Telegram Desktop.
Registers /shrug and replaces the message with a fixed text.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QString>

TGD_PLUGIN_PREVIEW(
	"example.shrug",
	"Example Shrug",
	"1.0",
	"Example",
	"Replaces /shrug with [shrug].",
	"",
	"")

class ShrugPlugin final : public Plugins::Plugin {
public:
	explicit ShrugPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.shrug");
		_info.name = QStringLiteral("Example Shrug");
		_info.version = QStringLiteral("1.0");
		_info.author = QStringLiteral("Example");
		_info.description = QStringLiteral("Replaces /shrug with [shrug].");
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_commandId = _host->registerCommand(
			_info.id,
			{
				QStringLiteral("shrug"),
				QStringLiteral("Insert [shrug]."),
				QStringLiteral("/shrug")
			},
			[=](const Plugins::CommandContext &) {
				auto result = Plugins::CommandResult();
				result.action = Plugins::CommandResult::Action::ReplaceText;
				result.replacementText = QStringLiteral("[shrug]");
				return result;
			});
	}

	void onUnload() override {
		if (_commandId) {
			_host->unregisterCommand(_commandId);
			_commandId = 0;
		}
	}

private:
	Plugins::Host *_host = nullptr;
	Plugins::CommandId _commandId = 0;
	Plugins::PluginInfo _info;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new ShrugPlugin(host);
}
