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

void AddOption(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<bool> &option,
		rpl::producer<> resetClicks) {
	auto &lifetime = container->lifetime();
	const auto name = option.name().isEmpty() ? option.id() : option.name();
	const auto toggles = lifetime.make_state<rpl::event_stream<bool>>();
	std::move(
		resetClicks
	) | rpl::map_to(
		option.defaultValue()
	) | rpl::start_to_stream(*toggles, lifetime);

	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(name),
		(option.relevant()
			? st::settingsButtonNoIcon
			: st::settingsOptionDisabled)
	))->toggleOn(toggles->events_starting_with(option.value()));

	const auto restarter = (option.relevant() && option.restartRequired())
		? button->lifetime().make_state<base::Timer>()
		: nullptr;
	if (restarter) {
		restarter->setCallback([=] {
			window->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(),
				.confirmed = [] { Core::Restart(); },
				.confirmText = tr::lng_settings_restart_now(),
				.cancelText = tr::lng_settings_restart_later(),
			}));
		});
	}
	button->toggledChanges(
	) | rpl::on_next([=, &option](bool toggled) {
		if (!option.relevant() && toggled != option.defaultValue()) {
			toggles->fire_copy(option.defaultValue());
			window->showToast(
				tr::lng_settings_experimental_irrelevant(tr::now));
			return;
		}
		option.set(toggled);
		if (restarter) {
			restarter->callOnce(st::settingsButtonNoIcon.toggle.duration);
		}
	}, container->lifetime());

	const auto &description = option.description();
	if (!description.isEmpty()) {
		Ui::AddSkip(container, st::settingsCheckboxesSkip);
		Ui::AddDividerText(container, rpl::single(description));
		Ui::AddSkip(container, st::settingsCheckboxesSkip);
	}
}

[[nodiscard]] QString ShellPresetTitle(AstrogramShellPreset preset) {
	switch (preset) {
	case AstrogramShellPreset::Balanced: return RuEn(
		"Сбалансированный shell-пресет",
		"Balanced shell preset");
	case AstrogramShellPreset::Focused: return RuEn(
		"Сфокусированный shell-пресет",
		"Focused shell preset");
	case AstrogramShellPreset::Wide: return RuEn(
		"Широкий + left-edge shell-пресет",
		"Wide + left-edge shell preset");
	}
	return QString();
}

[[nodiscard]] QString ShellPresetDescription(AstrogramShellPreset preset) {
	switch (preset) {
	case AstrogramShellPreset::Balanced: return RuEn(
		"Возвращает базовую shell-связку: immersive animation остаётся включённой, а wide/left-edge/expanded возвращаются к спокойному дефолту.",
		"Returns the baseline shell stack: immersive animation stays on, while wide/left-edge/expanded go back to the calmer default state.");
	case AstrogramShellPreset::Focused: return RuEn(
		"Явный runtime-hook для более плотного shell: включает расширенную боковую панель, но оставляет settings/info в центрированном режиме.",
		"An explicit runtime hook for a denser shell: turns on the expanded side panel while keeping settings/info in the centered mode.");
	case AstrogramShellPreset::Wide: return RuEn(
		"Явно включает весь широкий набор runtime-hook'ов сразу: expanded side panel, left-edge settings и wide settings pane. Иммерсивная анимация тоже остаётся активной.",
		"Explicitly enables the full wide runtime hook stack at once: expanded side panel, left-edge settings and the wide settings pane. Immersive animation stays active too.");
	}
	return QString();
}

[[nodiscard]] QString ShellPresetToast(AstrogramShellPreset preset) {
	switch (preset) {
	case AstrogramShellPreset::Balanced: return RuEn(
		"Сбалансированный shell-пресет применён.",
		"Balanced shell preset applied.");
	case AstrogramShellPreset::Focused: return RuEn(
		"Сфокусированный shell-пресет применён.",
		"Focused shell preset applied.");
	case AstrogramShellPreset::Wide: return RuEn(
		"Широкий shell-пресет применён.",
		"Wide shell preset applied.");
	}
	return QString();
}

void AddShellPresetButton(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		AstrogramShellPreset preset) {
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(ShellPresetTitle(preset)),
		st::settingsButtonNoIcon));
	button->addClickHandler([=] {
		if (!ApplyAstrogramShellPreset(preset)) {
			window->showToast(RuEn(
				"Не удалось применить shell preset.",
				"Could not apply the shell preset."));
			return;
		}
		window->showToast(ShellPresetToast(preset));
	});
	Ui::AddDividerText(container, rpl::single(ShellPresetDescription(preset)));
}

void SetupExperimental(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto window = &controller->window();
	Ui::AddSkip(container, st::settingsCheckboxesSkip);

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_settings_experimental_about(),
			st::boxLabel),
		st::defaultBoxDividerLabelPadding);

	auto reset = (Button*)nullptr;
	if (base::options::changed()) {
		const auto wrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();
		Ui::AddDivider(inner);
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
		reset = inner->add(object_ptr<Button>(
			inner,
			tr::lng_settings_experimental_restore(),
			st::settingsButtonNoIcon));
		reset->addClickHandler([=] {
			base::options::reset();
			wrap->hide(anim::type::normal);
		});
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
	}

	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);

	const auto addToggle = [&](const char name[]) {
		AddOption(
			window,
			container,
			base::options::lookup<bool>(name),
			(reset
				? (reset->clicks() | rpl::to_empty)
				: rpl::producer<>()));
	};

	addToggle(ChatHelpers::kOptionTabbedPanelShowOnClick);
	addToggle(Dialogs::kOptionForumHideChatsList);
	addToggle(Core::kOptionFractionalScalingEnabled);
	addToggle(Core::kOptionHighDpiDownscale);
	addToggle(Window::kOptionViewProfileInChatsListContextMenu);
	addToggle(Info::Profile::kOptionShowPeerIdBelowAbout);
	addToggle(Info::Profile::kOptionShowChannelJoinedBelowAbout);
	addToggle(Ui::kOptionUseSmallMsgBubbleRadius);
	addToggle(Media::Player::kOptionDisableAutoplayNext);
	addToggle(kOptionSendLargePhotos);
	addToggle(Webview::kOptionWebviewDebugEnabled);
	addToggle(Webview::kOptionWebviewLegacyEdge);
	addToggle(kOptionAutoScrollInactiveChat);
	addToggle(Window::Notifications::kOptionHideReplyButton);
	addToggle(Window::Notifications::kOptionCustomNotification);
	addToggle(Window::Notifications::kOptionGNotification);
	addToggle(Core::kOptionFreeType);
	addToggle(Core::kOptionSkipUrlSchemeRegister);
	addToggle(Core::kOptionDeadlockDetector);
	addToggle(Data::kOptionExternalVideoPlayer);
	addToggle(Window::kOptionNewWindowsSizeAsFirst);
	addToggle(MTP::details::kOptionPreferIPv6);
	if (base::options::lookup<bool>(kOptionFastButtonsMode).value()) {
		addToggle(kOptionFastButtonsMode);
	}
	addToggle(Window::kOptionDisableTouchbar);
	addToggle(Info::kAlternativeScrollProcessing);
	addToggle(kModerateCommonGroups);

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Меню Astrogram и режимы оболочки",
			"Astrogram menu and shell modes")));
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Вступительный гайд теперь не только ведёт сюда напрямую, но и умеет заранее применять стартовые shell-пресеты. Ниже живёт editor бокового меню и preview/runtime-связка для иммерсивной анимации, расширенной боковой панели, левоторцевых настроек и широкого контейнера settings. Открытое боковое меню тоже подхватывает эти файлы live, без ручного переоткрытия.",
			"The onboarding flow can now lead here directly and pre-apply starter shell presets. Below lives the side menu editor and the preview/runtime bridge for immersive animation, the expanded side panel, left-edge settings and a wider settings container. The opened side menu now hot-reloads these files as well, without a manual reopen.")));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Shell presets и runtime-hooks",
			"Shell presets and runtime hooks")));
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Это явные входы в runtime-хуки wide / left-edge / immersive без onboarding: пресеты сразу пишут в тот же preview/runtime файл, который уже слушают side menu, shell и settings/info layers.",
			"These are explicit entry points into the wide / left-edge / immersive runtime hooks without onboarding: the presets write directly into the same preview/runtime file already observed by the side menu, the shell and the settings/info layers.")));
	AddShellPresetButton(window, container, AstrogramShellPreset::Balanced);
	AddShellPresetButton(window, container, AstrogramShellPreset::Focused);
	AddShellPresetButton(window, container, AstrogramShellPreset::Wide);
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
