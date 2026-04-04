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
#include "window/window_session_controller_link_info.h"

#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainterPath>
#include <QTextOption>
#include <QUrl>

namespace Settings {
namespace {

constexpr auto kHeaderHeight = 240;
constexpr auto kAvatarSize = 120;

[[nodiscard]] bool IsRussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return IsRussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QImage AstrogramHeaderImage() {
	static const auto image = [] {
		auto result = QImage(u":/gui/art/astrogram/settings_avatar.png"_q);
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
	return QString::fromLatin1("Astrogram Desktop %1").arg(
		QString::fromLatin1(AppVersionStr));
}

void AddAstrogramHeader(not_null<Ui::VerticalLayout*> container) {
	const auto header = container->add(object_ptr<Ui::RpWidget>(container));
	const auto raw = header;
	raw->setMinimumHeight(kHeaderHeight);
	raw->setMaximumHeight(kHeaderHeight);

	raw->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(raw);
		const auto width = raw->width();
			const auto avatarRect = QRect(
				(width - kAvatarSize) / 2,
				14,
				kAvatarSize,
				kAvatarSize);
		const auto avatar = AstrogramHeaderImage();

		if (!avatar.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			QPainterPath path;
			path.addEllipse(QRectF(avatarRect));
			p.save();
			p.setClipPath(path);
			p.drawImage(avatarRect, avatar, CenterCropSourceRect(avatar));
			p.restore();
		}

		auto titleFont = st::semiboldFont->f;
		titleFont.setPixelSize(titleFont.pixelSize() + 9);
		titleFont.setBold(true);
		const auto titleMetrics = QFontMetrics(titleFont);
		const auto titleTop = avatarRect.bottom() + 12;

			p.setPen(st::windowFg);
			p.setFont(titleFont);
			p.drawText(
				QRect(24, titleTop, width - 48, titleMetrics.height() + 6),
				Qt::AlignHCenter | Qt::TextSingleLine,
				u"Astrogram"_q);

		auto versionFont = st::defaultFlatLabel.style.font->f;
		versionFont.setPixelSize(versionFont.pixelSize() + 1);
		const auto versionMetrics = QFontMetrics(versionFont);
		const auto versionTop = titleTop + titleMetrics.height() + 6;

			p.setPen(st::windowSubTextFg);
			p.setFont(versionFont);
			p.drawText(
				QRect(24, versionTop, width - 48, versionMetrics.height() + 4),
				Qt::AlignHCenter | Qt::TextSingleLine,
				AstrogramVersionText());
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

template <typename Callback>
void AddActionButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		Callback callback,
		IconDescriptor descriptor = {}) {
	AddButtonWithIcon(
		container,
		rpl::single(title),
		st::settingsButton,
		std::move(descriptor)
	)->addClickHandler(std::move(callback));
}

void AddSectionButton(
			not_null<Window::SessionController*> controller,
			not_null<Ui::VerticalLayout*> container,
			const QString &title,
			Type type,
			IconDescriptor descriptor) {
	AddButtonWithIcon(
		container,
		rpl::single(title),
		st::settingsButton,
		std::move(descriptor)
	)->addClickHandler([=] { controller->showSettings(type); });
}

void AddLinksSection(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn("Ссылки", "Links")));
	AddActionButtonWithLabel(
		container,
		RuEn("Основной канал", "Main channel"),
		u"@astrogramchannel"_q,
		[=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QStringLiteral("astrogramchannel"),
			});
		},
		{ &st::menuIconChannel });
	AddActionButtonWithLabel(
		container,
		RuEn("Чат сообщества", "Community chat"),
		u"@astrogram_chat"_q,
		[=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QStringLiteral("astrogram_chat"),
			});
		},
		{ &st::menuIconChats });
	AddActionButtonWithLabel(
		container,
		RuEn("Документация", "Documentation"),
		u"docs.astrogram.su"_q,
		[] {
			QDesktopServices::openUrl(QUrl(u"https://docs.astrogram.su"_q));
		},
		{ &st::menuIconIpAddress });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramHome(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	AddAstrogramHeader(container);
	AddSectionButton(
		controller,
		container,
		u"Astrogram"_q,
		AstrogramCore::Id(),
		{ &st::menuIconPremium });
	AddSectionButton(
		controller,
		container,
		RuEn("Приватность", "Privacy"),
		AstrogramPrivacy::Id(),
		{ &st::menuIconLock });
	AddSectionButton(
		controller,
		container,
		RuEn("Интерфейс", "Interface"),
		AstrogramInterface::Id(),
		{ &st::menuIconPalette });
	AddSectionButton(
		controller,
		container,
		RuEn("Защита от удаления", "Anti-recall"),
		AstrogramAntiRecall::Id(),
		{ &st::menuIconRestore });
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddSectionButton(
		controller,
		container,
		RuEn("Плагины", "Plugins"),
		Plugins::Id(),
		{ &st::menuIconCustomize });
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddLinksSection(controller, container);
}

void SetupAstrogramCore(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
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
}

void SetupAstrogramPrivacy(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddToggle(
		container,
		settings.ghostModeValue(),
		RuEn("Режим призрака", "Ghost mode"),
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
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramInterface(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddToggle(
		container,
		settings.disableStoriesValue(),
		RuEn("Скрыть истории", "Hide stories"),
		[&](bool toggled) { settings.setDisableStories(toggled); });
	AddToggle(
		container,
		settings.disableOpenLinkWarningValue(),
		RuEn("Не спрашивать перед открытием ссылок", "Skip link warning"),
		[&](bool toggled) { settings.setDisableOpenLinkWarning(toggled); });
	AddToggle(
		container,
		settings.showMessageSecondsValue(),
		RuEn("Показывать секунды во времени", "Show message seconds"),
		[&](bool toggled) { settings.setShowMessageSeconds(toggled); });
	AddToggle(
		container,
		settings.collapseSimilarChannelsValue(),
		RuEn("Сворачивать похожие каналы", "Collapse similar channels"),
		[&](bool toggled) { settings.setCollapseSimilarChannels(toggled); });
	AddToggle(
		container,
		settings.hideSimilarChannelsValue(),
		RuEn("Скрыть похожие каналы", "Hide similar channels"),
		[&](bool toggled) { settings.setHideSimilarChannels(toggled); });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramAntiRecall(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
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
	AddActionButton(
		container,
		RuEn("Показать журнал", "Show local log"),
		[] { File::ShowInFolder(u"./tdata/astro_recall_log.jsonl"_q); },
		{ &st::menuIconShowInFolder });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramLinks(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddLinksSection(controller, container);
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
	SetupAstrogramHome(controller, content);
	Ui::ResizeFitChild(this, content);
}

AstrogramCore::AstrogramCore(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramCore::title() {
	return rpl::single(u"Astrogram"_q);
}

void AstrogramCore::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramCore(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramPrivacy::AstrogramPrivacy(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramPrivacy::title() {
	return rpl::single(RuEn("Приватность", "Privacy"));
}

void AstrogramPrivacy::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramPrivacy(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramInterface::AstrogramInterface(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramInterface::title() {
	return rpl::single(RuEn("Интерфейс", "Interface"));
}

void AstrogramInterface::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramInterface(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramAntiRecall::AstrogramAntiRecall(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramAntiRecall::title() {
	return rpl::single(RuEn("Защита от удаления", "Anti-recall"));
}

void AstrogramAntiRecall::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramAntiRecall(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramLinks::AstrogramLinks(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramLinks::title() {
	return rpl::single(RuEn("Ссылки", "Links"));
}

void AstrogramLinks::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramLinks(controller, content);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
