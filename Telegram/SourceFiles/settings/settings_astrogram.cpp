/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_astrogram.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "core/core_settings.h"
#include "lang/lang_instance.h"
#include "settings/settings_common.h"
#include "settings/settings_plugins.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <optional>

namespace Settings {
namespace {

struct PluginShortcutSpec {
	QString id;
	const char *titleRu = nullptr;
	const char *titleEn = nullptr;
	const char *descriptionRu = nullptr;
	const char *descriptionEn = nullptr;
};

[[nodiscard]] bool IsRussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return IsRussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString PluginsLabel() {
	const auto count = int(Core::App().plugins().plugins().size());
	if (Core::App().plugins().safeModeEnabled()) {
		return RuEn("Безопасный режим", "Safe mode");
	}
	return IsRussianUi()
		? QString::fromUtf8("%1 плагинов").arg(count)
		: QString::fromUtf8("%1 plugins").arg(count);
}

[[nodiscard]] std::optional<::Plugins::PluginState> LookupPlugin(
		const QString &pluginId) {
	for (const auto &state : Core::App().plugins().plugins()) {
		if (state.info.id == pluginId) {
			return state;
		}
	}
	return std::nullopt;
}

template <typename Producer, typename Callback>
void AddToggle(
		not_null<Ui::VerticalLayout*> container,
		Producer value,
		const QString &label,
		Callback callback) {
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(label),
		st::settingsButtonNoIcon));
	button->toggleOn(std::move(value));
	button->toggledChanges(
	) | rpl::on_next([=](bool toggled) {
		callback(toggled);
		Core::App().saveSettingsDelayed();
	}, button->lifetime());
}

template <typename Callback>
void AddActionButtonWithLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &label,
		Callback callback,
		IconDescriptor descriptor = {}) {
	AddButtonWithLabel(
		container,
		rpl::single(title),
		rpl::single(label),
		st::settingsButton,
		std::move(descriptor)
	)->addClickHandler(std::move(callback));
}

void AddPluginShortcut(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		const PluginShortcutSpec &spec) {
	const auto state = LookupPlugin(spec.id);
	const auto title = IsRussianUi()
		? QString::fromUtf8(spec.titleRu)
		: QString::fromUtf8(spec.titleEn);
	const auto label = [&] {
		if (state) {
			auto version = state->info.version.trimmed();
			if (version.isEmpty()) {
				version = RuEn("Установлен", "Installed");
			}
			return state->loaded
				? version
				: version + u" • "_q + RuEn("метаданные", "metadata");
		}
		return RuEn("Открыть менеджер", "Open manager");
	}();
	AddActionButtonWithLabel(
		container,
		title,
		label,
		[=] {
			if (state) {
				controller->showSettings(PluginDetailsId(state->info.id));
			} else {
				controller->showSettings(Plugins::Id());
			}
		},
		{ &st::menuIconCustomize });
	Ui::AddDividerText(
		container,
		rpl::single(IsRussianUi()
			? QString::fromUtf8(spec.descriptionRu)
			: QString::fromUtf8(spec.descriptionEn)));
}

void SetupAstrogram(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();

	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Astrogram объединяет клиентские фишки, anti-recall, ghost mode и систему плагинов. Основная точка входа для расширений теперь находится здесь.",
			"Astrogram combines client-side features, anti-recall, ghost mode and the plugin system. The main entry point for extensions now lives here.")));

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Расширения", "Extensions")));
	AddActionButtonWithLabel(
		container,
		RuEn("Плагины", "Plugins"),
		PluginsLabel(),
		[=] { controller->showSettings(Plugins::Id()); },
		{ &st::menuIconCustomize });
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Здесь открывается менеджер плагинов Astrogram. Системные действия плагинов, документация, runtime API, безопасный режим, логи и папка плагинов вынесены в меню с тремя точками внутри раздела.",
			"This opens the Astrogram plugin manager. Plugin system actions, documentation, runtime API, safe mode, logs and the plugins folder live in the three-dots menu inside that section.")));

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Быстрые входы", "Quick Access")));
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"example.transparent_telegram"_q,
			.titleRu = "AstroTransparent",
			.titleEn = "AstroTransparent",
			.descriptionRu = "Прозрачность интерфейса, сообщений и текста в отдельной странице плагина.",
			.descriptionEn = "Interface, message and text transparency in a dedicated plugin page.",
		});
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"astro.blur_telegram"_q,
			.titleRu = "Blur Telegram",
			.titleEn = "Blur Telegram",
			.descriptionRu = "Живой blur крупных поверхностей Astrogram с настройкой силы эффекта.",
			.descriptionEn = "Live blur for major Astrogram surfaces with strength controls.",
		});
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"astro.accent_color"_q,
			.titleRu = "Accent Color",
			.titleEn = "Accent Color",
			.descriptionRu = "Accent-палитра и тонировка поверхностей для более выразительного оформления.",
			.descriptionEn = "Accent palette and surface tinting for a more expressive appearance.",
		});
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"astro.font_tuner"_q,
			.titleRu = "Font Tuner",
			.titleEn = "Font Tuner",
			.descriptionRu = "Масштаб шрифта и загрузка кастомных шрифтов по ссылке или из файла.",
			.descriptionEn = "Font scaling and custom font loading from a URL or a local file.",
		});
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"sosiskibot.ai_chat"_q,
			.titleRu = "AI Chat",
			.titleEn = "AI Chat",
			.descriptionRu = "Перехватывает /ai и открывает встроенный чат с ИИ на sosiskibot.ru/api.",
			.descriptionEn = "Intercepts /ai and opens the built-in AI chat backed by sosiskibot.ru/api.",
		});
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"astro.ayu_safe"_q,
			.titleRu = "AyuSafe",
			.titleEn = "AyuSafe",
			.descriptionRu = "Приватность, anti-recall и утилиты в стиле AyuGram внутри Astrogram.",
			.descriptionEn = "Privacy, anti-recall and utility controls in an AyuGram-style package.",
		});

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Приватность", "Privacy")));
	AddToggle(
		container,
		settings.ghostModeValue(),
		RuEn("Режим призрака", "Ghost mode"),
		[&](bool toggled) { settings.setGhostMode(toggled); });
	AddToggle(
		container,
		settings.localPremiumValue(),
		RuEn("Локальный Premium", "Local Premium"),
		[&](bool toggled) { settings.setLocalPremium(toggled); });

	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Антиудаление", "Anti-recall")));
	AddToggle(
		container,
		settings.saveDeletedMessagesValue(),
		RuEn("Сохранять удалённые сообщения", "Keep deleted messages"),
		[&](bool toggled) { settings.setSaveDeletedMessages(toggled); });
	AddToggle(
		container,
		settings.saveMessagesHistoryValue(),
		RuEn("Сохранять историю правок", "Keep edit history"),
		[&](bool toggled) { settings.setSaveMessagesHistory(toggled); });
	AddToggle(
		container,
		settings.semiTransparentDeletedMessagesValue(),
		RuEn("Полупрозрачные удалённые сообщения", "Semi-transparent deleted messages"),
		[&](bool toggled) { settings.setSemiTransparentDeletedMessages(toggled); });
	AddActionButtonWithLabel(
		container,
		RuEn("Показать лог anti-recall", "Show anti-recall log"),
		QString::fromUtf8("astro_recall_log.jsonl"),
		[] { File::ShowInFolder(u"./tdata/astro_recall_log.jsonl"_q); });

	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Часть функций Astrogram встроена прямо в клиент, а визуальные и экспериментальные расширения живут через плагины.",
			"Some Astrogram features are built directly into the client, while visual and experimental extensions live through plugins.")));
}

} // namespace

Astrogram::Astrogram(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Astrogram::title() {
	return rpl::single(u"Astrogram"_q);
}

void Astrogram::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogram(controller, content);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
