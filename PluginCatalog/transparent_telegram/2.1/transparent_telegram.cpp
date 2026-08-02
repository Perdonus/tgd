/*
Catalog migration version of the Transparent Telegram plugin.
Uses host-managed settings and window callbacks instead of raw plugin dialogs.
*/
#include "plugins/plugins_api.h"

#include <QtWidgets/QWidget>

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"example.transparent_telegram",
	"Transparent Telegram",
	"2.1",
	"Codex",
	"Makes Telegram windows transparent with a host-managed opacity slider.",
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
		_info.version = QStringLiteral("2.1");
		_info.author = QStringLiteral("Codex");
		_info.description = QStringLiteral(
			"Makes Telegram windows transparent with a host-managed opacity slider.");
		_opacityPercent = readOpacityPercent();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_opacityPercent = readOpacityPercent();
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[=](const Plugins::SettingDescriptor &setting) {
				if (setting.id == QStringLiteral("opacity")) {
					setOpacityPercent(setting.intValue);
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
		info.id = QStringLiteral("migration_note");
		info.title = QStringLiteral("Host-managed migration");
		info.description = QStringLiteral(
			"This version does not open a plugin-owned dialog. "
			"It relies on the client exposing plugin settings pages and window widget callbacks.");
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
			"Adjust Telegram window opacity without a separate plugin popup.");
		page.sections.push_back(section);
		return page;
	}

	void setOpacityPercent(int value) {
		const auto clamped = std::clamp(
			value,
			kMinOpacityPercent,
			kMaxOpacityPercent);
		if (_opacityPercent == clamped) {
			return;
		}
		_opacityPercent = clamped;
		applyCurrentOpacity();
	}

	void applyCurrentOpacity() const {
		_host->forEachWindowWidget([=](QWidget *widget) {
			applyOpacityToWidget(widget);
		});
		if (const auto widget = _host->activeWindowWidget()) {
			applyOpacityToWidget(widget);
		}
	}

	void restoreOpaque() const {
		_host->forEachWindowWidget([](QWidget *widget) {
			if (isSupportedWindowWidget(widget)) {
				widget->setWindowOpacity(1.0);
			}
		});
		if (const auto widget = _host->activeWindowWidget();
			isSupportedWindowWidget(widget)) {
			widget->setWindowOpacity(1.0);
		}
	}

	void applyOpacityToWidget(QWidget *widget) const {
		if (!isSupportedWindowWidget(widget)) {
			return;
		}
		widget->setWindowOpacity(opacityValue());
	}

	static bool isSupportedWindowWidget(QWidget *widget) {
		if (!widget || !widget->isWindow() || widget->parentWidget()) {
			return false;
		}
		// The current host API exposes QWidget* only, so the safest filter here is
		// to touch real top-level Qt::Window widgets and skip transient/tool windows.
		return widget->windowType() == Qt::Window
			&& !widget->testAttribute(Qt::WA_DontShowOnScreen);
	}

	double opacityValue() const {
		return std::clamp(
			_opacityPercent,
			kMinOpacityPercent,
			kMaxOpacityPercent) / 100.0;
	}

	int readOpacityPercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				QStringLiteral("opacity"),
				kDefaultOpacityPercent),
			kMinOpacityPercent,
			kMaxOpacityPercent);
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
