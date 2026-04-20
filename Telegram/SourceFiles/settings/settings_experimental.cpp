/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_experimental.h"

#include "settings/settings_menu_customization_editor.h"
#include "menu/menu_customization.h"
#include "data/components/passkeys.h"
#include "main/main_session.h"
#include "plugins/plugins_manager.h"
#include "ui/boxes/confirm_box.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "ui/gl/gl_detection.h"
#include "ui/chat/chat_style_radius.h"
#include "base/options.h"
#include "boxes/moderate_messages_box.h"
#include "core/application.h"
#include "core/launcher.h"
#include "core/sandbox.h"
#include "chat_helpers/tabbed_panel.h"
#include "dialogs/dialogs_widget.h"
#include "history/history_item_components.h"
#include "info/profile/info_profile_actions.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "media/player/media_player_instance.h"
#include "mtproto/session_private.h"
#include "webview/webview_embed.h"
#include "window/main_window.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "window/notifications_manager.h"
#include "storage/localimageloader.h"
#include "data/data_document_resolver.h"
#include "info/info_flexible_scroll.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <functional>
#include <vector>

namespace Settings {

ShellModePreferences ShellModePreferencesFor(AstrogramShellPreset preset) {
	auto result = ShellModePreferences();
	switch (preset) {
	case AstrogramShellPreset::Balanced:
		break;
	case AstrogramShellPreset::Focused:
		result.expandedSidePanel = true;
		break;
	case AstrogramShellPreset::Wide:
		result.expandedSidePanel = true;
		result.leftEdgeSettings = true;
		result.wideSettingsPane = true;
		break;
	}
	return result;
}

bool ApplyAstrogramShellPreset(AstrogramShellPreset preset) {
	return SaveShellModePreferences(ShellModePreferencesFor(preset));
}

namespace {

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

struct ShellModeUiState {
	ShellModePreferences prefs;
	std::vector<std::function<void(const ShellModePreferences&)>> syncs;
};

[[nodiscard]] bool SameShellModePreferences(
		const ShellModePreferences &a,
		const ShellModePreferences &b) {
	return (a.expandedSidePanel == b.expandedSidePanel)
		&& (a.leftEdgeSettings == b.leftEdgeSettings)
		&& (a.wideSettingsPane == b.wideSettingsPane);
}

void NotifyShellModeState(ShellModeUiState *state) {
	for (const auto &sync : state->syncs) {
		sync(state->prefs);
	}
}

[[nodiscard]] bool SaveShellModeState(
		not_null<Window::Controller*> window,
		ShellModeUiState *state,
		const ShellModePreferences &updated,
		QString successToast = QString()) {
	if (SameShellModePreferences(updated, state->prefs)) {
		if (!successToast.isEmpty()) {
			window->showToast(successToast);
		}
		return true;
	}
	if (!SaveShellModePreferences(updated)) {
		window->showToast(RuEn(
			"Не удалось сохранить параметры оболочки Astrogram.",
			"Could not save Astrogram shell settings."));
		return false;
	}
	state->prefs = updated;
	NotifyShellModeState(state);
	if (!successToast.isEmpty()) {
		window->showToast(successToast);
	}
	return true;
}

template <typename Getter, typename Setter>
void AddShellModeToggle(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		ShellModeUiState *state,
		QString title,
		QString description,
		Getter getter,
	Setter setter) {
	const auto toggles = container->lifetime().make_state<rpl::event_stream<bool>>();
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(title),
		st::settingsButtonNoIcon))->toggleOn(
		toggles->events_starting_with(getter(state->prefs)));
	state->syncs.push_back([=](const ShellModePreferences &prefs) {
		toggles->fire_copy(getter(prefs));
	});
	button->toggledChanges() | rpl::on_next([=](bool toggled) {
		auto updated = state->prefs;
		setter(updated, toggled);
		if (!SaveShellModeState(window, state, updated)) {
			toggles->fire_copy(getter(state->prefs));
		}
	}, container->lifetime());
	if (!description.isEmpty()) {
		Ui::AddSkip(container, st::settingsCheckboxesSkip);
		Ui::AddDividerText(container, rpl::single(description));
		Ui::AddSkip(container, st::settingsCheckboxesSkip);
	}
}

void SetupExperimental(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto window = &controller->window();
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Экспериментальные настройки Astrogram",
			"Astrogram experimental settings")));

	const auto shellState = container->lifetime().make_state<ShellModeUiState>();
	shellState->prefs = LoadShellModePreferences();

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Общие функции",
			"General features")));
	AddShellModeToggle(
		window,
		container,
		shellState,
		RuEn(
			"Расширенная боковая панель",
			"Expanded side panel"),
		QString(),
		[](const ShellModePreferences &prefs) {
			return prefs.expandedSidePanel;
		},
		[](ShellModePreferences &prefs, bool value) {
			prefs.expandedSidePanel = value;
		});
	AddShellModeToggle(
		window,
		container,
		shellState,
		RuEn(
			"Всплывающие окна от левого края",
			"Left-edge popup windows"),
		QString(),
		[](const ShellModePreferences &prefs) {
			return prefs.leftEdgeSettings;
		},
		[](ShellModePreferences &prefs, bool value) {
			prefs.leftEdgeSettings = value;
		});
	AddShellModeToggle(
		window,
		container,
		shellState,
		RuEn(
			"Более широкая панель настроек",
			"Wider settings pane"),
		QString(),
		[](const ShellModePreferences &prefs) {
			return prefs.wideSettingsPane;
		},
		[](ShellModePreferences &prefs, bool value) {
			prefs.wideSettingsPane = value;
		});
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Настройка меню и панелей",
			"Menus and panels")));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddMenuCustomizationEditor(controller, container);
}

} // namespace

Experimental::Experimental(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Experimental::title() {
	return tr::lng_settings_experimental();
}

void Experimental::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	SetupExperimental(controller, content);

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
