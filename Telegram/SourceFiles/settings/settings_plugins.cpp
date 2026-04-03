/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_plugins.h"
#include "settings/settings_common.h"
#include "settings/cloud_password/settings_cloud_password_common.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "lang/lang_text_entity.h"
#include "plugins/plugins_manager.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtCore/QTimer>
#include <QtGui/QDesktopServices>
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

[[nodiscard]] bool IsTelegramHandleChar(QChar ch) {
	return ch.isLetterOrNumber() || (ch == QChar::fromLatin1('_'));
}

[[nodiscard]] TextWithEntities PluginAuthorText(const QString &author) {
	const auto trimmed = author.trimmed();
	auto result = TextWithEntities{
		PluginUiText(u"Author: "_q, u"Автор: "_q) + trimmed
	};
	const auto offset = result.text.size() - trimmed.size();
	for (auto i = 0; i < trimmed.size();) {
		if (trimmed[i] != QChar::fromLatin1('@')) {
			++i;
			continue;
		}
		auto j = i + 1;
		while (j < trimmed.size() && IsTelegramHandleChar(trimmed[j])) {
			++j;
		}
		if (j > (i + 1)) {
			result.entities.push_back({
				EntityType::CustomUrl,
				offset + i,
				j - i,
				u"https://t.me/"_q + trimmed.mid(i + 1, j - i - 1),
			});
		}
		i = std::max(j, i + 1);
	}
	return result;
}

void WireExternalLinks(not_null<Ui::FlatLabel*> label) {
	label->setClickHandlerFilter([=](const auto &handler, auto) {
		const auto entity = handler->getTextEntity();
		if (entity.type != EntityType::CustomUrl) {
			return true;
		}
		QDesktopServices::openUrl(QUrl(entity.data));
		return false;
	});
}

void AddPluginAuthorLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &author) {
	if (author.trimmed().isEmpty()) {
		return;
	}
	const auto label = Ui::AddDividerText(
		container,
		rpl::single(PluginAuthorText(author)));
	WireExternalLinks(label);
}

QString FormatPluginTitle(const ::Plugins::PluginState &state) {
	const auto &info = state.info;
	return !info.name.isEmpty()
		? info.name
		: (!info.id.isEmpty()
			? info.id
			: PluginUiText(u"Plugin"_q, u"Плагин"_q));
}

QString FormatPluginStatusBadge(const ::Plugins::PluginState &state) {
	QString status;
	if (state.disabledByRecovery) {
		status = PluginUiText(
			u"Recovery disabled"_q,
			u"Выключен recovery"_q);
	} else if (!state.error.trimmed().isEmpty()) {
		status = PluginUiText(u"Error"_q, u"Ошибка"_q);
	} else if (!state.enabled) {
		status = PluginUiText(u"Disabled"_q, u"Выключен"_q);
	} else if (state.loaded) {
		status = PluginUiText(u"Active"_q, u"Активен"_q);
	} else {
		status = PluginUiText(u"Metadata only"_q, u"Только метаданные"_q);
	}
	const auto version = state.info.version.trimmed();
	if (version.isEmpty()) {
		return status;
	}
	return version + u" • "_q + status;
}

QString FormatPluginFeatureList(const ::Plugins::PluginState &state) {
	auto items = QStringList();
	if (!Core::App().plugins().settingsPagesFor(state.info.id).empty()) {
		items.push_back(PluginUiText(u"Settings"_q, u"Настройки"_q));
	}
	if (!Core::App().plugins().actionsFor(state.info.id).empty()) {
		items.push_back(PluginUiText(u"Actions"_q, u"Действия"_q));
	}
	if (!Core::App().plugins().panelsFor(state.info.id).empty()) {
		items.push_back(PluginUiText(u"Custom UI"_q, u"Пользовательский UI"_q));
	}
	return items.join(u" • "_q);
}

QString PluginDocsText() {
	return UseRussianPluginUi()
		? QString::fromUtf8(R"PLUGIN(Плагины Astrogram Desktop (техническая документация)

0) Кратко про архитектуру
- Плагин = нативная библиотека .tgd, загружается в процесс Astrogram Desktop.
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

11) Runtime API / HostInfo (всегда видимо в документации)
- `host->hostInfo()` всегда содержит поля `runtimeApiEnabled`, `runtimeApiPort`, `runtimeApiBaseUrl`.
- Даже если runtime API выключен, эти поля остаются частью контракта HostInfo и описаны здесь без скрытых unlock-жестов.
- Для диагностики также доступны `systemInfo()`, `workingPath` и `pluginsPath`.

12) Корректная очистка в onUnload
```cpp
void onUnload() override {
	if (_commandId) _host->unregisterCommand(_commandId);
	if (_actionId) _host->unregisterAction(_actionId);
	if (_panelId) _host->unregisterPanel(_panelId);
	if (_outgoingId) _host->unregisterOutgoingTextInterceptor(_outgoingId);
	if (_observerId) _host->unregisterMessageObserver(_observerId);
}
```

13) CMake пример для плагина
```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

14) ABI/совместимость (обязательно)
- platform, pointer size, compiler ABI, Qt major/minor, plugin API version должны совпадать.
- Простое переименование файла в `.tgd` не работает: нужен реальный compile+link.
- Несовместимость пишется в plugins.log как load-failed/abi-mismatch.

15) Safe mode и recovery
- При падении в рискованной операции (load/onload/panel/command/window/session/observer...) менеджер включает safe mode.
- Подозрительный плагин выключается автоматически.
- На следующем запуске появляется recovery-уведомление.

16) Диагностика: что смотреть сначала
1. plugins.log: `load-failed`, `abi-mismatch`, `onload failed`, `panel failed`.
2. Путь и SHA пакета в логе.
3. Совпадение compiler + Qt.
4. Не храните long-lived сырые указатели на объекты Telegram.
5. Уберите тяжёлую синхронную работу из callback'ов UI.

17) Практика надёжности
- `info()` и конструктор плагина должны быть дешёвыми и без I/O.
- Любые сетевые/тяжёлые операции уносите в worker.
- UI код открывайте только из UI callback'ов.
- Всегда тестируйте onUnload после reload/disable.

18) Runtime API и host info
- Runtime API больше не скрывается за developer easter egg и считается частью публичной документации.
- Поля `hostInfo().runtimeApiEnabled`, `hostInfo().runtimeApiPort`, `hostInfo().runtimeApiBaseUrl` всегда видны плагину.
- Если runtime API выключен, пустые/нулевые значения — нормальное состояние.
- Проверяйте `runtimeApiEnabled`, а не наличие «секретной» кнопки или серии кликов по документации.
)PLUGIN")
		: QString::fromUtf8(R"PLUGIN(Astrogram Desktop Plugins (technical documentation)

0) Architecture
- Plugin = native .tgd shared library loaded into Astrogram Desktop process.
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

11) Runtime API / HostInfo (always visible in docs)
- `host->hostInfo()` always exposes `runtimeApiEnabled`, `runtimeApiPort`, and `runtimeApiBaseUrl`.
- Even when runtime API is disabled, these fields stay part of the host contract and are documented here without hidden unlock gestures.
- `systemInfo()`, `workingPath`, and `pluginsPath` are part of the runtime diagnostics surface.

12) onUnload cleanup
```cpp
void onUnload() override {
	if (_commandId) _host->unregisterCommand(_commandId);
	if (_actionId) _host->unregisterAction(_actionId);
	if (_panelId) _host->unregisterPanel(_panelId);
	if (_outgoingId) _host->unregisterOutgoingTextInterceptor(_outgoingId);
	if (_observerId) _host->unregisterMessageObserver(_observerId);
}
```

13) CMake sample
```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

14) ABI checklist
- Match platform, architecture, compiler ABI, Qt major/minor, plugin API version.
- Renaming files to `.tgd` is not enough; you must compile/link.
- ABI failures are logged as load-failed/abi-mismatch in plugins.log.

15) Recovery/safe mode
- If Telegram detects crash risk in plugin operation, it enables safe mode.
- Suspected plugin is disabled automatically.
- Recovery notice appears on next start.

16) Debug order
1. Check plugins.log (`load-failed`, `abi-mismatch`, `onload failed`, `panel failed`).
2. Verify package path and SHA in log.
3. Verify compiler + Qt match.
4. Avoid long-lived raw pointers to Telegram internals.
5. Move heavy sync work out of UI callbacks.

17) Runtime API and host info
- Runtime API is no longer hidden behind a developer easter egg and is treated as public documentation.
- `hostInfo().runtimeApiEnabled`, `hostInfo().runtimeApiPort`, `hostInfo().runtimeApiBaseUrl` are always visible to the plugin.
- Empty / zero values are normal when runtime API is disabled.
- Check `runtimeApiEnabled`, not the presence of a hidden button or repeated taps on documentation.
)PLUGIN");
}

QString PluginRuntimeText() {
	const auto host = Core::App().plugins().hostInfo();
	const auto system = Core::App().plugins().systemInfo();

	auto lines = QStringList();
	lines.push_back(PluginUiText(
		u"Plugin Runtime & Diagnostics"_q,
		u"Рантайм плагинов и диагностика"_q));
	lines.push_back(QString());
	lines.push_back(PluginUiText(
		u"Host"_q,
		u"Хост"_q));
	lines.push_back(
		PluginUiText(u"App version: "_q, u"Версия приложения: "_q)
		+ host.appVersion);
	lines.push_back(
		PluginUiText(u"UI language: "_q, u"Язык интерфейса: "_q)
		+ host.appUiLanguage);
	lines.push_back(
		PluginUiText(u"Compiler: "_q, u"Компилятор: "_q)
		+ host.compiler
		+ u" "_q
		+ QString::number(host.compilerVersion));
	lines.push_back(
		PluginUiText(u"Platform: "_q, u"Платформа: "_q)
		+ host.platform
		+ u" • Qt "_q
		+ QString::number(host.qtMajor)
		+ u"."_q
		+ QString::number(host.qtMinor));
	lines.push_back(
		PluginUiText(u"Working dir: "_q, u"Рабочая папка: "_q)
		+ QDir::toNativeSeparators(host.workingPath));
	lines.push_back(
		PluginUiText(u"Plugins dir: "_q, u"Папка плагинов: "_q)
		+ QDir::toNativeSeparators(host.pluginsPath));
	lines.push_back(
		PluginUiText(u"Safe mode: "_q, u"Безопасный режим: "_q)
		+ PluginUiText(
			host.safeModeEnabled ? u"enabled"_q : u"disabled"_q,
			host.safeModeEnabled ? u"включён"_q : u"выключен"_q));
	lines.push_back(
		PluginUiText(u"Runtime API: "_q, u"Runtime API: "_q)
		+ PluginUiText(
			host.runtimeApiEnabled ? u"enabled"_q : u"disabled"_q,
			host.runtimeApiEnabled ? u"включён"_q : u"выключен"_q));
	if (host.runtimeApiEnabled) {
		lines.push_back(
			PluginUiText(u"Runtime port: "_q, u"Порт runtime: "_q)
			+ QString::number(host.runtimeApiPort));
		lines.push_back(
			PluginUiText(u"Runtime base URL: "_q, u"Base URL runtime: "_q)
			+ host.runtimeApiBaseUrl);
	}

	lines.push_back(QString());
	lines.push_back(PluginUiText(
		u"System"_q,
		u"Система"_q));
	lines.push_back(
		PluginUiText(u"OS: "_q, u"ОС: "_q)
		+ system.prettyProductName);
	lines.push_back(
		PluginUiText(u"Kernel: "_q, u"Ядро: "_q)
		+ system.kernelType
		+ u" "_q
		+ system.kernelVersion);
	lines.push_back(
		PluginUiText(u"Architecture: "_q, u"Архитектура: "_q)
		+ system.architecture
		+ u" • "_q
		+ system.buildAbi);
	lines.push_back(
		PluginUiText(u"CPU cores: "_q, u"Ядра CPU: "_q)
		+ QString::number(system.logicalCpuCores)
		+ u" / "_q
		+ QString::number(system.physicalCpuCores));
	lines.push_back(
		PluginUiText(u"Locale: "_q, u"Локаль: "_q)
		+ system.locale
		+ u" • "_q
		+ system.uiLanguage);
	lines.push_back(
		PluginUiText(u"Time zone: "_q, u"Часовой пояс: "_q)
		+ system.timeZone);
	lines.push_back(
		PluginUiText(u"User: "_q, u"Пользователь: "_q)
		+ system.userName
		+ u" @ "_q
		+ system.hostName);
	return lines.join(u"\n"_q);
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

void ShowPluginRuntimeBox(not_null<Window::SessionController*> controller) {
	const auto text = PluginRuntimeText();
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			PluginUiText(
				u"Plugin Runtime"_q,
				u"Рантайм плагинов"_q)));
		box->addLeftButton(
			rpl::single(PluginUiText(u"Copy"_q, u"Копировать"_q)),
			[=] {
				if (const auto clipboard = QGuiApplication::clipboard()) {
					clipboard->setText(text);
				}
				controller->window().showToast(PluginUiText(
					u"Runtime info copied."_q,
					u"Информация о рантайме скопирована."_q));
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

void OpenPluginsFolder() {
	File::ShowInFolder(Core::App().plugins().pluginsPath());
}

QString FormatPluginSummary(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	const auto &info = state.info;
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

QString FormatPluginCardSummary(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	const auto &info = state.info;
	if (!info.description.trimmed().isEmpty()) {
		lines.push_back(info.description.trimmed());
	}
	return lines.join(u"\n"_q);
}

QString FormatPluginCapabilityLine(const ::Plugins::PluginState &state) {
	const auto features = FormatPluginFeatureList(state);
	const auto settingsCount = int(Core::App().plugins().settingsPagesFor(
		state.info.id).size());
	const auto actionsCount = int(Core::App().plugins().actionsFor(
		state.info.id).size());
	const auto panelsCount = int(Core::App().plugins().panelsFor(
		state.info.id).size());
	return PluginUiText(
		u"Available: "_q,
		u"Доступно: "_q)
		+ (features.isEmpty()
			? PluginUiText(u"Metadata only"_q, u"Только метаданные"_q)
			: features)
		+ u"\n"_q
		+ PluginUiText(u"Pages: "_q, u"Страницы: "_q)
		+ QString::number(settingsCount)
		+ u"  •  "_q
		+ PluginUiText(u"Actions: "_q, u"Действия: "_q)
		+ QString::number(actionsCount)
		+ u"  •  "_q
		+ PluginUiText(u"Panels: "_q, u"Панели: "_q)
		+ QString::number(panelsCount);
}

QString PluginOverviewText(const std::vector<::Plugins::PluginState> &plugins) {
	auto enabled = 0;
	auto loaded = 0;
	auto recovery = 0;
	auto failed = 0;
	for (const auto &state : plugins) {
		if (state.enabled) {
			++enabled;
		}
		if (state.loaded) {
			++loaded;
		}
		if (state.disabledByRecovery || state.recoverySuspected) {
			++recovery;
		}
		if (!state.error.trimmed().isEmpty()) {
			++failed;
		}
	}
	return PluginUiText(
		u"Installed: %1  •  Enabled: %2  •  Loaded: %3  •  Recovery: %4  •  Errors: %5"_q,
		u"Установлено: %1  •  Включено: %2  •  Загружено: %3  •  Recovery: %4  •  Ошибки: %5"_q
	).arg(plugins.size()).arg(enabled).arg(loaded).arg(recovery).arg(failed);
}

QString FormatPluginCardNote(const ::Plugins::PluginState &state) {
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
	return lines.join(u"\n"_q);
}

QString FormatPluginDetailsNote(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	const auto base = FormatPluginCardNote(state);
	if (!base.isEmpty()) {
		lines.push_back(base);
	}
	if (!state.path.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Package: "_q, u"Пакет: "_q)
			+ QDir::toNativeSeparators(state.path));
	}
	return lines.join(u"\n"_q);
}

void RevealPluginAuxFile(
		not_null<Window::SessionController*> controller,
		const QString &path,
		const QString &errorText) {
	if (!QFileInfo(path).exists()) {
		controller->window().showToast(errorText);
		return;
	}
	File::ShowInFolder(path);
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

void RequestPluginRemoval(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> context,
		const ::Plugins::PluginState &state,
		Fn<void()> onRemoved) {
	controller->show(Ui::MakeConfirmBox({
		.text = PluginUiText(
			u"Delete plugin \"%1\"?"_q,
			u"Удалить плагин \"%1\"?"_q).arg(FormatPluginTitle(state)),
		.confirmed = crl::guard(context, [=] {
			QString error;
			if (!Core::App().plugins().removePlugin(state.info.id, &error)) {
				controller->window().showToast(
					error.isEmpty()
						? PluginUiText(
							u"Could not delete the plugin."_q,
							u"Не удалось удалить плагин."_q)
						: error);
				return;
			}
			if (onRemoved) {
				onRemoved();
			}
		}),
		.confirmText = PluginUiText(u"Delete"_q, u"Удалить"_q),
	}));
}

void AttachPluginCardActions(
		not_null<Button*> button,
		not_null<Window::SessionController*> controller,
		const ::Plugins::PluginState &state,
		Fn<void()> onChanged) {
	const auto raw = static_cast<Ui::RpWidget*>(button.get());
	const auto settings = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarEdit);
	const auto share = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarForward);
	const auto remove = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarDelete);

	settings->setClickedCallback([=] {
		controller->showSettings(PluginDetailsId(state.info.id));
	});
	share->setClickedCallback([=] {
		SharePluginPackage(controller, state);
	});
	remove->setClickedCallback([=] {
		RequestPluginRemoval(controller, raw, state, onChanged);
	});

	raw->widthValue() | rpl::start_with_next([=](int width) {
		const auto gap = 6;
		const auto total = settings->width() + share->width() + remove->width() + (gap * 2);
		auto left = std::max(st::settingsButton.padding.left(), width - total - st::settingsButton.padding.right() - 6);
		const auto top = std::max(0, (raw->height() - settings->height()) / 2);
		settings->move(left, top);
		left += settings->width() + gap;
		share->move(left, top);
		left += share->width() + gap;
		remove->move(left, top);
	}, raw->lifetime());
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
			u"Astrogram will unload all plugins and reopen the plugin list in metadata-only mode."_q,
			u"Astrogram выгрузит все плагины и откроет список плагинов в режиме только-метаданных."_q)
		: PluginUiText(
			u"Astrogram will try to load plugins again. Disable safe mode only if you trust the installed plugins."_q,
			u"Astrogram снова попробует загрузить плагины. Выключайте безопасный режим только если доверяете установленным плагинам."_q);
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
				const auto lastValue = std::make_shared<QString>(setting.textValue);
				const auto handleTextChange = [=](const QString &current) {
					if (current == *lastValue) {
						return;
					}
					*lastValue = current;
					auto updated = setting;
					updated.textValue = current;
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				};
				if (setting.secret) {
					const auto field = CloudPassword::AddPasswordField(
						container,
						rpl::single(placeholder),
						setting.textValue);
					QObject::connect(field, &Ui::MaskedInputField::changed, [=] {
						handleTextChange(field->getLastText().trimmed());
					});
				} else {
					const auto field = CloudPassword::AddWrappedField(
						container,
						rpl::single(placeholder),
						setting.textValue);
					field->changes() | rpl::on_next([=] {
						handleTextChange(field->getLastText().trimmed());
					}, field->lifetime());
				}
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

class PluginDetailsSection final : public AbstractSection {
public:
	PluginDetailsSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		QString pluginId,
		Type type)
	: AbstractSection(parent)
	, _controller(controller)
	, _pluginId(std::move(pluginId))
	, _type(std::move(type))
	, _content(Ui::CreateChild<Ui::VerticalLayout>(this)) {
		rebuild();
	}

	[[nodiscard]] Type id() const override {
		return _type;
	}

	[[nodiscard]] rpl::producer<QString> title() override {
		return rpl::single(_title);
	}

private:
	void rebuild() {
		_content->clear();
		Ui::AddDivider(_content);
		Ui::AddSkip(_content);

		const auto state = LookupPluginState(_pluginId);
		if (!state) {
			_title = PluginUiText(u"Plugin"_q, u"Плагин"_q);
			Ui::AddDividerText(
				_content,
				rpl::single(PluginUiText(
					u"Plugin was not found."_q,
					u"Плагин не найден."_q)));
			Ui::ResizeFitChild(this, _content);
			return;
		}

		_title = FormatPluginTitle(*state);
		const auto stateChanged = crl::guard(this, [=] { rebuild(); });

		const auto settingsPages = Core::App().plugins().settingsPagesFor(state->info.id);

		if (Core::App().plugins().safeModeEnabled()) {
			Ui::AddDividerText(
				_content,
				rpl::single(PluginUiText(
					u"Safe mode is enabled. The plugin is shown as metadata only."_q,
					u"Безопасный режим включён. Плагин показан только как метаданные."_q)));
		}

		if (!settingsPages.empty()) {
			Ui::AddSubsectionTitle(
				_content,
				rpl::single(PluginUiText(u"Settings"_q, u"Настройки"_q)));
			for (const auto &page : settingsPages) {
				Ui::AddSkip(_content);
				AddPluginSettingsContent(_content, page, stateChanged);
			}
		} else {
			Ui::AddSkip(_content);
			Ui::AddDividerText(
				_content,
				rpl::single(PluginUiText(
					u"This plugin does not expose separate settings yet."_q,
					u"У этого плагина пока нет отдельных настроек."_q)));
		}

		Ui::ResizeFitChild(this, _content);
	}

	const not_null<Window::SessionController*> _controller;
	const QString _pluginId;
	const Type _type;
	not_null<Ui::VerticalLayout*> _content;
	QString _title;
};

struct PluginDetailsFactory final
	: AbstractSectionFactory
	, std::enable_shared_from_this<PluginDetailsFactory> {
	explicit PluginDetailsFactory(QString pluginId)
	: pluginId(std::move(pluginId)) {
	}

	object_ptr<AbstractSection> create(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*> scroll,
		rpl::producer<Container> containerValue
	) const final override {
		Q_UNUSED(scroll);
		Q_UNUSED(containerValue);
		const auto type = std::static_pointer_cast<AbstractSectionFactory>(
			std::const_pointer_cast<PluginDetailsFactory>(shared_from_this()));
		return object_ptr<PluginDetailsSection>(
			parent,
			controller,
			pluginId,
			type);
	}

	QString pluginId;
};

[[nodiscard]] Type MakePluginDetailsType(const QString &pluginId) {
	return std::make_shared<PluginDetailsFactory>(pluginId);
}

} // namespace

Type PluginDetailsId(const QString &pluginId) {
	return MakePluginDetailsType(pluginId);
}

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
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Documentation"_q, u"Документация"_q),
		.handler = [=] {
			QDesktopServices::openUrl(QUrl(u"https://docs.astrogram.su"_q));
		},
		.icon = &st::menuIconFaq,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(
			u"Runtime & Diagnostics"_q,
			u"Рантайм и диагностика"_q),
		.handler = [=] { ShowPluginRuntimeBox(_controller); },
		.icon = &st::menuIconIpAddress,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Open Plugins Folder"_q, u"Открыть папку плагинов"_q),
		.handler = [=] { File::ShowInFolder(Core::App().plugins().pluginsPath()); },
		.icon = &st::menuIconShowInFolder,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Open plugins.log"_q, u"Открыть plugins.log"_q),
		.handler = [=] {
			RevealPluginAuxFile(
				_controller,
				u"./tdata/plugins.log"_q,
				PluginUiText(
					u"plugins.log was not found."_q,
					u"Файл plugins.log не найден."_q));
		},
		.icon = &st::menuIconSettings,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(
			u"Open plugins.trace.jsonl"_q,
			u"Открыть plugins.trace.jsonl"_q),
		.handler = [=] {
			RevealPluginAuxFile(
				_controller,
				u"./tdata/plugins.trace.jsonl"_q,
				PluginUiText(
					u"plugins.trace.jsonl was not found."_q,
					u"Файл plugins.trace.jsonl не найден."_q));
		},
		.icon = &st::menuIconSettings,
	});
	const auto safeModeEnabled = Core::App().plugins().safeModeEnabled();
	addAction(Ui::Menu::MenuCallback::Args{
		.text = safeModeEnabled
			? PluginUiText(
				u"Disable Safe Mode"_q,
				u"Выключить безопасный режим"_q)
			: PluginUiText(
				u"Enable Safe Mode"_q,
				u"Включить безопасный режим"_q),
		.handler = [=] {
			RequestSafeModeChange(
				_controller,
				this,
				!safeModeEnabled,
				crl::guard(this, [=] { rebuildList(); }));
		},
		.icon = &st::menuIconSettings,
	});
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
		const auto disableSafeModeButton = AddButtonWithIcon(
			_list,
			rpl::single(PluginUiText(
				u"Disable Safe Mode"_q,
				u"Выключить безопасный режим"_q)),
			st::settingsAttentionButton,
			{ &st::menuIconSettings });
		disableSafeModeButton->addClickHandler([=] {
			RequestSafeModeChange(
				_controller,
				this,
				false,
				crl::guard(this, [=] { rebuildList(); }));
		});
		Ui::AddSkip(_list);
	}

	const auto plugins = Core::App().plugins().plugins();
	if (plugins.empty()) {
		Ui::AddDividerText(
			_list,
			rpl::single(PluginUiText(
				u"No plugins found in tdata/plugins."_q,
				u"В tdata/plugins плагины не найдены."_q)));
		const auto folderButton = AddButtonWithIcon(
			_list,
			rpl::single(PluginUiText(
				u"Open Plugins Folder"_q,
				u"Открыть папку плагинов"_q)),
			st::settingsButton,
			{ &st::menuIconShowInFolder });
		folderButton->addClickHandler([=] {
			OpenPluginsFolder();
		});
		Ui::AddSkip(_list);
		Ui::ResizeFitChild(this, _content);
		return;
	}
	auto first = true;
	for (const auto &state : plugins) {
		if (!first) {
			Ui::AddSkip(_list);
			Ui::AddDivider(_list);
		}
		first = false;
		Ui::AddSkip(_list);

		const auto title = FormatPluginTitle(state);
		const auto versionBadge = FormatPluginStatusBadge(state);
		const auto summary = FormatPluginCardSummary(state);
		const auto stateNote = FormatPluginCardNote(state);
		const auto header = AddButtonWithIcon(
			_list,
			rpl::single(title),
			state.recoverySuspected
				? st::settingsAttentionButton
				: !state.error.isEmpty() && !state.disabledByRecovery
				? st::settingsOptionDisabled
				: st::settingsButtonLightNoIcon);
		header->toggleOn(rpl::single(state.enabled));
		if (!state.error.isEmpty() && !state.disabledByRecovery) {
			header->setToggleLocked(true);
		}
		header->toggledChanges(
		) | rpl::on_next([=](bool value) {
			if (!Core::App().plugins().setEnabled(state.info.id, value)) {
				_controller->window().showToast(PluginUiText(
					u"Could not change state."_q,
					u"Не удалось изменить состояние плагина."_q));
				}
			rebuildList();
		}, header->lifetime());
		AttachPluginCardActions(
			header,
			_controller,
			state,
			crl::guard(this, [=] { rebuildList(); }));
		Ui::AddDividerText(_list, rpl::single(versionBadge));
		if (!summary.isEmpty()) {
			Ui::AddDividerText(_list, rpl::single(summary));
		}
		AddPluginAuthorLabel(_list, state.info.author);
		if (!stateNote.isEmpty()) {
			Ui::AddDividerText(_list, rpl::single(stateNote));
		}
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
