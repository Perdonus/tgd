/*
Astrogram transparency plugin.
Adds separate host-managed sliders for window opacity and text/widget opacity.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <cmath>

TGD_PLUGIN_PREVIEW(
	"example.transparent_telegram",
	"AstroTransparent",
	"2.2",
	"@etopizdesblin",
	"Adds separate window and text transparency controls for Astrogram.",
	"https://sosiskibot.ru",
	"GusTheDuck/4")

namespace {

constexpr int kDefaultWindowOpacityPercent = 85;
constexpr int kDefaultTextOpacityPercent = 100;
constexpr int kMinOpacityPercent = 20;
constexpr int kMaxOpacityPercent = 100;
constexpr auto kPluginId = "example.transparent_telegram";

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
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
	return UseRussian(host) ? Latin1(ru) : Latin1(en);
}

bool IsSupportedWindowWidget(QWidget *widget) {
	if (!widget || !widget->isWindow() || widget->parentWidget()) {
		return false;
	}
	return widget->windowType() == Qt::Window
		&& !widget->testAttribute(Qt::WA_DontShowOnScreen);
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

class AstroTransparentPlugin final : public Plugins::Plugin {
public:
	explicit AstroTransparentPlugin(Plugins::Host *host) : _host(host) {
		_info.id = Latin1(kPluginId);
		_info.name = PluginText(
			_host,
			"AstroTransparent",
			"АстроПрозрачность");
		_info.version = QStringLiteral("2.2");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = PluginText(
			_host,
			"Adds separate window and text transparency controls for Astrogram.",
			"Добавляет отдельные настройки прозрачности окна и текста в Astrogram.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_windowOpacityPercent = readWindowOpacityPercent();
		_textOpacityPercent = readTextOpacityPercent();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_windowOpacityPercent = readWindowOpacityPercent();
		_textOpacityPercent = readTextOpacityPercent();
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[=](const Plugins::SettingDescriptor &setting) {
				if (setting.id == QStringLiteral("window_opacity")) {
					setWindowOpacityPercent(setting.intValue);
				} else if (setting.id == QStringLiteral("text_opacity")) {
					setTextOpacityPercent(setting.intValue);
				}
			});

		_host->onWindowWidgetCreated([=](QWidget *widget) {
			applyToWindow(widget);
		});

		applyCurrentAppearance();
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
			"Window opacity affects the whole Astrogram window. Text opacity adjusts widget-based text and controls and may be partial in heavily custom-drawn areas.",
			"Прозрачность окна влияет на всё окно Astrogram. Прозрачность текста меняет альфу текста и контролов на виджетах и может работать частично в сильно кастомно отрисованных зонах.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("appearance");
		section.title = PluginText(
			_host,
			"Appearance",
			"Оформление");
		section.settings.push_back(windowSlider);
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
			"Separate transparency controls for windows and text.",
			"Раздельные настройки прозрачности для окна и текста.");
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
		applyWindowOpacity();
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
		applyTextOpacity();
	}

	void applyCurrentAppearance() {
		applyWindowOpacity();
		applyTextOpacity();
	}

	void applyWindowOpacity() const {
		_host->forEachWindowWidget([=](QWidget *widget) {
			if (IsSupportedWindowWidget(widget)) {
				widget->setWindowOpacity(windowOpacityValue());
			}
		});
		if (const auto widget = _host->activeWindowWidget();
			IsSupportedWindowWidget(widget)) {
			widget->setWindowOpacity(windowOpacityValue());
		}
	}

	void applyTextOpacity() {
		_host->forEachWindowWidget([=](QWidget *widget) {
			applyTextOpacityToRoot(widget);
		});
		if (const auto widget = _host->activeWindowWidget()) {
			applyTextOpacityToRoot(widget);
		}
	}

	void applyToWindow(QWidget *widget) {
		if (!IsSupportedWindowWidget(widget)) {
			return;
		}
		widget->setWindowOpacity(windowOpacityValue());
		applyTextOpacityToRoot(widget);
	}

	void applyTextOpacityToRoot(QWidget *root) {
		if (!IsSupportedWindowWidget(root)) {
			return;
		}
		applyTextOpacityToWidget(root);
		for (const auto child : root->findChildren<QWidget*>()) {
			applyTextOpacityToWidget(child);
		}
	}

	void applyTextOpacityToWidget(QWidget *widget) {
		if (!widget) {
			return;
		}
		if (!_originalPalettes.contains(widget)) {
			_originalPalettes.insert(widget, widget->palette());
			QObject::connect(
				widget,
				&QObject::destroyed,
				qApp,
				[this, widget](QObject *) {
					_originalPalettes.remove(widget);
				});
		}
		const auto original = _originalPalettes.value(widget);
		if (_textOpacityPercent >= kMaxOpacityPercent) {
			widget->setPalette(original);
			widget->update();
			return;
		}

		auto adjusted = original;
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
			ApplyAlpha(adjusted, role, alpha);
		}
		widget->setPalette(adjusted);
		widget->update();
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
	int _textOpacityPercent = kDefaultTextOpacityPercent;
	QHash<QWidget*, QPalette> _originalPalettes;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AstroTransparentPlugin(host);
}
