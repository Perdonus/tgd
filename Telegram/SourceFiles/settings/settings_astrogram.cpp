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

namespace Settings {
namespace {

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
			"Здесь открывается менеджер плагинов Astrogram. Системные действия плагинов, документация, runtime API, безопасный режим и папка плагинов вынесены в меню с тремя точками внутри раздела.",
			"This opens the Astrogram plugin manager. Plugin system actions, documentation, runtime API, safe mode and the plugins folder live in the three-dots menu inside that section.")));

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
