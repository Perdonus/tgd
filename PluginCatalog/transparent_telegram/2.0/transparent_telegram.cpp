/*
Catalog version of the Transparent Telegram plugin.
Uses host-rendered plugin settings instead of a raw plugin-owned dialog.
*/
#include "plugins/plugins_api.h"

#include <QtWidgets/QWidget>

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"example.transparent_telegram",
	"Transparent Telegram",
	"2.0",
	"Codex",
	"Makes Telegram windows transparent with a live settings slider.",
	"",
	"GusTheDuck/4")

namespace {

constexpr int kDefaultOpacityPercent = 85;
constexpr int kMinOpacityPercent = 20;
constexpr int kMaxOpacityPercent = 100;

} // namespace

class TransparentTelegramPlugin final : public Plugins::Plugin {
public:
	explicit TransparentTelegramPlugin(Plugins::Host *host) : _host(host) {
		_info.id = QStringLiteral("example.transparent_telegram");
		_info.name = QStringLiteral("Transparent Telegram");
		_info.version = QStringLiteral("2.0");
		_info.author = QStringLiteral("Codex");
		_info.description = QStringLiteral(
			"Makes Telegram windows transparent with a live settings slider.");
		loadStoredSettings();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[=](const Plugins::SettingDescriptor &setting) {
				if (setting.id == QStringLiteral("opacity")) {
					setOpacityPercent(setting.intValue, true);
				}
			});

		_host->onWindowWidgetCreated([=](QWidget *widget) {
			applyOpacityToWidget(widget);
		});

		applyCurrentOpacity();
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		restoreOpaque();
	}

private:
	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto slider = Plugins::SettingDescriptor();
		slider.id = QStringLiteral("opacity");
		slider.title = QStringLiteral("Window opacity");
		slider.description = QStringLiteral(
			"Applied to Telegram windows immediately. 100% is fully opaque.");
		slider.type = Plugins::SettingControl::IntSlider;
		slider.intValue = _opacityPercent;
		slider.intMinimum = kMinOpacityPercent;
		slider.intMaximum = kMaxOpacityPercent;
		slider.intStep = 1;
		slider.valueSuffix = QStringLiteral("%");

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("hint");
		info.title = QStringLiteral("Only Telegram windows are affected.");
		info.description = QStringLiteral(
			"Menus and plugin-owned dialogs are not used by this plugin.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("appearance");
		section.title = QStringLiteral("Appearance");
		section.settings.push_back(slider);
		section.settings.push_back(info);

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("transparent_telegram");
		page.title = QStringLiteral("Transparency");
		page.description = QStringLiteral(
			"Adjust Telegram window opacity without opening a separate plugin dialog.");
		page.sections.push_back(section);
		return page;
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
		applyCurrentOpacity();
		if (persist) {
			loadStoredSettings();
		}
	}

	void applyCurrentOpacity() {
		_host->forEachWindowWidget([=](QWidget *widget) {
			applyOpacityToWidget(widget);
		});
		if (const auto widget = _host->activeWindowWidget()) {
			applyOpacityToWidget(widget);
		}
	}

	void restoreOpaque() {
		_host->forEachWindowWidget([](QWidget *widget) {
			if (widget && widget->isWindow()) {
				widget->setWindowOpacity(1.0);
			}
		});
	}

	void applyOpacityToWidget(QWidget *widget) const {
		if (!widget || !widget->isWindow()) {
			return;
		}
		widget->setWindowOpacity(opacityValue());
	}

	double opacityValue() const {
		return std::clamp(
			_opacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent) / 100.0;
	}

	void loadStoredSettings() {
		_opacityPercent = kDefaultOpacityPercent;
		const auto value = _host->storedSettingValue(
			_info.id,
			QStringLiteral("opacity"));
		if (value.isDouble()) {
			_opacityPercent = std::clamp(
				value.toInt(),
				kMinOpacityPercent,
				kMaxOpacityPercent);
		}
	}

	Plugins::Host *_host = nullptr;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	int _opacityPercent = kDefaultOpacityPercent;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new TransparentTelegramPlugin(host);
}
