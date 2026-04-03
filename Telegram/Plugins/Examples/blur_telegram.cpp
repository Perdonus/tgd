/*
Astrogram blur plugin.
Applies a configurable blur to major Telegram surfaces.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsBlurEffect>
#include <QtWidgets/QWidget>

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"astro.blur_telegram",
	"Blur Telegram",
	"1.1",
	"@etopizdesblin",
	"Applies a soft blur to Astrogram chat and dialog surfaces.",
	"https://sosiskibot.ru",
	"GusTheDuck/12")

namespace {

constexpr auto kPluginId = "astro.blur_telegram";
constexpr auto kEnabledSettingId = "enabled";
constexpr auto kRadiusSettingId = "blur_radius";
constexpr auto kExpandedTargetsSettingId = "expanded_targets";

constexpr int kDefaultRadius = 14;
constexpr int kMinRadius = 0;
constexpr int kMaxRadius = 36;

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
}

QString Utf8(const char *value) {
	return QString::fromUtf8(value);
}

QString Utf8(const char8_t *value) {
	return QString::fromUtf8(reinterpret_cast<const char*>(value));
}

bool UseRussian(const Plugins::Host *host) {
	auto language = host->hostInfo().appUiLanguage.trimmed();
	if (language.isEmpty()) {
		language = host->systemInfo().uiLanguage.trimmed();
	}
	return language.startsWith(QStringLiteral("ru"), Qt::CaseInsensitive);
}

QString Tr(const Plugins::Host *host, const char *en, const char8_t *ru) {
	return UseRussian(host) ? Utf8(ru) : Latin1(en);
}

QString Tr(const Plugins::Host *host, const char *en, const char *ru) {
	return UseRussian(host) ? Utf8(ru) : Latin1(en);
}

bool IsSupportedWindowWidget(QWidget *widget) {
	if (!widget || !widget->isWindow() || widget->parentWidget()) {
		return false;
	}
	return widget->windowType() == Qt::Window
		&& !widget->testAttribute(Qt::WA_DontShowOnScreen);
}

bool ContainsAny(const QString &value, std::initializer_list<const char*> needles) {
	for (const auto *needle : needles) {
		if (value.contains(QString::fromLatin1(needle))) {
			return true;
		}
	}
	return false;
}

bool IsNeverBlurWidget(QWidget *widget) {
	if (!widget) {
		return true;
	}
	const auto objectName = widget->objectName().toLower();
	const auto className = QByteArray(widget->metaObject()->className()).toLower();
	return ContainsAny(objectName, {
			"tooltip",
			"menu",
			"button",
			"input",
			"edit",
			"lineedit",
			"scrollbar",
			"slider",
			"checkbox",
			"radiobutton",
			"switch",
			"emoji",
			"sticker",
		}) || ContainsAny(QString::fromLatin1(className), {
			"button",
			"input",
			"lineedit",
			"checkbox",
			"radiobutton",
			"tooltip",
			"menu",
			"slider",
			"scrollbar",
		});
}

bool IsPrimaryBlurSurface(QWidget *widget) {
	if (!widget || widget->isWindow()) {
		return false;
	}
	const auto objectName = widget->objectName().toLower();
	const auto className = QString::fromLatin1(widget->metaObject()->className()).toLower();
	if (IsNeverBlurWidget(widget)) {
		return false;
	}
	return ContainsAny(objectName, {
			"history",
			"message",
			"chat",
			"dialogs",
			"dialogslist",
			"overview",
			"media",
		}) || ContainsAny(className, {
			"history",
			"message",
			"chat",
			"dialogs",
			"overview",
			"media",
		});
}

bool IsExpandedBlurSurface(QWidget *widget) {
	if (!widget || widget->isWindow()) {
		return false;
	}
	const auto objectName = widget->objectName().toLower();
	const auto className = QString::fromLatin1(widget->metaObject()->className()).toLower();
	if (IsNeverBlurWidget(widget)) {
		return false;
	}
	return IsPrimaryBlurSurface(widget)
		|| ContainsAny(objectName, {
			"sidebar",
			"info",
			"wrap",
			"section",
			"list",
		}) || ContainsAny(className, {
			"sidebar",
			"section",
			"list",
		});
}

} // namespace

class BlurTelegramPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit BlurTelegramPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host) {
		_info.id = Latin1(kPluginId);
		_info.name = Tr(_host, "Blur Telegram", u8"Размытие Telegram");
		_info.version = QStringLiteral("1.1");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = Tr(
			_host,
			"Applies a soft blur to Astrogram chat and dialog surfaces.",
			u8"Применяет мягкое размытие поверхностей чатов и списков Astrogram.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_enabled = _host->settingBoolValue(
			_info.id,
			Latin1(kEnabledSettingId),
			true);
		_radius = std::clamp(
			_host->settingIntValue(
				_info.id,
				Latin1(kRadiusSettingId),
				kDefaultRadius),
			kMinRadius,
			kMaxRadius);
		_expandedTargets = _host->settingBoolValue(
			_info.id,
			Latin1(kExpandedTargetsSettingId),
			false);
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
		_host->onWindowWidgetCreated([=](QWidget *widget) {
			applyToWindow(widget);
		});
		applyEverywhere();
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		clearEffects();
	}

private:
	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto enabled = Plugins::SettingDescriptor();
		enabled.id = Latin1(kEnabledSettingId);
		enabled.title = Tr(_host, "Enable blur", u8"Включить размытие");
		enabled.description = Tr(
			_host,
			"Turns the blur pass on or off without uninstalling the plugin.",
			u8"Включает или выключает эффект размытия без удаления плагина.");
		enabled.type = Plugins::SettingControl::Toggle;
		enabled.boolValue = _enabled;

		auto radius = Plugins::SettingDescriptor();
		radius.id = Latin1(kRadiusSettingId);
		radius.title = Tr(_host, "Blur radius", u8"Радиус размытия");
		radius.description = Tr(
			_host,
			"Controls how strong the blur effect is on supported Astrogram surfaces.",
			u8"Управляет силой эффекта размытия на поддерживаемых поверхностях Astrogram.");
		radius.type = Plugins::SettingControl::IntSlider;
		radius.intValue = _radius;
		radius.intMinimum = kMinRadius;
		radius.intMaximum = kMaxRadius;
		radius.intStep = 1;
		radius.valueSuffix = QStringLiteral(" px");

		auto expanded = Plugins::SettingDescriptor();
		expanded.id = Latin1(kExpandedTargetsSettingId);
		expanded.title = Tr(_host, "Blur more surfaces", u8"Размывать больше поверхностей");
		expanded.description = Tr(
			_host,
			"Also touches sidebars and extra sections, not only history/dialog areas.",
			u8"Дополнительно затрагивает боковые панели и вторичные секции, а не только историю и список диалогов.");
		expanded.type = Plugins::SettingControl::Toggle;
		expanded.boolValue = _expandedTargets;

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("info");
		info.title = Tr(_host, "How it works", u8"Как это работает");
		info.description = Tr(
			_host,
			"Blur Telegram works best on large chat/history containers. Interactive controls are intentionally skipped for stability.",
			u8"Blur Telegram лучше всего работает на крупных контейнерах истории и чатов. Интерактивные элементы специально пропускаются ради стабильности.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("blur");
		section.title = Tr(_host, "Blur", u8"Размытие");
		section.settings = { enabled, radius, expanded, info };

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("blur_telegram");
		page.title = Tr(_host, "Blur Telegram", u8"Размытие Telegram");
		page.description = Tr(
			_host,
			"Soft blur for Astrogram without rebuilding the client.",
			u8"Мягкое размытие для Astrogram без перебилда клиента.");
		page.sections.push_back(section);
		return page;
	}

	void handleSetting(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kEnabledSettingId)) {
			_enabled = setting.boolValue;
			applyEverywhere();
		} else if (setting.id == Latin1(kRadiusSettingId)) {
			_radius = std::clamp(setting.intValue, kMinRadius, kMaxRadius);
			applyEverywhere();
		} else if (setting.id == Latin1(kExpandedTargetsSettingId)) {
			_expandedTargets = setting.boolValue;
			applyEverywhere();
		}
	}

	bool shouldBlur(QWidget *widget) const {
		return _expandedTargets
			? IsExpandedBlurSurface(widget)
			: IsPrimaryBlurSurface(widget);
	}

	void applyEverywhere() {
		clearEffects();
		if (!_enabled || _radius <= 0) {
			return;
		}
		_host->forEachWindowWidget([=](QWidget *widget) {
			applyToWindow(widget);
		});
	}

	void applyToWindow(QWidget *window) {
		if (!_enabled || _radius <= 0 || !IsSupportedWindowWidget(window)) {
			return;
		}
		applyToWidget(window);
		const auto children = window->findChildren<QWidget*>();
		for (auto *child : children) {
			applyToWidget(child);
		}
	}

	void applyToWidget(QWidget *widget) {
		if (!shouldBlur(widget)) {
			return;
		}
		auto existing = widget->graphicsEffect();
		if (existing && !_effects.contains(widget)) {
			return;
		}
		auto effect = _effects.value(widget);
		if (!effect) {
			effect = new QGraphicsBlurEffect(widget);
			effect->setBlurHints(QGraphicsBlurEffect::QualityHint);
			widget->setGraphicsEffect(effect);
			_effects.insert(widget, effect);
		}
		effect->setBlurRadius(_radius);
		widget->update();
	}

	void clearEffects() {
		for (auto it = _effects.begin(); it != _effects.end(); ++it) {
			auto *widget = it.key();
			auto effect = it.value();
			if (widget && effect && widget->graphicsEffect() == effect) {
				widget->setGraphicsEffect(nullptr);
				widget->update();
			}
			if (effect) {
				effect->deleteLater();
			}
		}
		_effects.clear();
	}

	Plugins::Host *_host = nullptr;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	bool _enabled = true;
	int _radius = kDefaultRadius;
	bool _expandedTargets = false;
	QHash<QWidget*, QPointer<QGraphicsBlurEffect>> _effects;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new BlurTelegramPlugin(host);
}
