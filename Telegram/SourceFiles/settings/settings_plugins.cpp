/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_plugins.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "plugins/plugins_manager.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_settings.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

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

QString FormatPluginDetails(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	const auto &info = state.info;
	if (!info.author.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Author: "_q, u"Автор: "_q)
			+ info.author.trimmed());
	}
	if (!info.description.trimmed().isEmpty()) {
		lines.push_back(info.description.trimmed());
	}
	if (!info.version.trimmed().isEmpty()) {
		lines.push_back(
			PluginUiText(u"Version: "_q, u"Версия: "_q)
			+ info.version.trimmed());
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

QString PluginDocsText() {
	return UseRussianPluginUi()
		? u"Плагины Telegram Desktop\n\n"
			"Что это\n"
			"Плагин — это нативная библиотека .tgd, которая загружается прямо в процесс Telegram Desktop. Плагины могут добавлять команды, действия, панели, перехватывать исходящий текст и слушать события сообщений. Из-за этого плагины очень мощные, но и небезопасные: они работают с тем же ABI и теми же зависимостями, что и сам клиент.\n\n"
			"Где лежат файлы\n"
			"- Папка плагинов: <working dir>/tdata/plugins\n"
			"- Список вручную выключенных плагинов: <working dir>/tdata/plugins.json\n"
			"- Безопасный режим: <working dir>/tdata/plugins.safe-mode\n"
			"- Лог загрузки и runtime-сбоев: <working dir>/tdata/plugins.log\n"
			"- Recovery-state: <working dir>/tdata/plugins.recovery.json\n\n"
			"Установка и обновление\n"
			"- При клике на .tgd из чата Telegram показывает preview-box.\n"
			"- Preview берётся из статической metadata, без запуска кода плагина.\n"
			"- После установки плагин подхватывается автоматически, отдельная кнопка Reload не нужна.\n"
			"- Если пакет уже установлен, box покажет обновление со старой зачёркнутой версией.\n\n"
			"Совместимость\n"
			"- Плагин обязан совпадать с Telegram по platform, architecture, compiler ABI, Qt major/minor и plugin API version.\n"
			"- Просто переименовать .cpp в .tgd нельзя: .tgd — это уже собранный бинарь.\n"
			"- Несовместимый плагин не должен загружаться, а причина пишется в plugins.log.\n\n"
			"Metadata preview\n"
			"- Символ: TgdPluginPreviewInfo\n"
			"- Поля: id, name, version, author, description, website, icon\n"
			"- icon использует формат StickerPackShortName/index\n"
			"- Если иконка недоступна, UI показывает fallback-аватар.\n\n"
			"Жизненный цикл\n"
			"- TgdPluginEntry создаёт объект плагина.\n"
			"- info() должен быть дешёвым и без побочных эффектов.\n"
			"- onLoad() — регистрация команд, действий, панелей и подписок.\n"
			"- onUnload() — очистка при выключении/перезагрузке.\n\n"
			"Возможности API\n"
			"- registerCommand: slash-команды в поле ввода.\n"
			"- registerAction / registerActionWithContext: кнопки-действия в разделе Plugins.\n"
			"- registerPanel: полноценные UI entry points.\n"
			"- registerOutgoingTextInterceptor: перехват исходящих сообщений.\n"
			"- registerMessageObserver: наблюдение за new/edited/deleted сообщениями.\n"
			"- forEachWindow / onWindowCreated / activeSession / forEachSession: доступ к окнам и аккаунтам.\n\n"
			"Безопасность и recovery\n"
			"- Исключения из managed callback'ов автоматически выключают проблемный плагин.\n"
			"- При подозрении на native crash во время рискованной plugin-операции Telegram включает safe mode автоматически.\n"
			"- Подозреваемый плагин выключается, лог копируется в буфер, а на следующем запуске показывается recovery-box.\n"
			"- Safe mode не удаляет плагины, а только не даёт им загрузиться.\n\n"
			"Runtime API\n"
			"- При включении Telegram поднимает локальный HTTP endpoint на 127.0.0.1.\n"
			"- API позволяет получить host/system info, список плагинов, окна, сессии, выполнить reload и установить .tgd по локальному пути.\n"
			"- Для всех endpoint'ов кроме /api/ping нужен токен авторизации.\n"
			"- Токен можно скопировать или перевыпустить в разделе Plugins.\n\n"
			"Практические советы\n"
			"- Делайте конструктор и info() максимально лёгкими.\n"
			"- Тяжёлую работу уносите в worker thread или отдельный процесс.\n"
			"- Если вы открываете UI, дополнительно линкуйте QtWidgets.\n"
			"- Не храните HistoryItem* вне callback'а.\n"
			"- Проверяйте exact ABI match перед публикацией .tgd.\n"_q
		: u"Telegram Desktop Plugins\n\n"
			"What it is\n"
			"A plugin is a native .tgd shared library loaded into the Telegram Desktop process. Plugins can add commands, actions, panels, outgoing text interceptors, and message observers. This makes them powerful, but also unsafe: they run with the same ABI and dependencies as the app itself.\n\n"
			"Files and folders\n"
			"- Plugin folder: <working dir>/tdata/plugins\n"
			"- Manually disabled plugins: <working dir>/tdata/plugins.json\n"
			"- Safe mode flag: <working dir>/tdata/plugins.safe-mode\n"
			"- Load/runtime log: <working dir>/tdata/plugins.log\n"
			"- Recovery state: <working dir>/tdata/plugins.recovery.json\n\n"
			"Install and update\n"
			"- Clicking a .tgd in chat opens a preview box.\n"
			"- Preview metadata is read without running plugin code.\n"
			"- After install, Telegram reloads plugins automatically. No separate Reload button is required.\n"
			"- If the package is already installed, the box shows an update with the old version struck through.\n\n"
			"Compatibility\n"
			"- A plugin must match Telegram on platform, architecture, compiler ABI, Qt major/minor, and plugin API version.\n"
			"- Renaming a .cpp file to .tgd does not work: .tgd is already a compiled binary.\n"
			"- Incompatible plugins should fail to load and write the reason to plugins.log.\n\n"
			"Preview metadata\n"
			"- Symbol: TgdPluginPreviewInfo\n"
			"- Fields: id, name, version, author, description, website, icon\n"
			"- icon uses StickerPackShortName/index\n"
			"- If the icon cannot be resolved, UI falls back to a generated avatar.\n\n"
			"Lifecycle\n"
			"- TgdPluginEntry constructs the plugin object.\n"
			"- info() should stay cheap and side-effect free.\n"
			"- onLoad() is where commands, actions, panels, and subscriptions are registered.\n"
			"- onUnload() should clean up during disable/reload.\n\n"
			"API surface\n"
			"- registerCommand: slash commands in the compose field.\n"
			"- registerAction / registerActionWithContext: action buttons in the Plugins section.\n"
			"- registerPanel: full UI entry points.\n"
			"- registerOutgoingTextInterceptor: intercept outgoing text.\n"
			"- registerMessageObserver: observe new/edited/deleted messages.\n"
			"- forEachWindow / onWindowCreated / activeSession / forEachSession: access windows and sessions.\n\n"
			"Safety and recovery\n"
			"- Exceptions escaping managed callbacks automatically disable the failing plugin.\n"
			"- If Telegram suspects a native crash during a risky plugin operation, it enables safe mode automatically.\n"
			"- The suspected plugin is turned off, the recovery log is copied to the clipboard, and the next launch shows a recovery box.\n"
			"- Safe mode does not delete plugins; it only prevents loading them.\n\n"
			"Runtime API\n"
			"- When enabled, Telegram starts a local HTTP endpoint on 127.0.0.1.\n"
			"- The API can return host/system info, plugin lists, windows, sessions, trigger reload, and install a .tgd from a local path.\n"
			"- Every endpoint except /api/ping requires an authorization token.\n"
			"- The token can be copied or rotated from the Plugins section.\n\n"
			"Practical advice\n"
			"- Keep constructors and info() lightweight.\n"
			"- Move heavy work to worker threads or a separate process.\n"
			"- Link QtWidgets if your plugin opens UI.\n"
			"- Do not keep HistoryItem* beyond the callback lifetime.\n"
			"- Always verify exact ABI match before shipping a .tgd.\n"_q;
}

void ShowPluginDocsBox(not_null<Window::SessionController*> controller) {
	const auto text = PluginDocsText();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			PluginUiText(u"Plugin Documentation"_q, u"Документация плагинов"_q)));
		box->addLeftButton(
			PluginUiText(u"Copy"_q, u"Копировать"_q),
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

void Plugins::setupContent() {
	Ui::AddDivider(_content);
	Ui::AddSkip(_content);

	const auto docs = AddButtonWithIcon(
		_content,
		rpl::single(PluginUiText(u"Documentation"_q, u"Документация"_q)),
		st::settingsButton,
		{ &st::menuIconFaq });
	docs->addClickHandler([=] {
		ShowPluginDocsBox(_controller);
	});

	const auto openFolder = AddButtonWithIcon(
		_content,
		rpl::single(
			PluginUiText(u"Open Plugins Folder"_q, u"Открыть папку плагинов"_q)),
		st::settingsButton,
		{ &st::menuIconShowInFolder });
	openFolder->addClickHandler([=] {
		File::ShowInFolder(Core::App().plugins().pluginsPath());
	});

	const auto safeMode = _content->add(object_ptr<Ui::SettingsButton>(
		_content,
		rpl::single(PluginUiText(
			u"Plugin Safe Mode"_q,
			u"Безопасный режим плагинов"_q)),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(Core::App().plugins().safeModeEnabled()));
	safeMode->toggledChanges(
	) | rpl::on_next([=](bool value) {
		if (!Core::App().plugins().setSafeModeEnabled(value)) {
			_controller->window().showToast(PluginUiText(
				u"Could not change safe mode."_q,
				u"Не удалось переключить безопасный режим."_q));
		}
		rebuildList();
	}, safeMode->lifetime());
	Ui::AddDividerText(
		_content,
		rpl::single(
			PluginUiText(
				u"When enabled, Telegram skips plugin loading. "
				u"Crash recovery may enable this mode automatically."_q,
				u"Когда режим включён, Telegram не загружает плагины. "
				u"После крэша плагина этот режим может включиться автоматически."_q)));

	const auto runtimeApi = _content->add(object_ptr<Ui::SettingsButton>(
		_content,
		rpl::single(PluginUiText(
			u"Plugin Runtime API"_q,
			u"Runtime API плагинов"_q)),
		st::settingsButtonNoIcon
	))->toggleOn(rpl::single(Core::App().plugins().runtimeApiEnabled()));
	runtimeApi->toggledChanges(
	) | rpl::on_next([=](bool value) {
		if (!Core::App().plugins().setRuntimeApiEnabled(value)) {
			_controller->window().showToast(PluginUiText(
				u"Could not change runtime API state."_q,
				u"Не удалось переключить runtime API."_q));
		}
		rebuildList();
	}, runtimeApi->lifetime());
	Ui::AddDividerText(
		_content,
		rpl::single(PluginUiText(
			u"When enabled, Telegram listens on a localhost HTTP endpoint and requires the runtime token for privileged requests."_q,
			u"Когда режим включён, Telegram поднимает локальный HTTP endpoint и требует runtime-токен для привилегированных запросов."_q)));

	const auto copyRuntimeUrl = _content->add(object_ptr<Ui::SettingsButton>(
		_content,
		rpl::single(PluginUiText(
			u"Copy Runtime API URL"_q,
			u"Скопировать URL Runtime API"_q)),
		st::settingsButtonNoIcon));
	copyRuntimeUrl->setClickedCallback([=] {
		if (const auto clipboard = QGuiApplication::clipboard()) {
			clipboard->setText(Core::App().plugins().runtimeApiBaseUrl());
		}
		_controller->window().showToast(PluginUiText(
			u"Runtime API URL copied."_q,
			u"URL Runtime API скопирован."_q));
	});

	const auto copyRuntimeToken = _content->add(object_ptr<Ui::SettingsButton>(
		_content,
		rpl::single(PluginUiText(
			u"Copy Runtime API Token"_q,
			u"Скопировать токен Runtime API"_q)),
		st::settingsButtonNoIcon));
	copyRuntimeToken->setClickedCallback([=] {
		if (const auto clipboard = QGuiApplication::clipboard()) {
			clipboard->setText(Core::App().plugins().runtimeApiToken());
		}
		_controller->window().showToast(PluginUiText(
			u"Runtime API token copied."_q,
			u"Токен Runtime API скопирован."_q));
	});

	const auto rotateRuntimeToken = _content->add(object_ptr<Ui::SettingsButton>(
		_content,
		rpl::single(PluginUiText(
			u"Rotate Runtime API Token"_q,
			u"Перевыпустить токен Runtime API"_q)),
		st::settingsButtonNoIcon));
	rotateRuntimeToken->setClickedCallback([=] {
		const auto token = Core::App().plugins().rotateRuntimeApiToken();
		if (const auto clipboard = QGuiApplication::clipboard()) {
			clipboard->setText(token);
		}
		_controller->window().showToast(PluginUiText(
			u"New runtime API token copied."_q,
			u"Новый токен Runtime API скопирован."_q));
	});

	Ui::AddDivider(_content);
	Ui::AddSkip(_content);

	rebuildList();
	Ui::ResizeFitChild(this, _content);
}

void Plugins::rebuildList() {
	_list->clear();

	const auto plugins = Core::App().plugins().plugins();
	if (plugins.empty()) {
		if (Core::App().plugins().safeModeEnabled()) {
			Ui::AddDividerText(
				_list,
				rpl::single(
					PluginUiText(
						u"Plugin safe mode is enabled. Plugins are listed without loading."_q,
						u"Безопасный режим включён. Плагины показаны без загрузки."_q)));
		} else {
			Ui::AddDividerText(
				_list,
				rpl::single(PluginUiText(
					u"No plugins found in tdata/plugins."_q,
					u"В tdata/plugins плагины не найдены."_q)));
		}
		Ui::AddSkip(_list);
		Ui::ResizeFitChild(this, _content);
		return;
	}

	auto first = true;
	for (const auto &state : plugins) {
		if (!first) {
			Ui::AddDivider(_list);
		}
		first = false;
		Ui::AddSkip(_list);

		const auto title = FormatPluginTitle(state);
		const auto buttonStyle = state.recoverySuspected
			? st::settingsAttentionButton
			: state.error.isEmpty()
			? st::settingsButtonNoIcon
			: st::settingsOptionDisabled;
		const auto toggle = _list->add(object_ptr<Ui::SettingsButton>(
			_list,
			rpl::single(title),
			buttonStyle
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

		const auto actions = Core::App().plugins().actionsFor(state.info.id);
		const auto panels = Core::App().plugins().panelsFor(state.info.id);
		const auto details = FormatPluginDetails(state);
		if (!details.isEmpty()) {
			Ui::AddSkip(_list);
			Ui::AddDividerText(_list, rpl::single(details));
		}

		if (!actions.empty()) {
			Ui::AddSkip(_list);
			Ui::AddDividerText(
				_list,
				rpl::single(PluginUiText(u"Actions"_q, u"Действия"_q)));
			Ui::AddSkip(_list);
			for (const auto &action : actions) {
				const auto actionButton = _list->add(
					object_ptr<Ui::SettingsButton>(
						_list,
						rpl::single(action.title),
						st::settingsButtonNoIcon));
				actionButton->setClickedCallback([=] {
					Core::App().plugins().triggerAction(action.id);
				});
				if (!action.description.isEmpty()) {
					Ui::AddDividerText(
						_list,
						rpl::single(action.description));
				}
			}
		}

		if (!panels.empty()) {
			Ui::AddSkip(_list);
			Ui::AddDividerText(
				_list,
				rpl::single(PluginUiText(u"Panels"_q, u"Панели"_q)));
			Ui::AddSkip(_list);
			for (const auto &panel : panels) {
				const auto panelButton = _list->add(
					object_ptr<Ui::SettingsButton>(
						_list,
						rpl::single(panel.title),
						st::settingsButtonNoIcon));
				panelButton->setClickedCallback([=] {
					Core::App().plugins().openPanel(panel.id);
				});
				if (!panel.description.isEmpty()) {
					Ui::AddDividerText(
						_list,
						rpl::single(panel.description));
				}
			}
		}

		Ui::AddSkip(_list);
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
