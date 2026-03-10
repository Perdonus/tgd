/*
Example plugin for Telegram Desktop.
Registers a panel entry that opens a simple dialog.
*/
#include "plugins/plugins_api.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

TGD_PLUGIN_PREVIEW(
	"example.panel_demo",
	"Example Panel",
	"1.0",
	"Example",
	"Opens a demo UI panel.",
	"",
	"")

class PanelDemoPlugin final : public Plugins::Plugin {
public:
	explicit PanelDemoPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.panel_demo");
		_info.name = QStringLiteral("Example Panel");
		_info.version = QStringLiteral("1.0");
		_info.author = QStringLiteral("Example");
		_info.description = QStringLiteral("Opens a demo UI panel.");
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_panelId = _host->registerPanel(
			_info.id,
			{
				QStringLiteral("Open Demo Panel"),
				QStringLiteral("Shows a dialog created by the plugin."),
			},
			[=](Window::Controller *) {
				auto dialog = new QDialog(QApplication::activeWindow());
				dialog->setAttribute(Qt::WA_DeleteOnClose);
				dialog->setWindowTitle(QStringLiteral("Plugin Panel"));

				auto layout = new QVBoxLayout(dialog);
				layout->addWidget(new QLabel(
					QStringLiteral("Hello from the plugin UI."),
					dialog));
				auto close = new QPushButton(QStringLiteral("Close"), dialog);
				layout->addWidget(close);
				QObject::connect(
					close,
					&QPushButton::clicked,
					dialog,
					&QDialog::accept);
				dialog->show();
			});
	}

	void onUnload() override {
		if (_panelId) {
			_host->unregisterPanel(_panelId);
			_panelId = 0;
		}
	}

private:
	Plugins::Host *_host = nullptr;
	Plugins::PanelId _panelId = 0;
	Plugins::PluginInfo _info;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new PanelDemoPlugin(host);
}
