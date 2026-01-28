/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_plugins.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "plugins/plugins_manager.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

QString FormatPluginTitle(const ::Plugins::PluginState &state) {
	const auto &info = state.info;
	const auto name = !info.name.isEmpty()
		? info.name
		: (!info.id.isEmpty() ? info.id : u"Plugin"_q);
	const auto version = info.version.trimmed();
	return version.isEmpty() ? name : (name + u" "_q + version);
}

QString FormatPluginDetails(
		const ::Plugins::PluginState &state,
		const std::vector<::Plugins::CommandDescriptor> &commands) {
	auto lines = QStringList();
	const auto &info = state.info;
	if (!info.id.isEmpty()) {
		lines.push_back(u"Id: "_q + info.id);
	}
	if (!info.author.isEmpty()) {
		lines.push_back(u"Author: "_q + info.author);
	}
	if (!state.path.isEmpty()) {
		lines.push_back(u"Path: "_q + state.path);
	}
	if (!info.website.isEmpty()) {
		lines.push_back(u"Website: "_q + info.website);
	}
	if (!info.description.isEmpty()) {
		lines.push_back(u"Description: "_q + info.description);
	}
	lines.push_back(state.loaded
		? u"Status: Loaded"_q
		: state.enabled
		? u"Status: Enabled"_q
		: u"Status: Disabled"_q);
	if (!state.error.isEmpty()) {
		lines.push_back(u"Error: "_q + state.error);
	}
	if (!commands.empty()) {
		lines.push_back(u"Commands:"_q);
		for (const auto &command : commands) {
			auto line = u"  "_q + command.command;
			if (!command.description.isEmpty()) {
				line += u" - "_q + command.description;
			}
			lines.push_back(line);
			if (!command.usage.isEmpty()) {
				lines.push_back(u"    Usage: "_q + command.usage);
			}
		}
	}
	return lines.join('\n');
}

QString PluginDocsText() {
	return u"Overview\n"
		"Plugins are native shared libraries loaded by Telegram Desktop at "
		"startup. They can register slash commands, add actions and panels in "
		"the Plugins menu, intercept outgoing messages, and observe backend "
		"updates. Because plugins are native code, they can call internal APIs "
		"and customize UI by using the same headers as the app. Use with care.\n\n"
		"File format and location\n"
		"- Extension: .tgd (shared library, renamed).\n"
		"- Folder: <working dir>/tdata/plugins\n"
		"- Config: <working dir>/tdata/plugins.json (disabled list)\n\n"
		"API header (v2)\n"
		"- Telegram/SourceFiles/plugins/plugins_api.h\n"
		"- Main entry symbol: TgdPluginEntry\n\n"
		"Plugin lifecycle\n"
		"- The loader resolves TgdPluginEntry and constructs your plugin.\n"
		"- Entry receives apiVersion; accept versions >= required.\n"
		"- info() is called to read metadata (id, name, version).\n"
		"- Keep constructors and info() side-effect free.\n"
		"- onLoad() is called when enabled; register commands/actions here.\n"
		"- onUnload() is called on reload or disable; cleanup here.\n\n"
		"Commands\n"
		"- Register with Host::registerCommand.\n"
		"- Format: /command [args] at the start of the message.\n"
		"- Commands that contain '@' are ignored to avoid bot conflicts.\n"
		"- CommandContext provides session, history, text, command, args.\n"
		"- Return CommandResult::Handled to stop sending.\n"
		"- Return CommandResult::ReplaceText to send different text.\n\n"
		"Actions\n"
		"- Register with Host::registerAction (simple) or\n"
		"  Host::registerActionWithContext (gets window + session).\n"
		"- Actions appear in the Plugins section as buttons.\n\n"
		"Panels (UI entry points)\n"
		"- Register with Host::registerPanel.\n"
		"- Panels appear in the Plugins section; when clicked you receive a\n"
		"  Window::Controller* and can open UI (right column, box, dialog, etc).\n\n"
		"Outgoing text interceptors\n"
		"- Register with Host::registerOutgoingTextInterceptor.\n"
		"- Called for every outgoing text message (before commands).\n"
		"- Lower priority runs earlier.\n"
		"- Return Cancel / Handled / ReplaceText to stop or change sending.\n\n"
		"Message observers (backend)\n"
		"- Register with Host::registerMessageObserver.\n"
		"- Options allow new/edited/deleted and incoming/outgoing filters.\n"
		"- Current scope: active session (account) only.\n"
		"- HistoryItem pointers are only valid during the callback.\n\n"
		"Windows\n"
		"- Host::forEachWindow iterates existing windows.\n"
		"- Host::onWindowCreated notifies about new windows.\n\n"
		"Sessions\n"
		"- Host::activeSession returns the active account session.\n"
		"- Host::forEachSession iterates all loaded sessions.\n"
		"- Host::onSessionActivated notifies when the active session changes.\n\n"
		"UI access\n"
		"- Use Window::Controller methods to open layers, right column, boxes.\n"
		"- UI plugins usually link QtWidgets in addition to QtCore.\n\n"
		"Libraries and toolchain\n"
		"- Plugins are written in C++ (same language as Telegram Desktop).\n"
		"- Use QtCore (QString, QLibrary) and the C++ standard library.\n"
		"- For UI, also link QtWidgets.\n"
		"- Link against the same Qt version used by the app.\n"
		"- Build with the same compiler/ABI as Telegram Desktop.\n\n"
		"Other runtimes\n"
		"- DEX/Java plugins are not supported on desktop.\n"
		"- If you need another language, embed a runtime inside a C++ plugin\n"
		"  or communicate with a separate process via IPC.\n\n"
		"Build outline (Linux)\n"
		"- g++ -std=c++20 -fPIC -shared -I../../SourceFiles \\\n"
		"  -o my_plugin.so my_plugin.cpp "
		"$(pkg-config --cflags --libs Qt6Core Qt6Widgets)\n"
		"- mv my_plugin.so my_plugin.tgd\n\n"
		"Security\n"
		"- Plugins run as native code inside the app process.\n"
		"- Only load plugins you trust.\n"_q;
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
	return rpl::single(u"Plugins"_q);
}

void Plugins::setupContent() {
	Ui::AddDivider(_content);
	Ui::AddSkip(_content);

	const auto docs = AddButtonWithIcon(
		_content,
		rpl::single(u"Documentation"_q),
		st::settingsButton,
		{ &st::menuIconFaq });
	docs->addClickHandler([=] {
		showOther(PluginsDocumentation::Id());
	});

	const auto openFolder = AddButtonWithIcon(
		_content,
		rpl::single(u"Open Plugins Folder"_q),
		st::settingsButton,
		{ &st::menuIconShowInFolder });
	openFolder->addClickHandler([=] {
		File::ShowInFolder(Core::App().plugins().pluginsPath());
	});

	const auto reload = AddButtonWithIcon(
		_content,
		rpl::single(u"Reload Plugins"_q),
		st::settingsButton,
		{ &st::menuIconManage });
	reload->addClickHandler([=] {
		Core::App().plugins().reload();
		rebuildList();
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
		Ui::AddDividerText(
			_list,
			rpl::single(u"No plugins found in tdata/plugins."_q));
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
		const auto buttonStyle = state.error.isEmpty()
			? st::settingsButtonNoIcon
			: st::settingsOptionDisabled;
		const auto toggle = _list->add(object_ptr<Ui::SettingsButton>(
			_list,
			rpl::single(title),
			buttonStyle
		))->toggleOn(rpl::single(state.enabled));
		if (!state.error.isEmpty()) {
			toggle->setToggleLocked(true);
		}
		toggle->toggledChanges(
		) | rpl::on_next([=](bool value) {
			if (!Core::App().plugins().setEnabled(state.info.id, value)) {
				_controller->window().showToast(u"Could not change state."_q);
			}
			rebuildList();
		}, toggle->lifetime());

		const auto commands = Core::App().plugins().commandsFor(state.info.id);
		const auto actions = Core::App().plugins().actionsFor(state.info.id);
		const auto panels = Core::App().plugins().panelsFor(state.info.id);
		const auto details = FormatPluginDetails(state, commands);
		if (!details.isEmpty()) {
			Ui::AddSkip(_list);
			Ui::AddDividerText(_list, rpl::single(details));
		}

		if (!actions.empty()) {
			Ui::AddSkip(_list);
			Ui::AddDividerText(_list, rpl::single(u"Actions"_q));
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
			Ui::AddDividerText(_list, rpl::single(u"Panels"_q));
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
	return rpl::single(u"Plugin Documentation"_q);
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
