/*
Astrogram accent color plugin.
Applies a custom accent palette to the client.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"astro.accent_color",
	"Accent Color",
	"1.1",
	"@etopizdesblin",
	"Applies a custom accent palette to Astrogram windows and controls.",
	"https://sosiskibot.ru",
	"GusTheDuck/13")

namespace {

constexpr auto kPluginId = "astro.accent_color";
constexpr auto kHexSettingId = "accent_hex";
constexpr auto kSurfaceMixSettingId = "surface_mix";
constexpr auto kResetSettingId = "reset_palette";

constexpr auto kDefaultAccentHex = "#69A1FF";
constexpr int kDefaultSurfaceMix = 18;
constexpr int kMinSurfaceMix = 0;
constexpr int kMaxSurfaceMix = 40;

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
}

QString Utf8(const char *value) {
	return QString::fromUtf8(value);
}

bool UseRussian(const Plugins::Host *host) {
	auto language = host->hostInfo().appUiLanguage.trimmed();
	if (language.isEmpty()) {
		language = host->systemInfo().uiLanguage.trimmed();
	}
	return language.startsWith(QStringLiteral("ru"), Qt::CaseInsensitive);
}

QString Tr(const Plugins::Host *host, const char *en, const char *ru) {
	return UseRussian(host) ? Utf8(ru) : Utf8(en);
}

QColor Mix(const QColor &base, const QColor &accent, int accentPercent) {
	const auto clamped = std::clamp(accentPercent, 0, 100);
	const auto baseWeight = 100 - clamped;
	return QColor(
		(base.red() * baseWeight + accent.red() * clamped) / 100,
		(base.green() * baseWeight + accent.green() * clamped) / 100,
		(base.blue() * baseWeight + accent.blue() * clamped) / 100,
		base.alpha());
}

QColor ParseAccent(QString value) {
	value = value.trimmed();
	if (value.isEmpty()) {
		return QColor(Latin1(kDefaultAccentHex));
	}
	if (!value.startsWith(u'#')) {
		value.prepend(u'#');
	}
	const auto color = QColor(value);
	return color.isValid() ? color : QColor(Latin1(kDefaultAccentHex));
}

} // namespace

class AccentColorPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit AccentColorPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host)
	, _basePalette(QApplication::palette()) {
		_info.id = Latin1(kPluginId);
		_info.name = Tr(_host, "Accent Color", "Акцентный цвет");
		_info.version = QStringLiteral("1.1");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = Tr(
			_host,
			"Applies a custom accent palette to Astrogram windows and controls.",
			"Применяет кастомную accent-палитру к окнам и контролам Astrogram.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_hex = _host->settingStringValue(
			_info.id,
			Latin1(kHexSettingId),
			Latin1(kDefaultAccentHex));
		_surfaceMix = std::clamp(
			_host->settingIntValue(
				_info.id,
				Latin1(kSurfaceMixSettingId),
				kDefaultSurfaceMix),
			kMinSurfaceMix,
			kMaxSurfaceMix);
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSetting(setting);
			});
		schedulePaletteApply();
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		restorePalette();
	}

private:
	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto hex = Plugins::SettingDescriptor();
		hex.id = Latin1(kHexSettingId);
		hex.title = Tr(_host, "Accent color", "Accent цвет");
		hex.description = Tr(
			_host,
			"Use a hex color like #69A1FF. The palette updates live.",
			"Используй hex-цвет вроде #69A1FF. Палитра меняется сразу.");
		hex.type = Plugins::SettingControl::TextInput;
		hex.textValue = _hex;
		hex.placeholderText = Latin1(kDefaultAccentHex);

		auto mix = Plugins::SettingDescriptor();
		mix.id = Latin1(kSurfaceMixSettingId);
		mix.title = Tr(_host, "Surface tint", "Тонировка поверхностей");
		mix.description = Tr(
			_host,
			"How much the accent color mixes into buttons, highlight surfaces and secondary backgrounds.",
			"Насколько сильно accent-цвет подмешивается в кнопки, подсветку и вторичные фоны.");
		mix.type = Plugins::SettingControl::IntSlider;
		mix.intValue = _surfaceMix;
		mix.intMinimum = kMinSurfaceMix;
		mix.intMaximum = kMaxSurfaceMix;
		mix.intStep = 1;
		mix.valueSuffix = QStringLiteral("%");

		auto reset = Plugins::SettingDescriptor();
		reset.id = Latin1(kResetSettingId);
		reset.title = Tr(_host, "Reset palette", "Сбросить палитру");
		reset.description = Tr(
			_host,
			"Restore the original Astrogram palette.",
			"Вернуть исходную палитру Astrogram.");
		reset.type = Plugins::SettingControl::ActionButton;
		reset.buttonText = Tr(_host, "Reset", "Сбросить");

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("info");
		info.title = Tr(_host, "How it works", "Как это работает");
		info.description = Tr(
			_host,
			"Accent Color changes the shared Qt palette, so native custom-painted areas may still keep the client default colors.",
			"Accent Color меняет общую Qt-палитру, поэтому кастомно отрисованные нативные зоны клиента могут сохранить дефолтные цвета.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("accent");
		section.title = Tr(_host, "Palette", "Палитра");
		section.settings = { hex, mix, reset, info };

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("accent_color");
		page.title = Tr(_host, "Accent Color", "Accent Color");
		page.description = Tr(
			_host,
			"Accent palette controls for Astrogram.",
			"Настройка accent-палитры для Astrogram.");
		page.sections.push_back(section);
		return page;
	}

	void handleSetting(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kHexSettingId)) {
			_hex = setting.textValue.trimmed();
			_restoreRequested = false;
			schedulePaletteApply();
		} else if (setting.id == Latin1(kSurfaceMixSettingId)) {
			_surfaceMix = std::clamp(
				setting.intValue,
				kMinSurfaceMix,
				kMaxSurfaceMix);
			_restoreRequested = false;
			schedulePaletteApply();
		} else if (setting.id == Latin1(kResetSettingId)) {
			_hex = Latin1(kDefaultAccentHex);
			_surfaceMix = kDefaultSurfaceMix;
			_restoreRequested = true;
			schedulePaletteApply();
		}
	}

	void schedulePaletteApply() {
		if (_applyScheduled) {
			return;
		}
		_applyScheduled = true;
		QTimer::singleShot(0, this, [this] {
			_applyScheduled = false;
			if (_restoreRequested) {
				restorePalette();
				return;
			}
			applyPaletteNow();
		});
	}

	void applyPaletteNow() {
		auto accent = ParseAccent(_hex);
		auto palette = _basePalette;

		palette.setColor(QPalette::Highlight, accent);
		palette.setColor(QPalette::Link, accent);
		palette.setColor(QPalette::LinkVisited, accent.darker(112));
		palette.setColor(QPalette::Button, Mix(
			_basePalette.color(QPalette::Button),
			accent,
			_surfaceMix));
		palette.setColor(QPalette::AlternateBase, Mix(
			_basePalette.color(QPalette::AlternateBase),
			accent,
			std::min(_surfaceMix + 4, 60)));
		palette.setColor(QPalette::Base, Mix(
			_basePalette.color(QPalette::Base),
			accent,
			std::min(_surfaceMix / 2, 20)));
		palette.setColor(QPalette::ToolTipBase, Mix(
			_basePalette.color(QPalette::ToolTipBase),
			accent,
			std::min(_surfaceMix + 8, 70)));

		const auto highlightText = accent.lightness() > 160
			? QColor(Qt::black)
			: QColor(Qt::white);
		palette.setColor(QPalette::HighlightedText, highlightText);
		palette.setColor(QPalette::ButtonText, _basePalette.color(QPalette::ButtonText));
		palette.setColor(QPalette::BrightText, highlightText);

		QApplication::setPalette(palette);
		_host->forEachWindowWidget([](QWidget *widget) {
			if (widget && widget->isWindow()) {
				widget->update();
			}
		});
	}

	void restorePalette() {
		_restoreRequested = false;
		QApplication::setPalette(_basePalette);
		_host->forEachWindowWidget([](QWidget *widget) {
			if (widget && widget->isWindow()) {
				widget->update();
			}
		});
	}

	Plugins::Host *_host = nullptr;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	QPalette _basePalette;
	QString _hex = Latin1(kDefaultAccentHex);
	int _surfaceMix = kDefaultSurfaceMix;
	bool _applyScheduled = false;
	bool _restoreRequested = false;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AccentColorPlugin(host);
}
