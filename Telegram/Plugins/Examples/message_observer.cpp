/*
Example plugin for Telegram Desktop.
Shows a toast when a new incoming message arrives.
*/
#include "plugins/plugins_api.h"

TGD_PLUGIN_PREVIEW(
	"example.message_observer",
	"Example Message Observer",
	"1.0",
	"Example",
	"Toasts on new incoming messages.",
	"",
	"GusTheDuck/2")

class MessageObserverPlugin final : public Plugins::Plugin {
public:
	explicit MessageObserverPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.message_observer");
		_info.name = QStringLiteral("Example Message Observer");
		_info.version = QStringLiteral("1.0");
		_info.author = QStringLiteral("Example");
		_info.description = QStringLiteral("Toasts on new incoming messages.");
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		auto options = Plugins::MessageObserverOptions();
		options.newMessages = true;
		options.editedMessages = false;
		options.deletedMessages = false;
		options.incoming = true;
		options.outgoing = false;
		_observerId = _host->registerMessageObserver(
			_info.id,
			options,
			[=](const Plugins::MessageEventContext &) {
				_host->showToast(QStringLiteral("New message received."));
			});
	}

	void onUnload() override {
		if (_observerId) {
			_host->unregisterMessageObserver(_observerId);
			_observerId = 0;
		}
	}

private:
	Plugins::Host *_host = nullptr;
	Plugins::MessageObserverId _observerId = 0;
	Plugins::PluginInfo _info;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new MessageObserverPlugin(host);
}
