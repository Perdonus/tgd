/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_plugins.h"
#include "settings/settings_common.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "plugins/plugins_manager.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace Settings {
namespace {

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

QString FormatPluginTitle(const ::Plugins::PluginState &state) {
	const auto &info = state.info;
	const auto name = !info.name.isEmpty()
		? info.name
		: (!info.id.isEmpty()
			? info.id
			: PluginUiText(u"Plugin"_q, u"Плагин"_q));
	const auto version = info.version.trimmed();
	return version.isEmpty() ? name : (name + u" "_q + version);
}

QString PluginDocsText() {
	return UseRussianPluginUi()
		? QString::fromUtf8(R"PLUGIN(Плагины Telegram Desktop (техническая документация)

0) Кратко про архитектуру
- Плагин = нативная библиотека .tgd, загружается в процесс Telegram Desktop.
- Любая ошибка ABI или native-crash в плагине может уронить процесс.
- Менеджер плагинов ведёт recovery-state и может включить safe mode.

1) Пути и файлы
- Папка плагинов: <working dir>/tdata/plugins
- Ручные выключения: <working dir>/tdata/plugins.json
- Флаг safe mode: <working dir>/tdata/plugins.safe-mode
- Основной лог: <working dir>/tdata/plugins.log
- Recovery-state: <working dir>/tdata/plugins.recovery.json

2) Минимальный каркас плагина
```cpp
#include "plugins/plugins_api.h"

class MyPlugin final : public Plugins::Plugin {
public:
	explicit MyPlugin(Plugins::Host *host) : _host(host) {}
	Plugins::PluginInfo info() const override {
		return {
			.id = "example.my_plugin",
			.name = "My Plugin",
			.version = "1.0.0",
			.author = "You",
			.description = "Example plugin",
		};
	}
	void onLoad() override {}
	void onUnload() override {}
private:
	Plugins::Host *_host = nullptr;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) return nullptr;
	return new MyPlugin(host);
}
```

3) Preview metadata (без запуска кода плагина)
```cpp
TGD_PLUGIN_PREVIEW(
	"example.my_plugin",
	"My Plugin",
	"1.0.0",
	"You",
	"Example plugin",
	"https://example.com",
	"GusTheDuck/4")
```

4) Регистрация slash-команды
```cpp
_commandId = _host->registerCommand(
	"example.my_plugin",
	{ .command = "/ping", .description = "Ping command" },
	[=](const Plugins::CommandContext &ctx) {
		_host->showToast("pong");
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Cancel
		};
	});
```

5) Кнопка-действие в Settings > Plugins
```cpp
_actionId = _host->registerAction(
	"example.my_plugin",
	"Open popup",
	"Opens a toast",
	[=] { _host->showToast("Action called"); });
```

6) Action с контекстом (окно/сессия)
```cpp
_actionCtxId = _host->registerActionWithContext(
	"example.my_plugin",
	"Context action",
	"Uses active window/session",
	[=](const Plugins::ActionContext &ctx) {
		if (!ctx.window) return;
		_host->showToast("Window is available");
	});
```

7) Host-rendered settings page
```cpp
Plugins::SettingDescriptor slider;
slider.id = "opacity";
slider.title = "Window opacity";
slider.type = Plugins::SettingControl::IntSlider;
slider.intValue = 85;
slider.intMinimum = 20;
slider.intMaximum = 100;
slider.intStep = 1;
slider.valueSuffix = "%";

Plugins::SettingsSectionDescriptor section;
section.id = "appearance";
section.title = "Appearance";
section.settings.push_back(slider);

_settingsPageId = _host->registerSettingsPage(
	"example.my_plugin",
	{ .id = "my_plugin", .title = "My Plugin", .sections = { section } },
	[=](const Plugins::SettingDescriptor &setting) {
		if (setting.id == "opacity") {
			_host->showToast(QString::number(setting.intValue));
		}
	});
```

Старый `registerPanel()` всё ещё доступен для legacy UI, но сырой plugin-owned dialog значительно менее надёжен, чем host-rendered controls на странице Settings > Plugins.

8) Перехват исходящего текста
```cpp
_outgoingId = _host->registerOutgoingTextInterceptor(
	"example.my_plugin",
	[=](const Plugins::OutgoingTextContext &ctx) {
		if (ctx.text.startsWith("/shout ")) {
			_host->showToast("Intercepted");
			return Plugins::CommandResult{
				.action = Plugins::CommandResult::Action::Cancel
			};
		}
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Continue
		};
	},
	/*priority=*/100);
```

9) Observer новых/изменённых/удалённых сообщений
```cpp
Plugins::MessageObserverOptions opts;
opts.newMessages = true;
opts.editedMessages = true;
opts.deletedMessages = true;
opts.incoming = true;
opts.outgoing = true;

_observerId = _host->registerMessageObserver(
	"example.my_plugin",
	opts,
	[=](const Plugins::MessageEventContext &ctx) {
		switch (ctx.event) {
		case Plugins::MessageEvent::New: _host->showToast("New"); break;
		case Plugins::MessageEvent::Edited: _host->showToast("Edited"); break;
		case Plugins::MessageEvent::Deleted: _host->showToast("Deleted"); break;
		}
	});
```

10) Window/session callbacks
```cpp
_host->onWindowCreated([=](Window::Controller *window) {
	Q_UNUSED(window);
	_host->showToast("Window created");
});

_host->onWindowWidgetCreated([=](QWidget *widget) {
	if (widget && widget->isWindow()) {
		widget->setWindowOpacity(0.85);
	}
});

_host->onSessionActivated([=](Main::Session *session) {
	Q_UNUSED(session);
	_host->showToast("Session activated");
});
```

11) Корректная очистка в onUnload
```cpp
void onUnload() override {
	if (_commandId) _host->unregisterCommand(_commandId);
	if (_actionId) _host->unregisterAction(_actionId);
	if (_panelId) _host->unregisterPanel(_panelId);
	if (_outgoingId) _host->unregisterOutgoingTextInterceptor(_outgoingId);
	if (_observerId) _host->unregisterMessageObserver(_observerId);
}
```

12) CMake пример для плагина
```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

13) ABI/совместимость (обязательно)
- platform, pointer size, compiler ABI, Qt major/minor, plugin API version должны совпадать.
- Простое переименование файла в `.tgd` не работает: нужен реальный compile+link.
- Несовместимость пишется в plugins.log как load-failed/abi-mismatch.

14) Safe mode и recovery
- При падении в рискованной операции (load/onload/panel/command/window/session/observer...) менеджер включает safe mode.
- Подозрительный плагин выключается автоматически.
- На следующем запуске появляется recovery-уведомление.

15) Диагностика: что смотреть сначала
1. plugins.log: `load-failed`, `abi-mismatch`, `onload failed`, `panel failed`.
2. Путь и SHA пакета в логе.
3. Совпадение compiler + Qt.
4. Не храните long-lived сырые указатели на объекты Telegram.
5. Уберите тяжёлую синхронную работу из callback'ов UI.

16) Практика надёжности
- `info()` и конструктор плагина должны быть дешёвыми и без I/O.
- Любые сетевые/тяжёлые операции уносите в worker.
- UI код открывайте только из UI callback'ов.
- Всегда тестируйте onUnload после reload/disable.
)PLUGIN")
		: QString::fromUtf8(R"PLUGIN(Telegram Desktop Plugins (technical documentation)

0) Architecture
- Plugin = native .tgd shared library loaded into Telegram Desktop process.
- ABI mismatch or native crash in plugin code can crash the process.
- Plugin manager stores recovery-state and can auto-enable safe mode.

1) Paths
- Plugin folder: <working dir>/tdata/plugins
- Manual disable list: <working dir>/tdata/plugins.json
- Safe mode flag: <working dir>/tdata/plugins.safe-mode
- Main log: <working dir>/tdata/plugins.log
- Recovery state: <working dir>/tdata/plugins.recovery.json

2) Minimal plugin skeleton
```cpp
#include "plugins/plugins_api.h"

class MyPlugin final : public Plugins::Plugin {
public:
	explicit MyPlugin(Plugins::Host *host) : _host(host) {}
	Plugins::PluginInfo info() const override {
		return {
			.id = "example.my_plugin",
			.name = "My Plugin",
			.version = "1.0.0",
			.author = "You",
			.description = "Example plugin",
		};
	}
	void onLoad() override {}
	void onUnload() override {}
private:
	Plugins::Host *_host = nullptr;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) return nullptr;
	return new MyPlugin(host);
}
```

3) Preview metadata export
```cpp
TGD_PLUGIN_PREVIEW(
	"example.my_plugin",
	"My Plugin",
	"1.0.0",
	"You",
	"Example plugin",
	"https://example.com",
	"GusTheDuck/4")
```

4) Slash command
```cpp
_commandId = _host->registerCommand(
	"example.my_plugin",
	{ .command = "/ping", .description = "Ping command" },
	[=](const Plugins::CommandContext &ctx) {
		_host->showToast("pong");
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Cancel
		};
	});
```

5) Action button
```cpp
_actionId = _host->registerAction(
	"example.my_plugin",
	"Open popup",
	"Opens a toast",
	[=] { _host->showToast("Action called"); });
```

6) Action with context
```cpp
_actionCtxId = _host->registerActionWithContext(
	"example.my_plugin",
	"Context action",
	"Uses active window/session",
	[=](const Plugins::ActionContext &ctx) {
		if (!ctx.window) return;
		_host->showToast("Window is available");
	});
```

7) Host-rendered settings page
```cpp
Plugins::SettingDescriptor slider;
slider.id = "opacity";
slider.title = "Window opacity";
slider.type = Plugins::SettingControl::IntSlider;
slider.intValue = 85;
slider.intMinimum = 20;
slider.intMaximum = 100;
slider.intStep = 1;
slider.valueSuffix = "%";

Plugins::SettingsSectionDescriptor section;
section.id = "appearance";
section.title = "Appearance";
section.settings.push_back(slider);

_settingsPageId = _host->registerSettingsPage(
	"example.my_plugin",
	{ .id = "my_plugin", .title = "My Plugin", .sections = { section } },
	[=](const Plugins::SettingDescriptor &setting) {
		if (setting.id == "opacity") {
			_host->showToast(QString::number(setting.intValue));
		}
	});
```

Legacy `registerPanel()` is still available for plugin-owned UI, but raw native dialogs are less stable than host-rendered controls in Settings > Plugins.

8) Outgoing text interceptor
```cpp
_outgoingId = _host->registerOutgoingTextInterceptor(
	"example.my_plugin",
	[=](const Plugins::OutgoingTextContext &ctx) {
		if (ctx.text.startsWith("/shout ")) {
			_host->showToast("Intercepted");
			return Plugins::CommandResult{
				.action = Plugins::CommandResult::Action::Cancel
			};
		}
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Continue
		};
	},
	/*priority=*/100);
```

9) Message observer
```cpp
Plugins::MessageObserverOptions opts;
opts.newMessages = true;
opts.editedMessages = true;
opts.deletedMessages = true;
opts.incoming = true;
opts.outgoing = true;

_observerId = _host->registerMessageObserver(
	"example.my_plugin",
	opts,
	[=](const Plugins::MessageEventContext &ctx) {
		switch (ctx.event) {
		case Plugins::MessageEvent::New: _host->showToast("New"); break;
		case Plugins::MessageEvent::Edited: _host->showToast("Edited"); break;
		case Plugins::MessageEvent::Deleted: _host->showToast("Deleted"); break;
		}
	});
```

10) Window/session callbacks
```cpp
_host->onWindowCreated([=](Window::Controller *window) {
	Q_UNUSED(window);
	_host->showToast("Window created");
});

_host->onWindowWidgetCreated([=](QWidget *widget) {
	if (widget && widget->isWindow()) {
		widget->setWindowOpacity(0.85);
	}
});

_host->onSessionActivated([=](Main::Session *session) {
	Q_UNUSED(session);
	_host->showToast("Session activated");
});
```

11) onUnload cleanup
```cpp
void onUnload() override {
	if (_commandId) _host->unregisterCommand(_commandId);
	if (_actionId) _host->unregisterAction(_actionId);
	if (_panelId) _host->unregisterPanel(_panelId);
	if (_outgoingId) _host->unregisterOutgoingTextInterceptor(_outgoingId);
	if (_observerId) _host->unregisterMessageObserver(_observerId);
}
```

12) CMake sample
```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

13) ABI checklist
- Match platform, architecture, compiler ABI, Qt major/minor, plugin API version.
- Renaming files to `.tgd` is not enough; you must compile/link.
- ABI failures are logged as load-failed/abi-mismatch in plugins.log.

14) Recovery/safe mode
- If Telegram detects crash risk in plugin operation, it enables safe mode.
- Suspected plugin is disabled automatically.
- Recovery notice appears on next start.

15) Debug order
1. Check plugins.log (`load-failed`, `abi-mismatch`, `onload failed`, `panel failed`).
2. Verify package path and SHA in log.
3. Verify compiler + Qt match.
4. Avoid long-lived raw pointers to Telegram internals.
5. Move heavy sync work out of UI callbacks.
)PLUGIN");
}

void ShowPluginDocsBox(not_null<Window::SessionController*> controller) {
	const auto text = PluginDocsText();
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			PluginUiText(u"Plugin Documentation"_q, u"Документация плагинов"_q)));
		box->addLeftButton(
			rpl::single(PluginUiText(u"Copy"_q, u"Копировать"_q)),
			[=] {
				if (const auto clipboard = QGuiApplication::clipboard()) {
					clipboard->setText(text);
				}
				controller->window().showToast(PluginUiText(
					u"Documentation copied."_q,
					u"Документация скопирована."_q));
			});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				0),
			style::al_top);
	}));
}

QString FormatPluginSummary(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	const auto &info = state.info;
	if (!info.author.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Author: "_q, u"Автор: "_q)
			+ info.author.trimmed());
	}
	if (!info.version.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Version: "_q, u"Версия: "_q)
			+ info.version.trimmed());
	}
	if (!info.description.trimmed().isEmpty()) {
		lines.push_back(info.description.trimmed());
	}
	return lines.join(u"\n"_q);
}

QString FormatPluginStateNote(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	if (state.disabledByRecovery) {
		lines.push_back(PluginUiText(
			u"Disabled automatically after a recovery event."_q,
			u"Автоматически выключен после recovery-события."_q));
	}
	if (!state.recoveryReason.trimmed().isEmpty()) {
		lines.push_back(state.recoveryReason.trimmed());
	}
	if (!state.error.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Error: "_q, u"Ошибка: "_q)
			+ state.error.trimmed());
	}
	if (!state.path.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Package: "_q, u"Пакет: "_q)
			+ QDir::toNativeSeparators(state.path));
	}
	return lines.join(u"\n"_q);
}

std::optional<::Plugins::PluginState> LookupPluginState(
		const QString &pluginId) {
	for (const auto &state : Core::App().plugins().plugins()) {
		if (state.info.id == pluginId) {
			return state;
		}
	}
	return std::nullopt;
}

void SharePluginPackage(
		not_null<Window::SessionController*> controller,
		const ::Plugins::PluginState &state) {
	if (state.path.trimmed().isEmpty()) {
		controller->window().showToast(PluginUiText(
			u"Plugin file path is unavailable."_q,
			u"Путь к файлу плагина недоступен."_q));
		return;
	}
	if (const auto clipboard = QGuiApplication::clipboard()) {
		clipboard->setText(QDir::toNativeSeparators(state.path));
	}
	File::ShowInFolder(state.path);
	controller->window().showToast(PluginUiText(
		u"Plugin package path copied and file revealed."_q,
		u"Путь к пакету плагина скопирован, файл показан."_q));
}

void RequestSafeModeChange(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> context,
		bool enabled,
		Fn<void()> onStateChanged) {
	const auto title = enabled
		? PluginUiText(u"Enable plugin safe mode?"_q, u"Включить безопасный режим плагинов?"_q)
		: PluginUiText(u"Disable plugin safe mode?"_q, u"Выключить безопасный режим плагинов?"_q);
	const auto text = enabled
		? PluginUiText(
			u"Telegram will unload all plugins and reopen the plugin list in metadata-only mode."_q,
			u"Telegram выгрузит все плагины и откроет список плагинов в режиме только-метаданных."_q)
		: PluginUiText(
			u"Telegram will try to load plugins again. Disable safe mode only if you trust the installed plugins."_q,
			u"Telegram снова попробует загрузить плагины. Выключайте безопасный режим только если доверяете установленным плагинам."_q);
	controller->show(Ui::MakeConfirmBox({
		.text = title + u"\n\n"_q + text,
		.confirmed = crl::guard(context, [=] {
			QTimer::singleShot(0, context, [=] {
				if (!Core::App().plugins().setSafeModeEnabled(enabled)) {
					controller->window().showToast(PluginUiText(
						u"Could not change safe mode."_q,
						u"Не удалось переключить безопасный режим."_q));
				}
				if (onStateChanged) {
					onStateChanged();
				}
			});
		}),
		.confirmText = enabled
			? PluginUiText(u"Enable"_q, u"Включить"_q)
			: PluginUiText(u"Disable"_q, u"Выключить"_q),
	}));
}

void AddPluginSettingsContent(
		not_null<Ui::VerticalLayout*> container,
		const ::Plugins::SettingsPageState &page,
		Fn<void()> onStateChanged) {
	if (!page.title.trimmed().isEmpty()) {
		Ui::AddSubsectionTitle(container, rpl::single(page.title.trimmed()));
	}
	if (!page.description.trimmed().isEmpty()) {
		Ui::AddDividerText(container, rpl::single(page.description.trimmed()));
	}
	for (const auto &section : page.sections) {
		if (!section.title.trimmed().isEmpty()) {
			Ui::AddSkip(container);
			Ui::AddSubsectionTitle(container, rpl::single(section.title.trimmed()));
		}
		if (!section.description.trimmed().isEmpty()) {
			Ui::AddDividerText(container, rpl::single(section.description.trimmed()));
		}
		for (const auto &setting : section.settings) {
			switch (setting.type) {
			case ::Plugins::SettingControl::Toggle: {
				const auto button = container->add(object_ptr<Ui::SettingsButton>(
					container,
					rpl::single(setting.title),
					st::settingsButtonNoIcon
				))->toggleOn(rpl::single(setting.boolValue));
				button->toggledChanges(
				) | rpl::filter([=](bool value) {
					return value != setting.boolValue;
				}) | rpl::on_next([=](bool value) {
					auto updated = setting;
					updated.boolValue = value;
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				}, button->lifetime());
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::IntSlider: {
				if (!setting.title.trimmed().isEmpty()) {
					Ui::AddSkip(container);
					Ui::AddSubsectionTitle(
						container,
						rpl::single(setting.title.trimmed()));
				}
				const auto minValue = std::min(
					setting.intMinimum,
					setting.intMaximum);
				const auto maxValue = std::max(
					setting.intMinimum,
					setting.intMaximum);
				const auto step = std::max(1, setting.intStep);
				const auto formatValue = [=](int value) {
					return QString::number(value) + setting.valueSuffix;
				};
				auto sliderWithLabel = MakeSliderWithLabel(
					container,
					st::settingsScale,
					st::settingsScaleLabel,
					st::normalFont->spacew * 2,
					std::max(
						st::settingsScaleLabel.style.font->width(
							formatValue(minValue)),
						st::settingsScaleLabel.style.font->width(
							formatValue(maxValue))),
					true);
				const auto slider = sliderWithLabel.slider;
				const auto valueLabel = sliderWithLabel.label;
				slider->setAccessibleName(setting.title);
				const auto valueFromSlider = [=](double raw) {
					if (maxValue <= minValue) {
						return minValue;
					}
					auto candidate = minValue + int(std::lround(
						raw * (maxValue - minValue)));
					candidate = minValue + int(std::lround(
						(candidate - minValue) / double(step))) * step;
					return std::clamp(candidate, minValue, maxValue);
				};
				const auto sliderFromValue = [=](int value) {
					if (maxValue <= minValue) {
						return 0.;
					}
					return (value - minValue) / double(maxValue - minValue);
				};
				valueLabel->setText(formatValue(setting.intValue));
				slider->setAdjustCallback([=](double raw) {
					return sliderFromValue(valueFromSlider(raw));
				});
				slider->setValue(sliderFromValue(setting.intValue));
				slider->setChangeProgressCallback([=](double raw) {
					valueLabel->setText(formatValue(valueFromSlider(raw)));
				});
				slider->setChangeFinishedCallback([=](double raw) {
					auto updated = setting;
					updated.intValue = valueFromSlider(raw);
					valueLabel->setText(formatValue(updated.intValue));
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				});
				container->add(
					std::move(sliderWithLabel.widget),
					st::settingsBigScalePadding);
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::TextInput: {
				if (!setting.title.trimmed().isEmpty()) {
					Ui::AddSkip(container);
					Ui::AddSubsectionTitle(
						container,
						rpl::single(setting.title.trimmed()));
				}
				const auto placeholder = setting.placeholderText.trimmed().isEmpty()
					? setting.title.trimmed()
					: setting.placeholderText.trimmed();
				const auto field = container->add(
					object_ptr<Ui::InputField>(
						container,
						st::settingLocalPasscodeInputField,
						rpl::single(placeholder),
						setting.textValue),
					style::al_top);
				const auto lastValue = std::make_shared<QString>(setting.textValue);
				QObject::connect(field, &Ui::MaskedInputField::changed, [=] {
					const auto current = field->getLastText().trimmed();
					if (current == *lastValue) {
						return;
					}
					*lastValue = current;
					auto updated = setting;
					updated.textValue = current;
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				});
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::ActionButton: {
				const auto buttonTitle = setting.buttonText.isEmpty()
					? setting.title
					: setting.buttonText;
				const auto button = container->add(object_ptr<Ui::SettingsButton>(
					container,
					rpl::single(buttonTitle),
					st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					if (!Core::App().plugins().updateSetting(page.id, setting) && onStateChanged) {
						onStateChanged();
					}
				});
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::InfoText: {
				const auto text = setting.description.trimmed().isEmpty()
					? setting.title.trimmed()
					: setting.description.trimmed();
				if (!text.isEmpty()) {
					Ui::AddDividerText(container, rpl::single(text));
				}
			} break;
			}
		}
	}
}

void ShowPluginDetailsBox(
		not_null<Window::SessionController*> controller,
		const QString &pluginId,
		Fn<void()> onStateChanged) {
	const auto state = LookupPluginState(pluginId);
	if (!state) {
		controller->window().showToast(PluginUiText(
			u"Plugin was not found."_q,
			u"Плагин не найден."_q));
		return;
	}

	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(FormatPluginTitle(*state)));
		box->addLeftButton(
			rpl::single(PluginUiText(u"Share"_q, u"Поделиться"_q)),
			[=] { SharePluginPackage(controller, *state); });
		box->addButton(
			rpl::single(PluginUiText(u"Delete"_q, u"Удалить"_q)),
			crl::guard(box, [=] {
				controller->show(Ui::MakeConfirmBox({
					.text = PluginUiText(
						u"Delete plugin \"%1\"?"_q,
						u"Удалить плагин \"%1\"?"_q).arg(FormatPluginTitle(*state)),
					.confirmed = crl::guard(box, [=] {
						QString error;
						if (!Core::App().plugins().removePlugin(state->info.id, &error)) {
							controller->window().showToast(
								error.isEmpty()
									? PluginUiText(
										u"Could not delete the plugin."_q,
										u"Не удалось удалить плагин."_q)
									: error);
							return;
						}
						if (onStateChanged) {
							onStateChanged();
						}
						box->closeBox();
					}),
					.confirmText = PluginUiText(u"Delete"_q, u"Удалить"_q),
				}));
			}));
		box->addButton(
			rpl::single(PluginUiText(u"Close"_q, u"Закрыть"_q)),
			[=] { box->closeBox(); });

		const auto content = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::al_top);
		const auto summary = FormatPluginSummary(*state);
		const auto note = FormatPluginStateNote(*state);

		if (!summary.isEmpty()) {
			Ui::AddDividerText(content, rpl::single(summary));
		}
		if (!note.isEmpty()) {
			Ui::AddSkip(content);
			Ui::AddDividerText(content, rpl::single(note));
		}

		const auto settingsPages = Core::App().plugins().settingsPagesFor(state->info.id);
		const auto actions = Core::App().plugins().actionsFor(state->info.id);
		const auto panels = Core::App().plugins().panelsFor(state->info.id);

		if (Core::App().plugins().safeModeEnabled()) {
			Ui::AddSkip(content);
			Ui::AddDividerText(
				content,
				rpl::single(PluginUiText(
					u"Safe mode is enabled. The plugin is shown as metadata only."_q,
					u"Безопасный режим включён. Плагин показан только как метаданные."_q)));
		}

		if (!settingsPages.empty()) {
			Ui::AddSkip(content);
			Ui::AddSubsectionTitle(
				content,
				rpl::single(PluginUiText(u"Settings"_q, u"Настройки"_q)));
			for (const auto &page : settingsPages) {
				Ui::AddSkip(content);
				AddPluginSettingsContent(content, page, onStateChanged);
			}
		}

		if (!actions.empty()) {
			Ui::AddSkip(content);
			Ui::AddSubsectionTitle(
				content,
				rpl::single(PluginUiText(u"Actions"_q, u"Действия"_q)));
			for (const auto &action : actions) {
				const auto actionButton = content->add(
					object_ptr<Ui::SettingsButton>(
						content,
						rpl::single(action.title),
						st::settingsButtonNoIcon));
				actionButton->setClickedCallback([=] {
					Core::App().plugins().triggerAction(action.id);
				});
				if (!action.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						content,
						rpl::single(action.description.trimmed()));
				}
			}
		}

		if (!panels.empty()) {
			Ui::AddSkip(content);
			Ui::AddSubsectionTitle(
				content,
				rpl::single(PluginUiText(
					u"Custom UI"_q,
					u"Пользовательский UI"_q)));
			for (const auto &panel : panels) {
				const auto panelButton = content->add(
					object_ptr<Ui::SettingsButton>(
						content,
						rpl::single(panel.title),
						st::settingsButtonNoIcon));
				panelButton->setClickedCallback([=] {
					Core::App().plugins().openPanel(panel.id);
				});
				if (!panel.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						content,
						rpl::single(panel.description.trimmed()));
				}
			}
		}

		if (settingsPages.empty() && actions.empty() && panels.empty()) {
			Ui::AddSkip(content);
			Ui::AddDividerText(
				content,
				rpl::single(PluginUiText(
					u"This plugin does not expose settings, actions or custom panels yet."_q,
					u"Этот плагин пока не предоставляет настроек, действий или пользовательских панелей."_q)));
		}
	}));
}

} // namespace

Plugins::Plugins(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent)
, _controller(controller)
, _content(Ui::CreateChild<Ui::VerticalLayout>(this))
, _list(_content->add(object_ptr<Ui::VerticalLayout>(_content))) {
	setupContent();
}

rpl::producer<QString> Plugins::title() {
	return rpl::single(PluginUiText(u"Plugins"_q, u"Плагины"_q));
}

void Plugins::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	addAction(
		PluginUiText(u"Documentation"_q, u"Документация"_q),
		[=] { ShowPluginDocsBox(_controller); },
		&st::menuIconFaq);
	addAction(
		PluginUiText(u"Open Plugins Folder"_q, u"Открыть папку плагинов"_q),
		[=] { File::ShowInFolder(Core::App().plugins().pluginsPath()); },
		&st::menuIconShowInFolder);
	const auto safeModeEnabled = Core::App().plugins().safeModeEnabled();
	addAction(
		safeModeEnabled
			? PluginUiText(
				u"Disable Safe Mode"_q,
				u"Выключить безопасный режим"_q)
			: PluginUiText(
				u"Enable Safe Mode"_q,
				u"Включить безопасный режим"_q),
		[=] {
			RequestSafeModeChange(
				_controller,
				this,
				!safeModeEnabled,
				crl::guard(this, [=] { rebuildList(); }));
		},
		&st::menuIconSettings);
}

void Plugins::setupContent() {
	Ui::AddDivider(_content);
	Ui::AddSkip(_content);

	rebuildList();
	Ui::ResizeFitChild(this, _content);
}

void Plugins::rebuildList() {
	_list->clear();
	if (Core::App().plugins().safeModeEnabled()) {
		Ui::AddDividerText(
			_list,
			rpl::single(
				PluginUiText(
					u"Safe mode is enabled. Plugins are shown without loading. Open the top bar menu to disable it."_q,
					u"Безопасный режим включён. Плагины показаны без загрузки. Откройте меню в верхней панели, чтобы выключить его."_q)));
		Ui::AddSkip(_list);
	}

	const auto plugins = Core::App().plugins().plugins();
	if (plugins.empty()) {
		Ui::AddDividerText(
			_list,
			rpl::single(PluginUiText(
				u"No plugins found in tdata/plugins."_q,
				u"В tdata/plugins плагины не найдены."_q)));
		Ui::AddSkip(_list);
		Ui::ResizeFitChild(this, _content);
		return;
	}

	auto first = true;
	const auto refresh = crl::guard(this, [=] { rebuildList(); });
	for (const auto &state : plugins) {
		if (!first) {
			Ui::AddDivider(_list);
		}
		first = false;
		Ui::AddSkip(_list);

		const auto title = FormatPluginTitle(state);
		const auto summary = FormatPluginSummary(state);
		const auto stateNote = FormatPluginStateNote(state);
		const auto openButton = AddButtonWithIcon(
			_list,
			rpl::single(title),
			state.recoverySuspected
				? st::settingsAttentionButton
				: st::settingsButton,
			{ &st::menuIconSettings });
		openButton->addClickHandler([=] {
			ShowPluginDetailsBox(_controller, state.info.id, refresh);
		});

		if (!summary.isEmpty()) {
			Ui::AddDividerText(_list, rpl::single(summary));
		}
		if (!stateNote.isEmpty()) {
			Ui::AddDividerText(_list, rpl::single(stateNote));
		}

		const auto toggle = _list->add(object_ptr<Ui::SettingsButton>(
			_list,
			rpl::single(PluginUiText(
				u"Enabled"_q,
				u"Включён"_q)),
			state.recoverySuspected
				? st::settingsAttentionButton
				: !state.error.isEmpty() && !state.disabledByRecovery
				? st::settingsOptionDisabled
				: st::settingsButtonNoIcon
		))->toggleOn(rpl::single(state.enabled));
		if (!state.error.isEmpty() && !state.disabledByRecovery) {
			toggle->setToggleLocked(true);
		}
		toggle->toggledChanges(
		) | rpl::on_next([=](bool value) {
			if (!Core::App().plugins().setEnabled(state.info.id, value)) {
				_controller->window().showToast(PluginUiText(
					u"Could not change state."_q,
					u"Не удалось изменить состояние плагина."_q));
			}
			rebuildList();
		}, toggle->lifetime());

		const auto actionsCount = int(Core::App().plugins().actionsFor(state.info.id).size());
		const auto panelsCount = int(Core::App().plugins().panelsFor(state.info.id).size());
		const auto settingsCount = int(Core::App().plugins().settingsPagesFor(
			state.info.id).size());
		Ui::AddDividerText(
			_list,
			rpl::single(PluginUiText(
				u"Open the plugin menu for settings, actions, share and delete."_q,
				u"Откройте меню плагина для настроек, действий, удаления и экспорта."_q)
				+ u"\n"_q
				+ PluginUiText(u"Settings pages: "_q, u"Страниц настроек: "_q)
				+ QString::number(settingsCount)
				+ u" • "_q
				+ PluginUiText(u"Actions: "_q, u"Действий: "_q)
				+ QString::number(actionsCount)
				+ u" • "_q
				+ PluginUiText(u"Custom UI: "_q, u"Пользовательский UI: "_q)
				+ QString::number(panelsCount)));
	}

	Ui::ResizeFitChild(this, _content);
}

PluginsDocumentation::PluginsDocumentation(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent)
, _controller(controller) {
	setupContent();
}

rpl::producer<QString> PluginsDocumentation::title() {
	return rpl::single(
		PluginUiText(u"Plugin Documentation"_q, u"Документация плагинов"_q));
}

void PluginsDocumentation::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	Ui::AddDividerText(content, rpl::single(PluginDocsText()));
	Ui::AddSkip(content);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
