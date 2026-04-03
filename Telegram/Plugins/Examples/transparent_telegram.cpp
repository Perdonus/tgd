/*
Astrogram transparency plugin.
Adds separate host-managed sliders for window opacity, message opacity,
and text/widget opacity.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>

TGD_PLUGIN_PREVIEW(
	"example.transparent_telegram",
	"AstroTransparent",
	"2.5",
	"@etopizdesblin",
	"Adds separate interface, message, and text transparency controls for Astrogram.",
	"https://sosiskibot.ru",
	"GusTheDuck/4")

namespace {

constexpr int kDefaultWindowOpacityPercent = 85;
constexpr int kDefaultMessageOpacityPercent = 100;
constexpr int kDefaultTextOpacityPercent = 100;
constexpr int kMinOpacityPercent = 20;
constexpr int kMaxOpacityPercent = 100;
constexpr auto kPluginId = "example.transparent_telegram";

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
}

QString Utf8(const char *value) {
	return QString::fromUtf8(value);
}

bool UseRussian(const Plugins::Host *host) {
	const auto hostInfo = host->hostInfo();
	const auto fallback = host->systemInfo().uiLanguage;
	const auto language = !hostInfo.appUiLanguage.trimmed().isEmpty()
		? hostInfo.appUiLanguage.trimmed()
		: fallback.trimmed();
	return language.toLower().startsWith(QStringLiteral("ru"));
}

QString PluginText(
		const Plugins::Host *host,
		const char *en,
		const char *ru) {
	return UseRussian(host) ? Utf8(ru) : Utf8(en);
}

bool IsSupportedWindowWidget(QWidget *widget) {
	if (!widget || !widget->isWindow() || widget->parentWidget()) {
		return false;
	}
	return widget->windowType() == Qt::Window
		&& !widget->testAttribute(Qt::WA_DontShowOnScreen);
}

bool IsReadyWidget(QWidget *widget) {
	return widget
		&& widget->testAttribute(Qt::WA_WState_Created)
		&& !widget->testAttribute(Qt::WA_DontShowOnScreen);
}

bool LooksLikeMessageSurface(QWidget *widget) {
	if (!widget || widget->isWindow()) {
		return false;
	}
	const auto objectName = widget->objectName().toLower();
	const auto className = QByteArray(widget->metaObject()->className()).toLower();
	const auto contains = [&](const char *needle) {
		return objectName.contains(QString::fromLatin1(needle))
			|| className.contains(needle);
	};
	if (contains("tooltip")
		|| contains("menu")
		|| contains("button")
		|| contains("input")
		|| contains("edit")
		|| contains("lineedit")
		|| contains("scrollbar")
		|| contains("slider")
		|| contains("checkbox")
		|| contains("radiobutton")
		|| contains("emoji")) {
		return false;
	}
	return contains("history")
		|| contains("message")
		|| contains("media")
		|| contains("overview")
		|| contains("chatlist")
		|| contains("dialogs");
}

bool LooksLikeTextWidget(QWidget *widget) {
	if (!widget || widget->isWindow()) {
		return false;
	}
	const auto objectName = widget->objectName().toLower();
	const auto className = QByteArray(widget->metaObject()->className()).toLower();
	const auto contains = [&](const char *needle) {
		return objectName.contains(QString::fromLatin1(needle))
			|| className.contains(needle);
	};
	return contains("label")
		|| contains("text")
		|| contains("button")
		|| contains("input")
		|| contains("edit")
		|| contains("lineedit")
		|| contains("checkbox")
		|| contains("radio")
		|| contains("switch")
		|| contains("title")
		|| contains("subtitle");
}

void ApplyAlpha(QPalette &palette, QPalette::ColorRole role, int alpha) {
	for (const auto group : {
			QPalette::Active,
			QPalette::Inactive,
			QPalette::Disabled,
		}) {
		auto color = palette.color(group, role);
		color.setAlpha(std::clamp(alpha, 0, 255));
		palette.setColor(group, role, color);
	}
}

} // namespace

class AstroTransparentPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit AstroTransparentPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host) {
		_info.id = Latin1(kPluginId);
		_info.name = PluginText(
			_host,
			"AstroTransparent",
			"АстроПрозрачность");
		_info.version = QStringLiteral("2.5");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = PluginText(
			_host,
			"Adds separate interface, message, and text transparency controls for Astrogram.",
			"Добавляет отдельные настройки прозрачности интерфейса, сообщений и текста в Astrogram.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_windowOpacityPercent = readWindowOpacityPercent();
		_messageOpacityPercent = readMessageOpacityPercent();
		_textOpacityPercent = readTextOpacityPercent();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_windowOpacityPercent = readWindowOpacityPercent();
		_messageOpacityPercent = readMessageOpacityPercent();
		_textOpacityPercent = readTextOpacityPercent();
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				if (setting.id == QStringLiteral("window_opacity")) {
					setWindowOpacityPercent(setting.intValue);
				} else if (setting.id == QStringLiteral("message_opacity")) {
					setMessageOpacityPercent(setting.intValue);
				} else if (setting.id == QStringLiteral("text_opacity")) {
					setTextOpacityPercent(setting.intValue);
				}
			});

		_host->onWindowWidgetCreated([this](QWidget *widget) {
			const auto guard = QPointer<QWidget>(widget);
			QTimer::singleShot(0, this, [this, guard] {
				if (guard) {
					applyToWindow(guard.data());
				}
			});
		});

		scheduleAppearanceApply();
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		restoreDefaults();
	}

private:
	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto windowSlider = Plugins::SettingDescriptor();
		windowSlider.id = QStringLiteral("window_opacity");
		windowSlider.title = PluginText(
			_host,
			"Window opacity",
			"Прозрачность окна");
		windowSlider.description = PluginText(
			_host,
			"Controls the opacity of Astrogram windows.",
			"Управляет общей прозрачностью окон Astrogram.");
		windowSlider.type = Plugins::SettingControl::IntSlider;
		windowSlider.intValue = _windowOpacityPercent;
		windowSlider.intMinimum = kMinOpacityPercent;
		windowSlider.intMaximum = kMaxOpacityPercent;
		windowSlider.intStep = 1;
		windowSlider.valueSuffix = QStringLiteral("%");

		auto messageSlider = Plugins::SettingDescriptor();
		messageSlider.id = QStringLiteral("message_opacity");
		messageSlider.title = PluginText(
			_host,
			"Message opacity",
			"Прозрачность сообщений");
		messageSlider.description = PluginText(
			_host,
			"Controls message/history surfaces and attached media blocks separately from the main interface.",
			"Отдельно управляет поверхностями сообщений, истории и медиа-блоками независимо от общего интерфейса.");
		messageSlider.type = Plugins::SettingControl::IntSlider;
		messageSlider.intValue = _messageOpacityPercent;
		messageSlider.intMinimum = kMinOpacityPercent;
		messageSlider.intMaximum = kMaxOpacityPercent;
		messageSlider.intStep = 1;
		messageSlider.valueSuffix = QStringLiteral("%");

		auto textSlider = Plugins::SettingDescriptor();
		textSlider.id = QStringLiteral("text_opacity");
		textSlider.title = PluginText(
			_host,
			"Text opacity",
			"Прозрачность текста");
		textSlider.description = PluginText(
			_host,
			"Applies a softer alpha to widget-based text and controls.",
			"Применяет более мягкую альфу к тексту и элементам управления на виджетах.");
		textSlider.type = Plugins::SettingControl::IntSlider;
		textSlider.intValue = _textOpacityPercent;
		textSlider.intMinimum = kMinOpacityPercent;
		textSlider.intMaximum = kMaxOpacityPercent;
		textSlider.intStep = 1;
		textSlider.valueSuffix = QStringLiteral("%");

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("info");
		info.title = PluginText(
			_host,
			"How it works",
			"Как это работает");
		info.description = PluginText(
			_host,
			"Interface opacity affects the whole Astrogram window. Message opacity targets history/message surfaces. Text opacity adjusts widget-based text and controls and may be partial in heavily custom-drawn areas.",
			"Прозрачность интерфейса влияет на всё окно Astrogram. Прозрачность сообщений нацелена на поверхности истории и пузырей. Прозрачность текста меняет альфу текста виджетов и контролов и может работать частично в сильно кастомно отрисованных зонах.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("appearance");
		section.title = PluginText(
			_host,
			"Appearance",
			"Оформление");
		section.settings.push_back(windowSlider);
		section.settings.push_back(messageSlider);
		section.settings.push_back(textSlider);
		section.settings.push_back(info);

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("astro_transparent");
		page.title = PluginText(
			_host,
			"AstroTransparent",
			"АстроПрозрачность");
		page.description = PluginText(
			_host,
			"Separate transparency controls for interface, messages, and text.",
			"Раздельные настройки прозрачности для интерфейса, сообщений и текста.");
		page.sections.push_back(section);
		return page;
	}

	void setWindowOpacityPercent(int value) {
		const auto clamped = std::clamp(
			value,
			kMinOpacityPercent,
			kMaxOpacityPercent);
		if (_windowOpacityPercent == clamped) {
			return;
		}
		_windowOpacityPercent = clamped;
		scheduleAppearanceApply();
	}

	void setMessageOpacityPercent(int value) {
		const auto clamped = std::clamp(
			value,
			kMinOpacityPercent,
			kMaxOpacityPercent);
		if (_messageOpacityPercent == clamped) {
			return;
		}
		_messageOpacityPercent = clamped;
		scheduleAppearanceApply();
	}

	void setTextOpacityPercent(int value) {
		const auto clamped = std::clamp(
			value,
			kMinOpacityPercent,
			kMaxOpacityPercent);
		if (_textOpacityPercent == clamped) {
			return;
		}
		_textOpacityPercent = clamped;
		scheduleAppearanceApply();
	}

	void scheduleAppearanceApply() {
		if (_appearanceApplyScheduled) {
			return;
		}
		_appearanceApplyScheduled = true;
		QTimer::singleShot(0, this, [this] {
			_appearanceApplyScheduled = false;
			applyCurrentAppearance();
		});
	}

	void applyCurrentAppearance() {
		applyWindowOpacity();
		applyWidgetPalettes();
	}

	void applyWindowOpacity() const {
		_host->forEachWindowWidget([this](QWidget *widget) {
			if (IsSupportedWindowWidget(widget)) {
				widget->setWindowOpacity(windowOpacityValue());
			}
		});
		if (const auto widget = _host->activeWindowWidget();
			IsSupportedWindowWidget(widget)) {
			widget->setWindowOpacity(windowOpacityValue());
		}
	}

	void applyToWindow(QWidget *widget) {
		if (!IsSupportedWindowWidget(widget)) {
			return;
		}
		widget->setWindowOpacity(windowOpacityValue());
		applyPalettePassToRoot(widget);
	}

	void applyWidgetPalettes() {
		_host->forEachWindowWidget([this](QWidget *widget) {
			applyPalettePassToRoot(widget);
		});
		if (const auto widget = _host->activeWindowWidget()) {
			applyPalettePassToRoot(widget);
		}
	}

	void applyPalettePassToRoot(QWidget *root) {
		if (!IsSupportedWindowWidget(root) || !IsReadyWidget(root)) {
			return;
		}
		for (const auto child : root->findChildren<QWidget*>()) {
			applyPaletteToWidget(child);
		}
	}

	void applyPaletteToWidget(QWidget *widget) {
		if (!widget || !IsReadyWidget(widget)) {
			return;
		}
		if (!_originalPalettes.contains(widget)) {
			_originalPalettes.insert(widget, widget->palette());
			QObject::connect(
				widget,
				&QObject::destroyed,
				this,
				[this, widget](QObject *) {
					_originalPalettes.remove(widget);
				});
		}
		const auto original = _originalPalettes.value(widget);
		auto adjusted = original;
		auto changed = false;
		if (LooksLikeMessageSurface(widget) && _messageOpacityPercent < kMaxOpacityPercent) {
			adjusted = adjustedMessagePalette(adjusted);
			changed = true;
		}
		if (LooksLikeTextWidget(widget) && _textOpacityPercent < kMaxOpacityPercent) {
			adjusted = adjustedTextPalette(adjusted);
			changed = true;
		}
		widget->setPalette(changed ? adjusted : original);
		widget->update();
	}

	QPalette adjustedMessagePalette(QPalette palette) const {
		const auto alpha = messageOpacityAlpha();
		for (const auto role : {
				QPalette::Window,
				QPalette::Base,
				QPalette::AlternateBase,
				QPalette::Button,
				QPalette::ToolTipBase,
			}) {
			ApplyAlpha(palette, role, alpha);
		}
		return palette;
	}

	QPalette adjustedTextPalette(QPalette palette) const {
		const auto alpha = textOpacityAlpha();
		for (const auto role : {
				QPalette::WindowText,
				QPalette::Text,
				QPalette::ButtonText,
				QPalette::BrightText,
				QPalette::HighlightedText,
				QPalette::PlaceholderText,
				QPalette::ToolTipText,
			}) {
			ApplyAlpha(palette, role, alpha);
		}
		return palette;
	}

	void restoreDefaults() {
		_host->forEachWindowWidget([](QWidget *widget) {
			if (IsSupportedWindowWidget(widget)) {
				widget->setWindowOpacity(1.0);
			}
		});
		if (const auto widget = _host->activeWindowWidget();
			IsSupportedWindowWidget(widget)) {
			widget->setWindowOpacity(1.0);
		}
		restorePalettes();
	}

	void restorePalettes() {
		for (auto i = _originalPalettes.begin(); i != _originalPalettes.end(); ++i) {
			if (i.key()) {
				i.key()->setPalette(i.value());
				i.key()->update();
			}
		}
		_originalPalettes.clear();
	}

	double windowOpacityValue() const {
		return std::clamp(
			_windowOpacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent) / 100.0;
	}

	double messageOpacityValue() const {
		return std::clamp(
			_messageOpacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent) / 100.0;
	}

	int messageOpacityAlpha() const {
		return std::clamp(
			int(std::lround(std::clamp(
				_messageOpacityPercent,
				kMinOpacityPercent,
				kMaxOpacityPercent) * 2.55)),
			51,
			255);
	}

	int textOpacityAlpha() const {
		return std::clamp(
			int(std::lround(std::clamp(
				_textOpacityPercent,
				kMinOpacityPercent,
				kMaxOpacityPercent) * 2.55)),
			51,
			255);
	}

	int readWindowOpacityPercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				QStringLiteral("window_opacity"),
				kDefaultWindowOpacityPercent),
			kMinOpacityPercent,
			kMaxOpacityPercent);
	}

	int readMessageOpacityPercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				QStringLiteral("message_opacity"),
				kDefaultMessageOpacityPercent),
			kMinOpacityPercent,
			kMaxOpacityPercent);
	}

	int readTextOpacityPercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				QStringLiteral("text_opacity"),
				kDefaultTextOpacityPercent),
			kMinOpacityPercent,
			kMaxOpacityPercent);
	}

	Plugins::Host *_host = nullptr;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	int _windowOpacityPercent = kDefaultWindowOpacityPercent;
	int _messageOpacityPercent = kDefaultMessageOpacityPercent;
	int _textOpacityPercent = kDefaultTextOpacityPercent;
	QHash<QWidget*, QPalette> _originalPalettes;
	bool _appearanceApplyScheduled = false;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AstroTransparentPlugin(host);
}
