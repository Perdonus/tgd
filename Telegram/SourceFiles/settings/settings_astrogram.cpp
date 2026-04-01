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
#include "styles/style_settings.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"

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

void SetupAstrogram(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();

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
	const auto openRecallLog = container->add(object_ptr<Button>(
		container,
		rpl::single(RuEn("Показать лог anti-recall", "Show anti-recall log")),
		st::settingsButtonNoIcon));
	openRecallLog->addClickHandler([] {
		File::ShowInFolder(u"./tdata/astro_recall_log.jsonl"_q);
	});

	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Функции ниже уже работают локально в клиенте Astrogram и не требуют отдельных плагинов.",
			"These features already run in the Astrogram client and do not require separate plugins.")));

	Q_UNUSED(controller);
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
