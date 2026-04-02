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
#include "core/version.h"
#include "lang/lang_instance.h"
#include "plugins/plugins_manager.h"
#include "settings/settings_common.h"
#include "settings/settings_plugins.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <optional>
#include <QImage>
#include <QPainterPath>
#include <QTextOption>

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

[[nodiscard]] QImage AstrogramHeaderImage() {
	static const auto image = [] {
		auto result = QImage(u":/gui/art/astrogram/settings_avatar.jpg"_q);
		if (result.isNull()) {
			result = QImage(u":/gui/art/logo_256_no_margin.png"_q);
		}
		return result;
	}();
	return image;
}

[[nodiscard]] QRectF CenterCropSourceRect(const QImage &image) {
	if (image.isNull()) {
		return {};
	}
	if (image.width() == image.height()) {
		return QRectF(0, 0, image.width(), image.height());
	}
	if (image.width() > image.height()) {
		const auto side = image.height();
		const auto left = (image.width() - side) / 2.0;
		return QRectF(left, 0, side, side);
	}
	const auto side = image.width();
	const auto top = (image.height() - side) / 2.0;
	return QRectF(0, top, side, side);
}

[[nodiscard]] QString AstrogramVersionText() {
	return RuEn("Версия %1", "Version %1").arg(QString::fromLatin1(AppVersionStr));
}

[[nodiscard]] QString AstrogramHeaderDescription() {
	return RuEn(
		"Здесь собраны основные возможности Astrogram: приватность, защита от удаления, оформление и вся система плагинов.",
		"Astrogram keeps its core client features, privacy tools, anti-recall options, appearance controls and the plugin system here.");
}

void AddAstrogramHeader(not_null<Ui::VerticalLayout*> container) {
	const auto header = container->add(object_ptr<Ui::RpWidget>(container));
	const auto raw = header;
	raw->setMinimumHeight(248);
	raw->setMaximumHeight(248);

	raw->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(raw);
		const auto avatar = AstrogramHeaderImage();
		const auto width = raw->width();
		const auto avatarSize = st::settingsCloudPasswordIconSize;
		const auto avatarRect = QRect(
			(width - avatarSize) / 2,
			12,
			avatarSize,
			avatarSize);

		if (!avatar.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			QPainterPath path;
			path.addRoundedRect(
				QRectF(avatarRect),
				avatarRect.width() / 2.0,
				avatarRect.height() / 2.0);
			p.save();
			p.setClipPath(path);
			p.drawImage(avatarRect, avatar, CenterCropSourceRect(avatar));
			p.restore();
		}

		const auto titleTop = avatarRect.bottom() + 24;
		p.setPen(st::windowFg);
		p.setFont(st::semiboldFont);
		p.drawText(
			QRect(24, titleTop, width - 48, st::semiboldFont->height + 8),
			Qt::AlignHCenter | Qt::TextSingleLine,
			u"Astrogram"_q);

		const auto versionTop = titleTop + st::semiboldFont->height + 8;
		p.setPen(st::windowSubTextFg);
		p.setFont(st::defaultFlatLabel.style.font);
		p.drawText(
			QRect(24, versionTop, width - 48, st::defaultFlatLabel.style.font->height + 6),
			Qt::AlignHCenter | Qt::TextSingleLine,
			AstrogramVersionText());

		const auto descriptionTop = versionTop
			+ st::defaultFlatLabel.style.font->height
			+ 12;
		p.drawText(
			QRect(36, descriptionTop, width - 72, 64),
			AstrogramHeaderDescription(),
			QTextOption(Qt::AlignHCenter | Qt::AlignTop));
	}, raw->lifetime());
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
	const auto description = IsRussianUi()
		? QString::fromUtf8(spec.descriptionRu)
		: QString::fromUtf8(spec.descriptionEn);
	if (!description.trimmed().isEmpty()) {
		Ui::AddDividerText(container, rpl::single(description));
	}
}

void SetupAstrogram(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();

	AddAstrogramHeader(container);
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Плагины", "Extensions")));
	AddActionButtonWithLabel(
		container,
		RuEn("Плагины", "Plugins"),
		PluginsLabel(),
		[=] { controller->showSettings(Plugins::Id()); },
		{ &st::menuIconCustomize });
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Здесь открывается менеджер плагинов Astrogram. Документация, встроенный API плагинов, безопасный режим, журнал и папка плагинов вынесены в меню с тремя точками внутри раздела.",
			"This opens the Astrogram plugin manager. Plugin system actions, documentation, runtime API, safe mode, logs and the plugins folder live in the three-dots menu inside that section.")));

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Быстрый доступ", "Quick Access")));
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
			.descriptionRu = "Живое размытие крупных элементов Astrogram с настройкой силы эффекта.",
			.descriptionEn = "Live blur for major Astrogram surfaces with strength controls.",
		});
	AddPluginShortcut(
		controller,
		container,
		{
			.id = u"astro.accent_color"_q,
			.titleRu = "Accent Color",
			.titleEn = "Accent Color",
			.descriptionRu = "Акцентные цвета и тонировка поверхностей для более выразительного оформления.",
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
			.descriptionRu = "",
			.descriptionEn = "",
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
		RuEn("Режим невидимки", "Ghost mode"),
		[&](bool toggled) { settings.setGhostMode(toggled); });
	AddToggle(
		container,
		settings.ghostHideReadMessagesValue(),
		RuEn("Не читать сообщения", "Don't read messages"),
		[&](bool toggled) { settings.setGhostHideReadMessages(toggled); });
	AddToggle(
		container,
		settings.ghostHideOnlineStatusValue(),
		RuEn("Не отправлять статус «в сети»", "Don't send online packets"),
		[&](bool toggled) { settings.setGhostHideOnlineStatus(toggled); });
	AddToggle(
		container,
		settings.ghostHideTypingProgressValue(),
		RuEn("Не отправлять набор текста и ход загрузки", "Don't send typing/upload progress"),
		[&](bool toggled) { settings.setGhostHideTypingProgress(toggled); });
	AddToggle(
		container,
		settings.localPremiumValue(),
		RuEn("Локальный премиум", "Local Premium"),
		[&](bool toggled) { settings.setLocalPremium(toggled); });
	AddToggle(
		container,
		settings.disableAdsValue(),
		RuEn("Скрывать рекламу и спонсорские блоки", "Hide ads and sponsored"),
		[&](bool toggled) { settings.setDisableAds(toggled); });

	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Интерфейс", "Interface")));
	AddToggle(
		container,
		settings.disableStoriesValue(),
		RuEn("Скрыть истории", "Disable Stories"),
		[&](bool toggled) { settings.setDisableStories(toggled); });
	AddToggle(
		container,
		settings.disableOpenLinkWarningValue(),
		RuEn("Не спрашивать перед открытием ссылок", "Disable open-link warning"),
		[&](bool toggled) { settings.setDisableOpenLinkWarning(toggled); });
	AddToggle(
		container,
		settings.showMessageSecondsValue(),
		RuEn("Показывать секунды во времени", "Show message seconds"),
		[&](bool toggled) { settings.setShowMessageSeconds(toggled); });
	AddToggle(
		container,
		settings.collapseSimilarChannelsValue(),
		RuEn("Сворачивать похожие каналы", "Collapse Similar Channels"),
		[&](bool toggled) { settings.setCollapseSimilarChannels(toggled); });
	AddToggle(
		container,
		settings.hideSimilarChannelsValue(),
		RuEn("Скрыть похожие каналы", "Hide Similar Channels"),
		[&](bool toggled) { settings.setHideSimilarChannels(toggled); });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Защита от удаления", "Anti-recall")));
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
		RuEn("Показать журнал защиты от удаления", "Show anti-recall log"),
		QString::fromUtf8("astro_recall_log.jsonl"),
		[] { File::ShowInFolder(u"./tdata/astro_recall_log.jsonl"_q); });

	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

} // namespace

Astrogram::Astrogram(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Astrogram::title() {
	return rpl::single(QString());
}

void Astrogram::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogram(controller, content);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
