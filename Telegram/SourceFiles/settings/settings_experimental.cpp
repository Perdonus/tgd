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

struct ShellModeUiState {
	ShellModePreferences prefs;
	std::vector<std::function<void(const ShellModePreferences&)>> syncs;
};

[[nodiscard]] bool SameShellModePreferences(
		const ShellModePreferences &a,
		const ShellModePreferences &b) {
	return (a.immersiveAnimation == b.immersiveAnimation)
		&& (a.expandedSidePanel == b.expandedSidePanel)
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
	const auto current = getter(state->prefs);
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(title),
		st::settingsButtonNoIcon))->toggleOn(
		toggles->events_starting_with(current));
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

void AddShellPresetButton(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		ShellModeUiState *state,
		AstrogramShellPreset preset) {
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(ShellPresetTitle(preset)),
		st::settingsButtonNoIcon));
	button->addClickHandler([=] {
		const auto updated = ShellModePreferencesFor(preset);
		SaveShellModeState(window, state, updated, ShellPresetToast(preset));
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

	const auto shellState = container->lifetime().make_state<ShellModeUiState>();
	shellState->prefs = LoadShellModePreferences();

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Меню Astrogram и режимы оболочки",
			"Astrogram menu and shell modes")));
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Вступительный гайд теперь не только ведёт сюда напрямую, но и отдельно объясняет кастомизацию меню перед shell-шагом. Здесь вынесены быстрые runtime-переключатели для боковой панели и анимации, а ниже остаётся живой editor с preview и более глубокой раскладкой меню.",
			"The onboarding flow now not only lands here directly, but also explains menu customization before the shell step. This top area surfaces quick runtime switches for the side panel and animation, while the live editor with the preview and deeper menu layout stays below.")));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Быстрые shell-переключатели",
			"Quick shell switches")));
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Это те же runtime-hooks wide / left-edge / immersive, которые использует onboarding и слушают side menu, shell и settings/info layers. Здесь ими можно управлять напрямую, не заходя сразу в deeper editor surface.",
			"These are the same wide / left-edge / immersive runtime hooks used by onboarding and observed by the side menu, the shell and the settings/info layers. You can control them directly here without jumping into the deeper editor surface right away.")));
	AddShellModeToggle(
		window,
		container,
		shellState,
		RuEn(
			"Расширенная боковая панель",
			"Expanded side panel"),
		RuEn(
			"Шире делает реальное боковое меню и одновременно перестраивает preview-панель ниже.",
			"Makes the real side menu wider and immediately reshapes the preview panel below."),
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
			"Левоторцевые settings/info surfaces",
			"Left-edge settings/info surfaces"),
		RuEn(
			"Переставляет settings и info ближе к левому краю, чтобы оболочка ощущалась продолжением боковой панели, а не отдельным центрированным слоем.",
			"Moves settings and info closer to the left edge so the shell feels like a continuation of the side menu instead of a separate centered layer."),
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
			"Иммерсивная анимация бокового меню",
			"Immersive side menu animation"),
		RuEn(
			"Основная часть клиента уезжает вправо вместе с открытием бокового меню. Это тот же runtime-переключатель, который теперь показывается и в onboarding shell-шаге.",
			"The main part of the client shifts right together with the side menu opening. This is the same runtime switch now referenced by the onboarding shell step."),
		[](const ShellModePreferences &prefs) {
			return prefs.immersiveAnimation;
		},
		[](ShellModePreferences &prefs, bool value) {
			prefs.immersiveAnimation = value;
		});
	AddShellModeToggle(
		window,
		container,
		shellState,
		RuEn(
			"Более широкая панель настроек",
			"Wider settings pane"),
		RuEn(
			"Даёт settings/info более широкий контейнер, чтобы длинные строки и новые shell-поверхности не упирались в узкую колонку.",
			"Gives settings/info a wider container so longer rows and the newer shell surfaces do not collapse into a narrow column."),
		[](const ShellModePreferences &prefs) {
			return prefs.wideSettingsPane;
		},
		[](ShellModePreferences &prefs, bool value) {
			prefs.wideSettingsPane = value;
		});
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Shell presets и runtime-hooks",
			"Shell presets and runtime hooks")));
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Если не хочется собирать режим по одному флагу, ниже лежат те же связки одним нажатием. Они обновляют этот верхний блок сразу, а дальше тот же state подхватывает live editor ниже.",
			"If you do not want to build a mode flag by flag, the same stacks are available below in one tap. They update this top block immediately, and the live editor below picks up the same state next.")));
	AddShellPresetButton(window, container, shellState, AstrogramShellPreset::Balanced);
	AddShellPresetButton(window, container, shellState, AstrogramShellPreset::Focused);
	AddShellPresetButton(window, container, shellState, AstrogramShellPreset::Wide);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Ниже остаётся расширенный editor: side menu layout, restore-tray, footer/profile presentation и тот же preview/runtime bridge для оболочки Astrogram.",
			"The extended editor stays below: side menu layout, restore tray, footer/profile presentation and the same preview/runtime bridge for the Astrogram shell.")));
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
