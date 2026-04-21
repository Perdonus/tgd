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
#include "ui/wrap/vertical_layout_reorder.h"
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
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
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

void AddThreeDotsMenuCustomizationEditor(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);

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

struct SideMenuUiState {
	::Menu::Customization::SideMenuOptions options;
	std::vector<std::function<void(
		const ::Menu::Customization::SideMenuOptions&)>> syncs;
};

struct ShellLayoutOrderUiState {
	QStringList order;
	std::vector<std::function<void(const QStringList&)>> syncs;
};

[[nodiscard]] QString ShellLayoutOrderPath() {
	return cWorkingDir() + QStringLiteral("tdata/shell_layout_order.json");
}

[[nodiscard]] QStringList DefaultShellLayoutOrder() {
	return {
		QStringLiteral("folders"),
		QStringLiteral("dialogs"),
		QStringLiteral("chat"),
		QStringLiteral("info"),
	};
}

[[nodiscard]] QStringList NormalizeShellLayoutOrder(QStringList order) {
	const auto defaults = DefaultShellLayoutOrder();
	QStringList result;
	for (const auto &id : order) {
		if (defaults.contains(id) && !result.contains(id)) {
			result.push_back(id);
		}
	}
	for (const auto &id : defaults) {
		if (!result.contains(id)) {
			result.push_back(id);
		}
	}
	return result;
}

[[nodiscard]] QStringList LoadShellLayoutOrder() {
	auto file = QFile(ShellLayoutOrderPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return DefaultShellLayoutOrder();
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return DefaultShellLayoutOrder();
	}
	auto values = QStringList();
	for (const auto &value : document.object().value(
			QStringLiteral("order")).toArray()) {
		const auto id = value.toString().trimmed();
		if (!id.isEmpty()) {
			values.push_back(id);
		}
	}
	return NormalizeShellLayoutOrder(values);
}

[[nodiscard]] bool SaveShellLayoutOrder(const QStringList &order) {
	const auto path = ShellLayoutOrderPath();
	const auto directory = QFileInfo(path).absolutePath();
	if (!directory.isEmpty() && !QDir().mkpath(directory)) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	auto array = QJsonArray();
	for (const auto &id : NormalizeShellLayoutOrder(order)) {
		array.push_back(id);
	}
	const auto document = QJsonDocument(QJsonObject{
		{ QStringLiteral("order"), array },
	});
	if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

[[nodiscard]] QString ShellLayoutOrderItemLabel(const QString &id) {
	if (id == QStringLiteral("folders")) {
		return RuEn("Папки", "Folders");
	} else if (id == QStringLiteral("dialogs")) {
		return RuEn("Чаты", "Chats");
	} else if (id == QStringLiteral("chat")) {
		return RuEn("Чат", "Chat");
	} else if (id == QStringLiteral("info")) {
		return RuEn("Инфо", "Info");
	}
	return id;
}

[[nodiscard]] style::margins ExperimentalSectionTitlePadding() {
	return QMargins(
		st::boxRowPadding.left() - st::defaultSubsectionTitlePadding.left(),
		0,
		0,
		0);
}

[[nodiscard]] not_null<Ui::FlatLabel*> AddExperimentalBlueTitle(
		not_null<Ui::VerticalLayout*> container,
		const QString &title) {
	const auto label = Ui::AddSubsectionTitle(
		container,
		rpl::single(title),
		ExperimentalSectionTitlePadding());
	label->setTextColorOverride(st::windowActiveTextFg->c);
	return label;
}

void AddExperimentalSectionHeader(
		not_null<Ui::VerticalLayout*> container,
		const QString &title) {
	Ui::AddDivider(container);
	AddExperimentalBlueTitle(container, title);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
}

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

void NotifySideMenuState(SideMenuUiState *state) {
	for (const auto &sync : state->syncs) {
		sync(state->options);
	}
}

void NotifyShellLayoutOrderState(ShellLayoutOrderUiState *state) {
	for (const auto &sync : state->syncs) {
		sync(state->order);
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

[[nodiscard]] bool SaveSideMenuState(
		not_null<Window::Controller*> window,
		SideMenuUiState *state,
		const ::Menu::Customization::SideMenuOptions &updated) {
	if ((updated.showFooterText == state->options.showFooterText)
		&& (updated.profileBlockPosition == state->options.profileBlockPosition)) {
		return true;
	}
	if (!::Menu::Customization::SaveSideMenuOptions(updated)) {
		window->showToast(RuEn(
			"Не удалось сохранить параметры боковой панели Astrogram.",
			"Could not save Astrogram side panel settings."));
		return false;
	}
	state->options = ::Menu::Customization::LoadSideMenuOptions();
	NotifySideMenuState(state);
	return true;
}

[[nodiscard]] bool SaveShellLayoutOrderState(
		not_null<Window::Controller*> window,
		ShellLayoutOrderUiState *state,
		const QStringList &updated) {
	const auto normalized = NormalizeShellLayoutOrder(updated);
	if (normalized == state->order) {
		return true;
	}
	if (!SaveShellLayoutOrder(normalized)) {
		window->showToast(RuEn(
			"Не удалось сохранить порядок блоков Astrogram.",
			"Could not save Astrogram block order."));
		return false;
	}
	state->order = normalized;
	NotifyShellLayoutOrderState(state);
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

template <typename Getter, typename Setter>
void AddSideMenuToggle(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		SideMenuUiState *state,
		QString title,
		Getter getter,
		Setter setter) {
	const auto toggles = container->lifetime().make_state<rpl::event_stream<bool>>();
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(title),
		st::settingsButtonNoIcon))->toggleOn(
		toggles->events_starting_with(getter(state->options)));
	state->syncs.push_back([=](const ::Menu::Customization::SideMenuOptions &options) {
		toggles->fire_copy(getter(options));
	});
	button->toggledChanges() | rpl::on_next([=](bool toggled) {
		auto updated = state->options;
		setter(updated, toggled);
		if (!SaveSideMenuState(window, state, updated)) {
			toggles->fire_copy(getter(state->options));
		}
	}, container->lifetime());
}

void ApplyShellLayoutOrder(
		not_null<Window::SessionController*> controller,
		not_null<Window::Controller*> window,
		ShellLayoutOrderUiState *state,
		const QStringList &updated) {
	if (!SaveShellLayoutOrderState(window, state, updated)) {
		return;
	}
	controller->updateColumnLayout();
	controller->window().widget()->updateControlsGeometry();
	controller->widget()->updateControlsGeometry();
}

void AddShellLayoutOrderEditor(
		not_null<Window::SessionController*> controller,
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		ShellLayoutOrderUiState *state) {
	const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto reorder = container->lifetime().make_state<Ui::VerticalLayoutReorder>(
		list);
	const auto rebuilding = container->lifetime().make_state<bool>(false);
	const auto rebuild = [=] {
		*rebuilding = true;
		reorder->cancel();
		list->clear();
		auto index = 1;
		for (const auto &id : NormalizeShellLayoutOrder(state->order)) {
			const auto title = QString::number(index++)
				+ QStringLiteral(". ")
				+ ShellLayoutOrderItemLabel(id);
			const auto button = list->add(object_ptr<Button>(
				list,
				rpl::single(title),
				st::settingsButtonNoIcon));
			button->setProperty("astro_shell_block_id", id);
		}
		*rebuilding = false;
	};
	reorder->updates() | rpl::on_next([=](Ui::VerticalLayoutReorder::Single data) {
		using ReorderState = Ui::VerticalLayoutReorder::State;
		if ((data.state != ReorderState::Applied) || *rebuilding) {
			return;
		}
		auto updated = QStringList();
		updated.reserve(list->count());
		for (auto i = 0; i != list->count(); ++i) {
			const auto id = list->widgetAt(i)->property(
				"astro_shell_block_id").toString().trimmed();
			if (!id.isEmpty()) {
				updated.push_back(id);
			}
		}
		ApplyShellLayoutOrder(controller, window, state, updated);
	}, list->lifetime());
	state->syncs.push_back([=](const QStringList &) {
		if (!*rebuilding) {
			rebuild();
		}
	});
	rebuild();
	container->add(object_ptr<Button>(
		container,
		rpl::single(RuEn(
			"Сбросить порядок блоков",
			"Reset block order")),
		st::settingsButtonNoIcon))->clicks() | rpl::on_next([=](Qt::MouseButton) {
		ApplyShellLayoutOrder(
			controller,
			window,
			state,
			DefaultShellLayoutOrder());
	}, container->lifetime());
}

void SetupExperimental(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto window = &controller->window();

	const auto shellState = container->lifetime().make_state<ShellModeUiState>();
	const auto sideMenuState = container->lifetime().make_state<SideMenuUiState>();
	const auto shellOrderState = container->lifetime().make_state<ShellLayoutOrderUiState>();
	shellState->prefs = LoadShellModePreferences();
	sideMenuState->options = ::Menu::Customization::LoadSideMenuOptions();
	shellOrderState->order = LoadShellLayoutOrder();

	AddExperimentalSectionHeader(
		container,
		RuEn("Окно и оболочка", "Window and shell"));
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
	AddExperimentalSectionHeader(
		container,
		RuEn("Боковая панель", "Side panel"));
	AddSideMenuToggle(
		window,
		container,
		sideMenuState,
		RuEn(
			"Показывать footer-текст Astrogram Desktop и версию",
			"Show Astrogram Desktop footer text and version"),
		[](const ::Menu::Customization::SideMenuOptions &options) {
			return options.showFooterText;
		},
		[](::Menu::Customization::SideMenuOptions &options, bool value) {
			options.showFooterText = value;
		});
	AddSideMenuToggle(
		window,
		container,
		sideMenuState,
		RuEn(
			"Профиль и список аккаунтов снизу",
			"Move profile and account list to the bottom"),
		[](const ::Menu::Customization::SideMenuOptions &options) {
			return options.profileBlockPosition
				== QString::fromLatin1(
					::Menu::Customization::SideMenuProfileBlockPositionId::Bottom);
		},
		[](::Menu::Customization::SideMenuOptions &options, bool value) {
			options.profileBlockPosition = value
				? QString::fromLatin1(
					::Menu::Customization::SideMenuProfileBlockPositionId::Bottom)
				: QString::fromLatin1(
					::Menu::Customization::SideMenuProfileBlockPositionId::Top);
		});

	AddExperimentalSectionHeader(
		container,
		RuEn("Редакторы меню", "Menu editors"));
	AddMenuCustomizationEditor(controller, container);
	AddThreeDotsMenuCustomizationEditor(controller, container);

	AddExperimentalSectionHeader(
		container,
		RuEn(
			"Порядок экранов",
			"Screen order"));
	AddShellLayoutOrderEditor(
		controller,
		window,
		container,
		shellOrderState);
}

} // namespace

QStringList LoadAstrogramShellLayoutOrder() {
	return LoadShellLayoutOrder();
}

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
