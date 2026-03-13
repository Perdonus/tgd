/*
Example plugin for Telegram Desktop.
Adds a panel with a slider that makes Telegram windows transparent.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>

#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"example.transparent_telegram",
	"Transparent Telegram",
	"1.3",
	"Codex",
	"Makes Telegram main windows transparent with a live slider.",
	"",
	"GusTheDuck/4")

namespace {

constexpr auto kDialogMarkerProperty = "tgd.transparent.settings_dialog";
constexpr auto kOpacityKey = "opacityPercent";
constexpr int kDefaultOpacityPercent = 85;
constexpr int kMinOpacityPercent = 20;
constexpr int kMaxOpacityPercent = 100;

QString PercentText(int value) {
	return QString::number(value) + QStringLiteral("%");
}

} // namespace

class TransparentTelegramPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit TransparentTelegramPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.transparent_telegram");
		_info.name = QStringLiteral("Transparent Telegram");
		_info.version = QStringLiteral("1.3");
		_info.author = QStringLiteral("Codex");
		_info.description = QStringLiteral(
			"Makes Telegram main windows transparent with a live slider.");
		_configPath = QDir(_host->pluginsPath()).filePath(
			_info.id + QStringLiteral(".json"));
		loadConfig();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_panelId = _host->registerPanel(
			_info.id,
			{
				QStringLiteral("Transparency"),
				QStringLiteral("Adjust Telegram main window opacity."),
			},
			[=](Window::Controller *window) {
				openSettingsDialog(window);
			});

		_host->onWindowCreated([=](Window::Controller *window) {
			applyOpacityToWindow(window);
		});

		applyCurrentOpacityToTelegramWindows();
	}

	void onUnload() override {
		if (_settingsDialog) {
			_settingsDialog->close();
			_settingsDialog = nullptr;
		}
		if (_panelId) {
			_host->unregisterPanel(_panelId);
			_panelId = 0;
		}
		restoreOpaque();
	}

private:
	QWidget *resolveDialogParent() const {
		if (const auto active = QApplication::activeWindow()) {
			return active->window();
		}
		return nullptr;
	}

	void openSettingsDialog(Window::Controller *) {
		if (_settingsDialog) {
			_settingsDialog->raise();
			_settingsDialog->activateWindow();
			return;
		}

		auto *parent = resolveDialogParent();
		auto dialog = parent ? new QDialog(parent) : new QDialog();
		_settingsDialog = dialog;
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setProperty(kDialogMarkerProperty, true);
		dialog->setWindowModality(parent ? Qt::WindowModal : Qt::NonModal);
		dialog->setWindowTitle(QStringLiteral("Telegram Transparency"));
		dialog->setMinimumWidth(420);

		auto layout = new QVBoxLayout(dialog);

		auto intro = new QLabel(
			QStringLiteral(
				"Changes are applied immediately to Telegram main windows. "
				"Transient menus and popup layers stay opaque for stability."),
			dialog);
		intro->setWordWrap(true);
		layout->addWidget(intro);

		auto value = new QLabel(PercentText(_opacityPercent), dialog);
		auto font = value->font();
		font.setBold(true);
		font.setPointSize(font.pointSize() + 2);
		value->setFont(font);
		value->setAlignment(Qt::AlignHCenter);
		layout->addWidget(value);

		auto slider = new QSlider(Qt::Horizontal, dialog);
		slider->setRange(kMinOpacityPercent, kMaxOpacityPercent);
		slider->setSingleStep(1);
		slider->setPageStep(5);
		slider->setValue(_opacityPercent);
		layout->addWidget(slider);

		auto hint = new QLabel(
			QStringLiteral(
				"100% is fully opaque. 20% is the minimum so the app stays usable."),
			dialog);
		hint->setWordWrap(true);
		layout->addWidget(hint);

		auto buttons = new QDialogButtonBox(
			QDialogButtonBox::Close,
			dialog);
		auto reset = buttons->addButton(
			QStringLiteral("Reset"),
			QDialogButtonBox::ResetRole);
		layout->addWidget(buttons);

		QObject::connect(slider, &QSlider::valueChanged, this, [=](int valueNow) {
			value->setText(PercentText(valueNow));
			setOpacityPercent(valueNow, false);
		});
		QObject::connect(slider, &QSlider::sliderReleased, this, [=] {
			saveConfig();
		});
		QObject::connect(reset, &QPushButton::clicked, this, [=] {
			slider->setValue(kDefaultOpacityPercent);
			saveConfig();
		});
		QObject::connect(
			buttons,
			&QDialogButtonBox::rejected,
			dialog,
			&QDialog::reject);
		QObject::connect(dialog, &QDialog::finished, this, [=] {
			saveConfig();
			_settingsDialog = nullptr;
		});

		dialog->show();
		dialog->raise();
		dialog->activateWindow();
	}

	void setOpacityPercent(int value, bool persist) {
		const auto clamped = std::clamp(
			value,
			kMinOpacityPercent,
			kMaxOpacityPercent);
		if (_opacityPercent == clamped && !persist) {
			return;
		}
		_opacityPercent = clamped;
		applyCurrentOpacityToTelegramWindows();
		if (persist) {
			saveConfig();
		}
	}

	void applyCurrentOpacityToTelegramWindows() {
		_host->forEachWindow([=](Window::Controller *window) {
			applyOpacityToWindow(window);
		});
	}

	void restoreOpaque() {
		_host->forEachWindow([=](Window::Controller *window) {
			restoreOpacityForWindow(window);
		});
	}

	void applyOpacityToWindow(Window::Controller *window) const {
		if (!window) {
			return;
		}
		auto *widget = window->widget().get();
		if (!widget || !widget->isWindow() || shouldSkipWidget(widget)) {
			return;
		}
		widget->setWindowOpacity(opacityValue());
	}

	void restoreOpacityForWindow(Window::Controller *window) const {
		if (!window) {
			return;
		}
		auto *widget = window->widget().get();
		if (!widget || !widget->isWindow()) {
			return;
		}
		widget->setWindowOpacity(1.0);
	}

	bool shouldSkipWidget(QWidget *widget) const {
		return widget
			&& widget->property(kDialogMarkerProperty).toBool();
	}

	double opacityValue() const {
		return std::clamp(
			_opacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent) / 100.0;
	}

	void loadConfig() {
		_opacityPercent = kDefaultOpacityPercent;

		QFile file(_configPath);
		if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
			return;
		}

		const auto document = QJsonDocument::fromJson(file.readAll());
		if (!document.isObject()) {
			return;
		}

		const auto value = document.object().value(kOpacityKey);
		if (value.isDouble()) {
			_opacityPercent = std::clamp(
				value.toInt(),
				kMinOpacityPercent,
				kMaxOpacityPercent);
		}
	}

	void saveConfig() const {
		QDir().mkpath(QFileInfo(_configPath).absolutePath());

		QFile file(_configPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return;
		}

		const auto document = QJsonDocument(QJsonObject{
			{ QString::fromLatin1(kOpacityKey), _opacityPercent },
		});
		file.write(document.toJson(QJsonDocument::Indented));
	}

	Plugins::Host *_host = nullptr;
	Plugins::PanelId _panelId = 0;
	Plugins::PluginInfo _info;
	QString _configPath;
	int _opacityPercent = kDefaultOpacityPercent;
	QPointer<QDialog> _settingsDialog;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new TransparentTelegramPlugin(host);
}
