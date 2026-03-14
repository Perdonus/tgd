/*
Example plugin for Telegram Desktop.
Adds a panel with a slider that makes Telegram windows transparent.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"example.transparent_telegram",
	"Transparent Telegram",
	"1.4",
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

#ifdef Q_OS_WIN
constexpr auto kNativeOpaqueAlpha = BYTE(255);
#endif // Q_OS_WIN

} // namespace

class TransparentTelegramPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit TransparentTelegramPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.transparent_telegram");
		_info.name = QStringLiteral("Transparent Telegram");
		_info.version = QStringLiteral("1.4");
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
		logPluginEvent(QStringLiteral("load"), QJsonObject{
			{ QStringLiteral("opacityPercent"), _opacityPercent },
		});
		_panelId = _host->registerPanel(
			_info.id,
			{
				QStringLiteral("Transparency"),
				QStringLiteral("Adjust Telegram main window opacity."),
			},
			[=](Window::Controller *) {
				openSettingsDialog();
			});

		_host->onWindowCreated([=](Window::Controller *) {
			logPluginEvent(QStringLiteral("window-created"));
			scheduleApplyPasses(QStringLiteral("window-created"));
		});

		scheduleApplyPasses(QStringLiteral("load"));
	}

	void onUnload() override {
		logPluginEvent(QStringLiteral("unload"));
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
	void logPluginEvent(
		const QString &event,
		QJsonObject payload = QJsonObject()) const {
		payload.insert(QStringLiteral("pluginId"), _info.id);
		payload.insert(QStringLiteral("version"), _info.version);

		const auto tdataPath = QFileInfo(_host->pluginsPath()).absolutePath();
		const auto logPath = QDir(tdataPath).filePath(QStringLiteral("plugins.log"));
		QFile file(logPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
			return;
		}

		QTextStream stream(&file);
		stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
			<< " phase=\"plugin.transparent_telegram\" event=\""
			<< event
			<< "\" payload="
			<< QString::fromUtf8(
				QJsonDocument(payload).toJson(QJsonDocument::Compact))
			<< '\n';
	}

	QJsonObject widgetToJson(QWidget *widget) const {
		if (!widget) {
			return QJsonObject{
				{ QStringLiteral("exists"), false },
			};
		}
		return QJsonObject{
			{ QStringLiteral("exists"), true },
			{ QStringLiteral("className"), QString::fromLatin1(widget->metaObject()->className()) },
			{ QStringLiteral("objectName"), widget->objectName() },
			{ QStringLiteral("title"), widget->windowTitle() },
			{ QStringLiteral("visible"), widget->isVisible() },
			{ QStringLiteral("isWindow"), widget->isWindow() },
			{ QStringLiteral("windowType"), int(widget->windowType()) },
			{ QStringLiteral("windowFlags"), int(widget->windowFlags()) },
			{ QStringLiteral("width"), widget->width() },
			{ QStringLiteral("height"), widget->height() },
		};
	}

	QString skipReasonForWidget(QWidget *widget) const {
		if (!widget) {
			return QStringLiteral("null");
		}
		if (!widget->isWindow()) {
			return QStringLiteral("not-window");
		}
		if (widget->property(kDialogMarkerProperty).toBool()) {
			return QStringLiteral("plugin-dialog");
		}
		switch (widget->windowType()) {
		case Qt::Popup:
		case Qt::Tool:
		case Qt::ToolTip:
		case Qt::SplashScreen:
		case Qt::Desktop:
		case Qt::Drawer:
		case Qt::Sheet:
			return QStringLiteral("transient-window-type");
		default:
			return QString();
		}
	}

	void scheduleApplyPass(const QString &reason, int delayMs) {
		QTimer::singleShot(delayMs, this, [=] {
			applyCurrentOpacityToTelegramWindows(reason);
		});
	}

	void scheduleApplyPasses(const QString &reasonBase) {
		scheduleApplyPass(reasonBase + QStringLiteral("-0"), 0);
		scheduleApplyPass(reasonBase + QStringLiteral("-250"), 250);
		scheduleApplyPass(reasonBase + QStringLiteral("-1000"), 1000);
	}

	void openSettingsDialog() {
		logPluginEvent(QStringLiteral("panel-open-request"));
		if (_settingsDialog) {
			_settingsDialog->raise();
			_settingsDialog->activateWindow();
			logPluginEvent(QStringLiteral("panel-reused"));
			return;
		}

		auto dialog = new QDialog();
		_settingsDialog = dialog;
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setProperty(kDialogMarkerProperty, true);
		dialog->setWindowModality(Qt::NonModal);
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
			logPluginEvent(QStringLiteral("panel-closed"));
		});

		dialog->show();
		dialog->raise();
		dialog->activateWindow();
		logPluginEvent(QStringLiteral("panel-shown"));
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
		logPluginEvent(QStringLiteral("opacity-changed"), QJsonObject{
			{ QStringLiteral("opacityPercent"), _opacityPercent },
			{ QStringLiteral("persist"), persist },
		});
		applyCurrentOpacityToTelegramWindows(QStringLiteral("slider-change"));
		if (persist) {
			saveConfig();
		}
	}

	void applyCurrentOpacityToTelegramWindows(const QString &reason) {
		const auto widgets = QApplication::topLevelWidgets();
		auto applied = 0;
		auto skipped = 0;
		for (auto *widget : widgets) {
			if (applyOpacityToWidget(widget, reason)) {
				++applied;
			} else {
				++skipped;
			}
		}
		logPluginEvent(QStringLiteral("apply-pass"), QJsonObject{
			{ QStringLiteral("reason"), reason },
			{ QStringLiteral("topLevelCount"), int(widgets.size()) },
			{ QStringLiteral("appliedCount"), applied },
			{ QStringLiteral("skippedCount"), skipped },
			{ QStringLiteral("opacityPercent"), _opacityPercent },
		});
	}

	void restoreOpaque() {
		const auto widgets = QApplication::topLevelWidgets();
		auto restored = 0;
		for (auto *widget : widgets) {
			if (restoreOpacityForWidget(widget)) {
				++restored;
			}
		}
		logPluginEvent(QStringLiteral("restore-opaque"), QJsonObject{
			{ QStringLiteral("topLevelCount"), int(widgets.size()) },
			{ QStringLiteral("restoredCount"), restored },
		});
	}

	bool applyOpacityToWidget(QWidget *widget, const QString &reason) const {
		const auto skipReason = skipReasonForWidget(widget);
		if (!skipReason.isEmpty()) {
			auto payload = widgetToJson(widget);
			payload.insert(QStringLiteral("reason"), reason);
			payload.insert(QStringLiteral("skipReason"), skipReason);
			logPluginEvent(QStringLiteral("skip-window"), payload);
			return false;
		}

		auto payload = widgetToJson(widget);
		payload.insert(QStringLiteral("reason"), reason);
		payload.insert(QStringLiteral("opacityPercent"), _opacityPercent);
#ifdef Q_OS_WIN
		const auto ok = applyNativeOpacity(widget, nativeOpacityAlpha(), &payload);
#else
		widget->setWindowOpacity(opacityValue());
		const auto ok = true;
#endif // Q_OS_WIN
		logPluginEvent(ok ? QStringLiteral("apply-window") : QStringLiteral("apply-window-failed"), payload);
		return ok;
	}

	bool restoreOpacityForWidget(QWidget *widget) const {
		const auto skipReason = skipReasonForWidget(widget);
		if (!skipReason.isEmpty()) {
			return false;
		}

		auto payload = widgetToJson(widget);
#ifdef Q_OS_WIN
		const auto ok = applyNativeOpacity(widget, kNativeOpaqueAlpha, &payload);
#else
		widget->setWindowOpacity(1.0);
		const auto ok = true;
#endif // Q_OS_WIN
		logPluginEvent(ok ? QStringLiteral("restore-window") : QStringLiteral("restore-window-failed"), payload);
		return ok;
	}

	double opacityValue() const {
		return std::clamp(
			_opacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent) / 100.0;
	}

#ifdef Q_OS_WIN
	BYTE nativeOpacityAlpha() const {
		const auto percent = std::clamp(
			_opacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent);
		return BYTE((percent * 255) / 100);
	}

	bool applyNativeOpacity(
		QWidget *widget,
		BYTE alpha,
		QJsonObject *payload) const {
		if (!widget) {
			if (payload) {
				payload->insert(QStringLiteral("error"), QStringLiteral("null-widget"));
			}
			return false;
		}
		const auto hwnd = reinterpret_cast<HWND>(widget->winId());
		if (!hwnd) {
			if (payload) {
				payload->insert(QStringLiteral("error"), QStringLiteral("null-hwnd"));
			}
			return false;
		}
		const auto style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
		if ((style & WS_EX_LAYERED) == 0) {
			SetLastError(ERROR_SUCCESS);
			SetWindowLongPtr(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
			const auto styleError = GetLastError();
			if (styleError != ERROR_SUCCESS) {
				if (payload) {
					payload->insert(QStringLiteral("error"), QStringLiteral("set-layered-style-failed"));
					payload->insert(QStringLiteral("win32Error"), int(styleError));
				}
				return false;
			}
		}
		SetLastError(ERROR_SUCCESS);
		const auto applied = (SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA) != FALSE);
		if (payload) {
			if (!applied) {
				payload->insert(QStringLiteral("error"), QStringLiteral("set-layered-alpha-failed"));
				payload->insert(QStringLiteral("win32Error"), int(GetLastError()));
			}
			payload->insert(QStringLiteral("alpha"), int(alpha));
		}
		return applied;
	}
#endif // Q_OS_WIN

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
