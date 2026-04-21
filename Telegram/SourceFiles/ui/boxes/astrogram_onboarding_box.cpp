/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/boxes/astrogram_onboarding_box.h"

#include "apiwrap.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/controls/userpic_button.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/top_background_gradient.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtGui/QFontMetrics>

#include <algorithm>
#include <memory>
#include <optional>

namespace Ui {
namespace {

enum class Step {
	Welcome = 0,
	Presets = 1,
	PluginsInfo = 2,
	PluginsInstall = 3,
	MenuCustomization = 4,
	ShellMode = 5,
	ExperimentalTips = 6,
	Finish = 7,
};

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QImage AstrogramLogo() {
	auto result = QImage(u":/gui/art/astrogram/settings_avatar.png"_q);
	if (result.isNull()) {
		result = QImage(u":/gui/art/logo_256_no_margin.png"_q);
	}
	return result;
}

[[nodiscard]] int OnboardingSidePadding() {
	return 18;
}

[[nodiscard]] int OnboardingTextInset() {
	return 30;
}

[[nodiscard]] int AdaptiveOnboardingWidth(
		not_null<Window::SessionController*> controller) {
	const auto body = controller->widget()->bodyWidget();
	const auto available = body ? body->width() : 0;
	if (available <= 0) {
		return st::boxWideWidth;
	}
	return std::max(
		st::boxWideWidth,
		available - (OnboardingSidePadding() * 2));
}

struct PeerCardTexts {
	QString title;
	QString status;
};

struct PluginCardTexts {
	QString title;
	QString description;
	QString sourceLabel;
};

enum class OnboardingBadgeTone {
	Trusted,
	Official,
	Pending,
	Warning,
};

struct BadgePalette {
	QColor fill;
	QColor border;
	QColor fg;
};

struct SurfacePalette {
	QColor top;
	QColor bottom;
	QColor border;
	QColor accent;
	QColor title;
	QColor body;
	QColor footer;
};

struct InfoCardDescriptor {
	QString eyebrow;
	QString title;
	QString description;
	QString footer;
	const style::icon *icon = nullptr;
	OnboardingBadgeTone tone = OnboardingBadgeTone::Trusted;
};

void AppendInlinePart(QString &base, QString part) {
	part = part.trimmed();
	if (part.isEmpty()) {
		return;
	}
	if (!base.isEmpty()) {
		base += u" \u00b7 "_q;
	}
	base += part;
}

[[nodiscard]] QString AppendLine(QString base, QString line) {
	base = base.trimmed();
	line = line.trimmed();
	if (line.isEmpty()) {
		return base;
	}
	return base.isEmpty() ? line : (base + u"\n"_q + line);
}

[[nodiscard]] BadgePalette BadgeColors(OnboardingBadgeTone tone) {
	switch (tone) {
	case OnboardingBadgeTone::Trusted:
		return {
			.fill = QColor(0x2e, 0xa4, 0xff, 44),
			.border = QColor(0x5c, 0xba, 0xff),
			.fg = QColor(0x1d, 0x7f, 0xff),
		};
	case OnboardingBadgeTone::Official:
		return {
			.fill = QColor(0x27, 0xc9, 0x83, 46),
			.border = QColor(0x46, 0xe0, 0x98),
			.fg = QColor(0x1e, 0xa8, 0x6b),
		};
	case OnboardingBadgeTone::Pending:
		return {
			.fill = QColor(0xf2, 0xc9, 0x4c, 42),
			.border = QColor(0xe6, 0xb8, 0x2f),
			.fg = QColor(0xb7, 0x82, 0x00),
		};
	case OnboardingBadgeTone::Warning:
		return {
			.fill = QColor(0xeb, 0x57, 0x57, 34),
			.border = QColor(0xeb, 0x57, 0x57),
			.fg = QColor(0xcf, 0x45, 0x45),
		};
	}
	Unexpected("Unknown onboarding badge tone.");
}

[[nodiscard]] SurfacePalette SurfaceColors(OnboardingBadgeTone tone) {
	switch (tone) {
	case OnboardingBadgeTone::Trusted:
		return {
			.top = QColor(0xec, 0xf6, 0xff),
			.bottom = QColor(0xe2, 0xf1, 0xff),
			.border = QColor(0x9f, 0xd1, 0xff),
			.accent = QColor(0x2e, 0xa4, 0xff),
			.title = QColor(0x1a, 0x2c, 0x3f),
			.body = QColor(0x4c, 0x61, 0x75),
			.footer = QColor(0x1d, 0x7f, 0xff),
		};
	case OnboardingBadgeTone::Official:
		return {
			.top = QColor(0xeb, 0xfb, 0xf1),
			.bottom = QColor(0xe0, 0xf7, 0xe9),
			.border = QColor(0x96, 0xe1, 0xbd),
			.accent = QColor(0x27, 0xc9, 0x83),
			.title = QColor(0x1f, 0x35, 0x2b),
			.body = QColor(0x4f, 0x67, 0x5a),
			.footer = QColor(0x1e, 0xa8, 0x6b),
		};
	case OnboardingBadgeTone::Pending:
		return {
			.top = QColor(0xff, 0xf8, 0xe3),
			.bottom = QColor(0xff, 0xef, 0xc1),
			.border = QColor(0xf0, 0xd1, 0x73),
			.accent = QColor(0xe6, 0xb8, 0x2f),
			.title = QColor(0x3c, 0x33, 0x18),
			.body = QColor(0x6f, 0x60, 0x2e),
			.footer = QColor(0xb7, 0x82, 0x00),
		};
	case OnboardingBadgeTone::Warning:
		return {
			.top = QColor(0xff, 0xee, 0xee),
			.bottom = QColor(0xff, 0xe1, 0xe1),
			.border = QColor(0xff, 0xba, 0xba),
			.accent = QColor(0xeb, 0x57, 0x57),
			.title = QColor(0x3d, 0x1f, 0x1f),
			.body = QColor(0x7d, 0x4a, 0x4a),
			.footer = QColor(0xcf, 0x45, 0x45),
		};
	}
	Unexpected("Unknown onboarding surface tone.");
}

[[nodiscard]] QString AstrogramKnownChannelUsername(qint64 channelId) {
	switch (channelId) {
	case -1003814280064LL: return u"astroplugin"_q;
	case -1003641835839LL: return u"astrogramchannel"_q;
	}
	return QString();
}

[[nodiscard]] QString ChannelHandleText(PeerData *peer, qint64 channelId) {
	auto username = peer ? peer->username().trimmed() : QString();
	if (username.isEmpty()) {
		username = AstrogramKnownChannelUsername(channelId);
	}
	return username.isEmpty() ? QString() : (u"@"_q + username);
}

[[nodiscard]] QString ChannelMetaText(PeerData *peer, qint64 channelId) {
	auto result = QString();
	AppendInlinePart(result, ChannelHandleText(peer, channelId));
	if (channelId) {
		AppendInlinePart(
			result,
			RuEn("ID %1", "ID %1").arg(QString::number(channelId)));
	}
	return result;
}

[[nodiscard]] OnboardingBadgeTone ChannelCardBadgeTone(
		PeerData *peer,
		bool official) {
	if (!peer) {
		return OnboardingBadgeTone::Pending;
	}
	return official
		? OnboardingBadgeTone::Official
		: OnboardingBadgeTone::Trusted;
}

[[nodiscard]] QString ChannelCardDetailText(
		PeerData *peer,
		qint64 channelId,
		bool official) {
	auto result = ChannelMetaText(peer, channelId);
	result = AppendLine(
		result,
		official
			? RuEn(
				"Название, аватар и аудитория подгружаются прямо из канала, поэтому карточка остаётся актуальной.",
				"The title, avatar and audience are fetched from the channel itself, so the card stays current.")
			: RuEn(
				"Название, аватар и аудитория берутся прямо из доверенного канала.",
				"The title, avatar and audience come directly from the trusted channel."));
	return result;
}

[[nodiscard]] OnboardingBadgeTone PluginSourceBadgeTone(
		const AstrogramOnboardingPlugin &plugin) {
	if (plugin.invalidServerData) {
		return OnboardingBadgeTone::Warning;
	} else if (plugin.pendingServerData) {
		return OnboardingBadgeTone::Pending;
	}
	return OnboardingBadgeTone::Trusted;
}

[[nodiscard]] bool IsAstrogramPluginPackage(DocumentData *document) {
	if (!document) {
		return false;
	}
	const auto filename = document->filename().trimmed();
	return !filename.isEmpty()
		&& filename.endsWith(u".tgd"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString AstrogramPluginTitle(
		DocumentData *document,
		QString fallbackTitle) {
	fallbackTitle = fallbackTitle.trimmed();
	if (!fallbackTitle.isEmpty()) {
		return fallbackTitle;
	}
	auto filename = document ? document->filename().trimmed() : QString();
	if (filename.isEmpty()) {
		return RuEn("Плагин Astrogram", "Astrogram plugin");
	}
	if (filename.endsWith(u".tgd"_q, Qt::CaseInsensitive)) {
		filename.chop(4);
	}
	filename = QFileInfo(filename).completeBaseName().trimmed();
	return filename.isEmpty()
		? RuEn("Плагин Astrogram", "Astrogram plugin")
		: filename;
}

[[nodiscard]] QString AstrogramPluginDescription(
		HistoryItem *item,
		QString fallbackDescription,
		qint64 postId) {
	fallbackDescription = fallbackDescription.trimmed();
	if (!fallbackDescription.isEmpty()) {
		return fallbackDescription;
	}
	if (item) {
		const auto message = item->originalText().text.simplified();
		if (!message.isEmpty()) {
			return message;
		}
		const auto notification = item->notificationText().text.simplified();
		if (!notification.isEmpty()) {
			return notification;
		}
	}
	return RuEn(
		"Пакет плагина из поста #%1",
		"Plugin package from post #%1").arg(postId);
}

[[nodiscard]] QString AstrogramPluginSourceLabel(
		QString channelTitle,
		qint64 postId) {
	channelTitle = channelTitle.trimmed();
	if (channelTitle.isEmpty()) {
		channelTitle = RuEn(
			"Astrogram Plugins",
			"Astrogram Plugins");
	}
	return RuEn(
		"Источник: %1 · пост #%2",
		"Source: %1 · post #%2").arg(channelTitle).arg(postId);
}

[[nodiscard]] QString PeerAudienceText(not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	if (!channel || !channel->membersCountKnown()) {
		return QString();
	}
	const auto count = channel->membersCount();
	if (count <= 0) {
		return QString();
	}
	return (!channel->isMegagroup()
		? tr::lng_chat_status_subscribers
		: tr::lng_chat_status_members)(
			tr::now,
			lt_count,
			count);
}

[[nodiscard]] PeerCardTexts ComputePeerCardTexts(
		PeerData *peer,
		QString fallbackTitle,
		QString fallbackStatus) {
	auto title = fallbackTitle.trimmed();
	auto status = fallbackStatus.trimmed();
	if (peer) {
		const auto peerName = peer->name().trimmed();
		if (!peerName.isEmpty()) {
			title = peerName;
		}
		const auto audience = PeerAudienceText(peer);
		if (!audience.isEmpty()) {
			status = status.isEmpty()
				? audience
				: (audience + u" · "_q + status);
		}
	}
	return {
		.title = title,
		.status = status,
	};
}

[[nodiscard]] PluginCardTexts ResolvePluginCardTexts(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		const AstrogramOnboardingPlugin &plugin,
		QString fallbackChannelTitle) {
	const auto channel = peer ? peer->asChannel() : nullptr;
	const auto item = (channel && (plugin.postId > 0))
		? controller->session().data().message(channel, MsgId(plugin.postId))
		: nullptr;
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	const auto effectiveChannelTitle = ComputePeerCardTexts(
		peer,
		std::move(fallbackChannelTitle),
		QString()).title;
	return {
		.title = AstrogramPluginTitle(
			IsAstrogramPluginPackage(document) ? document : nullptr,
			plugin.title),
		.description = AstrogramPluginDescription(
			item,
			plugin.description,
			plugin.postId),
		.sourceLabel = (plugin.postId > 0)
			? AstrogramPluginSourceLabel(
				effectiveChannelTitle,
				plugin.postId)
			: plugin.sourceLabel.trimmed(),
	};
}

void AddHeroCover(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &subtitle,
		QColor gradientEdge,
		QColor gradientCenter,
		QColor gradientLeft,
		QColor gradientRight) {
	Q_UNUSED(gradientLeft);
	Q_UNUSED(gradientRight);
	const auto cover = container->add(object_ptr<Ui::RpWidget>(container));

	const auto logoTop = st::cocoonLogoTop;
	const auto logoSize = st::cocoonLogoSize;
	const auto titleTop = logoTop + logoSize + st::cocoonTitleTop;
	const auto subtitleTop = titleTop
		+ st::cocoonTitleFont->height
		+ st::cocoonSubtitleTop;

	struct State {
		QImage gradient;
		QImage logo;
		style::owned_color subtitleFg = style::owned_color{ QColor(0xe9, 0xf2, 0xf8) };
		style::owned_color subtitleBoldFg = style::owned_color{ QColor(0xff, 0xff, 0xff) };
		style::FlatLabel subtitleSt = st::cocoonSubtitle;
	};
	const auto state = cover->lifetime().make_state<State>();
	state->subtitleSt.textFg = state->subtitleFg.color();
	state->subtitleSt.palette.linkFg = state->subtitleBoldFg.color();

	const auto ratio = style::DevicePixelRatio();
	state->logo = AstrogramLogo().scaled(
		QSize(logoSize, logoSize) * ratio,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	state->logo.setDevicePixelRatio(ratio);

	const auto subtitleLabel = Ui::CreateChild<Ui::FlatLabel>(
		cover,
		rpl::single(tr::rich(subtitle)),
		state->subtitleSt);
	subtitleLabel->setTryMakeSimilarLines(true);

	cover->widthValue() | rpl::on_next([=](int width) {
		const auto available = width
			- st::boxRowPadding.left()
			- st::boxRowPadding.right();
		subtitleLabel->resizeToWidth(available);
		subtitleLabel->moveToLeft(st::boxRowPadding.left(), subtitleTop);
		cover->resize(
			width,
			subtitleLabel->y() + subtitleLabel->height() + st::cocoonSubtitleBottom);
	}, cover->lifetime());

	cover->paintRequest() | rpl::on_next([=] {
		auto p = Painter(cover);
		const auto width = cover->width();
		const auto ratio = style::DevicePixelRatio();
		if (state->gradient.size() != cover->size() * ratio) {
			state->gradient = Ui::CreateTopBgGradient(
				cover->size(),
				gradientCenter,
				gradientEdge);

			auto font = st::cocoonTitleFont->f;
			font.setWeight(QFont::Bold);
			const auto metrics = QFontMetrics(font);
			const auto textw = metrics.horizontalAdvance(title);
			const auto left = (width - textw) / 2;

			auto q = QPainter(&state->gradient);
			auto hq = PainterHighQualityEnabler(q);
			q.setPen(QColor(0xff, 0xff, 0xff));
			q.setFont(font);
			q.setBrush(Qt::NoBrush);
			q.drawText(left, titleTop + metrics.ascent(), title);
		}
		p.drawImage(0, 0, state->gradient);

		const auto logoRect = QRect(
			(width - logoSize) / 2,
			logoTop,
			logoSize,
			logoSize);
		p.drawImage(logoRect, state->logo);
	}, cover->lifetime());
}

not_null<Ui::RpWidget*> AddInfoCard(
		not_null<Ui::VerticalLayout*> container,
		InfoCardDescriptor descriptor,
		style::margins margins = style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0)) {
	const auto card = container->add(
		object_ptr<Ui::RpWidget>(container),
		margins,
		style::al_top);
	const auto palette = SurfaceColors(descriptor.tone);

	struct State {
		Ui::Text::String eyebrow;
		Ui::Text::String title;
		Ui::Text::String description;
		Ui::Text::String footer;
		int height = 0;
	};
	const auto state = card->lifetime().make_state<State>();
	state->eyebrow.setText(st::defaultTextStyle, descriptor.eyebrow.trimmed());
	state->title.setText(st::semiboldTextStyle, descriptor.title.trimmed());
	state->description.setText(st::defaultTextStyle, descriptor.description.trimmed());
	state->footer.setText(st::semiboldTextStyle, descriptor.footer.trimmed());

	const auto relayout = [=](int width) {
		const auto cardWidth = std::max(width, 200);
		const auto contentLeft = descriptor.icon ? 72 : 18;
		const auto contentWidth = std::max(80, cardWidth - contentLeft - 18);
		auto top = 18;
		if (!descriptor.eyebrow.trimmed().isEmpty()) {
			top += state->eyebrow.countHeight(contentWidth);
			top += 5;
		}
		top += state->title.countHeight(contentWidth);
		if (!descriptor.description.trimmed().isEmpty()) {
			top += 8;
			top += state->description.countHeight(contentWidth);
		}
		if (!descriptor.footer.trimmed().isEmpty()) {
			top += 10;
			top += state->footer.countHeight(contentWidth);
		}
		top += 18;
		state->height = std::max(top, descriptor.icon ? 78 : top);
		card->resize(width, state->height);
		card->update();
	};
	card->widthValue() | rpl::on_next(relayout, card->lifetime());
	relayout(std::max(card->width(), container->width() - 24));

	card->paintRequest() | rpl::on_next([=] {
		auto p = Painter(card);
		auto hq = PainterHighQualityEnabler(p);
		const auto width = card->width();
		const auto height = card->height();
		const auto outer = QRectF(0.5, 0.5, width - 1., height - 1.);
		p.setPen(QPen(palette.border, 1.));
		p.setBrush(palette.top);
		p.drawRoundedRect(outer, 18., 18.);
		if (descriptor.icon) {
			const auto bubble = QRect(18, 18, 42, 42);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(palette.accent.red(), palette.accent.green(), palette.accent.blue(), 26));
			p.drawRoundedRect(QRectF(bubble), 12., 12.);
			descriptor.icon->paintInCenter(p, bubble.adjusted(9, 9, -9, -9));
		}

		const auto contentLeft = descriptor.icon ? 72 : 18;
		const auto contentWidth = std::max(80, width - contentLeft - 18);
		auto top = 18;
		if (!descriptor.eyebrow.trimmed().isEmpty()) {
			p.setPen(palette.accent);
			state->eyebrow.drawLeft(p, contentLeft, top, contentWidth, width);
			top += state->eyebrow.countHeight(contentWidth);
			top += 5;
		}
		p.setPen(palette.title);
		state->title.drawLeft(p, contentLeft, top, contentWidth, width);
		top += state->title.countHeight(contentWidth);
		if (!descriptor.description.trimmed().isEmpty()) {
			top += 8;
			p.setPen(palette.body);
			state->description.drawLeft(p, contentLeft, top, contentWidth, width);
			top += state->description.countHeight(contentWidth);
		}
		if (!descriptor.footer.trimmed().isEmpty()) {
			top += 10;
			p.setPen(palette.footer);
			state->footer.drawLeft(p, contentLeft, top, contentWidth, width);
		}
	}, card->lifetime());

	return card;
}

not_null<Ui::AbstractButton*> AddChannelCard(
		not_null<Ui::VerticalLayout*> container,
		PeerData *peer,
		qint64 channelId,
		QString fallbackTitle,
		QString subtitle,
		bool official,
		std::function<void()> callback,
		style::margins margins = style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0)) {
	const auto card = container->add(
		object_ptr<Ui::AbstractButton>::fromRaw(
			Ui::CreateSimpleSettingsButton(
				container,
				st::defaultRippleAnimation,
				st::defaultSettingsButton.textBgOver)),
		margins,
		style::al_top);
	card->setClickedCallback(std::move(callback));

	const auto tone = official
		? OnboardingBadgeTone::Official
		: ChannelCardBadgeTone(peer, false);
	const auto palette = SurfaceColors(tone);
	const auto badgePalette = BadgeColors(tone);
	const auto userpicSt = card->lifetime().make_state<style::UserpicButton>(
		st::defaultUserpicButton);
	userpicSt->photoSize = 58;
	userpicSt->size = QSize(userpicSt->photoSize, userpicSt->photoSize);

	struct State {
		Ui::Text::String title;
		Ui::Text::String meta;
		Ui::Text::String subtitle;
		Ui::Text::String footer;
		QImage fallbackLogo;
		int height = 0;
	};
	const auto state = card->lifetime().make_state<State>();
	const auto ratio = style::DevicePixelRatio();
	state->fallbackLogo = AstrogramLogo().scaled(
		QSize(34, 34) * ratio,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	state->fallbackLogo.setDevicePixelRatio(ratio);

	if (peer) {
		using Button = Ui::UserpicButton;
		const auto userpic = Ui::CreateChild<Button>(card, peer, *userpicSt);
		userpic->move(18, 20);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		peer->loadUserpic();
		if (const auto channel = peer->asChannel();
			channel && !channel->membersCountKnown()) {
			peer->session().api().requestFullPeer(channel);
		}
	}

	const auto refresh = [=](int width) {
		const auto cardWidth = std::max(width, 240);
		auto meta = QString();
		AppendInlinePart(meta, ChannelHandleText(peer, channelId));
		if (peer) {
			AppendInlinePart(meta, PeerAudienceText(peer));
		}
		if (meta.isEmpty()) {
			meta = ChannelMetaText(peer, channelId);
		}
		state->title.setText(
			st::semiboldTextStyle,
			ComputePeerCardTexts(peer, fallbackTitle, QString()).title);
		state->meta.setText(st::defaultTextStyle, meta);
		state->subtitle.setText(
			st::defaultTextStyle,
			subtitle.trimmed().isEmpty()
				? ChannelCardDetailText(peer, channelId, official)
				: subtitle.trimmed());
		state->footer.setText(
			st::semiboldTextStyle,
			RuEn("Нажми, чтобы открыть канал", "Click to open the channel"));
		const auto contentLeft = 90;
		const auto contentWidth = std::max(90, cardWidth - contentLeft - 18);
		auto top = 18;
		top += state->title.countHeight(contentWidth);
		top += 6;
		top += state->meta.countHeight(contentWidth);
		top += 8;
		top += state->subtitle.countHeight(contentWidth);
		top += 10;
		top += state->footer.countHeight(contentWidth);
		top += 18;
		state->height = std::max(top, 96);
		card->resize(width, state->height);
		card->update();
	};
	if (peer) {
		peer->session().changes().peerUpdates(
			peer,
			Data::PeerUpdate::Flag::Name
				| Data::PeerUpdate::Flag::Username
				| Data::PeerUpdate::Flag::Photo
				| Data::PeerUpdate::Flag::Members
				| Data::PeerUpdate::Flag::FullInfo
		) | rpl::on_next([=](const Data::PeerUpdate &) {
			refresh(card->width());
		}, card->lifetime());
	}
	card->widthValue() | rpl::on_next(refresh, card->lifetime());
	refresh(std::max(card->width(), container->width() - 24));

	card->paintRequest() | rpl::on_next([=] {
		auto p = Painter(card);
		auto hq = PainterHighQualityEnabler(p);
		const auto width = card->width();
		const auto height = card->height();
		const auto outer = QRectF(0.5, 0.5, width - 1., height - 1.);
		p.setPen(QPen(palette.border, 1.));
		p.setBrush(palette.top);
		p.drawRoundedRect(outer, 18., 18.);
		if (!peer) {
			const auto userpicRect = QRect(18, 20, 58, 58);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(badgePalette.fill.red(), badgePalette.fill.green(), badgePalette.fill.blue(), 28));
			p.drawRoundedRect(QRectF(userpicRect), 16., 16.);
			p.drawImage(
				QRect(userpicRect.x() + 12, userpicRect.y() + 12, 34, 34),
				state->fallbackLogo);
		}
		const auto contentLeft = 90;
		const auto contentWidth = std::max(90, width - contentLeft - 18);
		auto top = 18;
		p.setPen(palette.title);
		state->title.drawLeft(p, contentLeft, top, contentWidth, width);
		top += state->title.countHeight(contentWidth);
		top += 6;
		p.setPen(badgePalette.fg);
		state->meta.drawLeft(p, contentLeft, top, contentWidth, width);
		top += state->meta.countHeight(contentWidth);
		top += 8;
		p.setPen(palette.body);
		state->subtitle.drawLeft(p, contentLeft, top, contentWidth, width);
		top += state->subtitle.countHeight(contentWidth);
		top += 10;
		p.setPen(palette.footer);
		state->footer.drawLeft(p, contentLeft, top, contentWidth, width);
	}, card->lifetime());

	return card;
}

not_null<Ui::RoundButton*> AddPrimaryButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		std::function<void()> callback) {
	auto button = object_ptr<Ui::RoundButton>(
		container,
		rpl::single(text),
		st::defaultActiveButton);
	button->setTextTransform(Ui::RoundButton::TextTransform::NoTransform);
	const auto raw = container->add(
		std::move(button),
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
	raw->resizeToWidth(container->width()
		- (OnboardingSidePadding() * 2));
	raw->setClickedCallback(std::move(callback));
	return raw;
}

not_null<Ui::LinkButton*> AddLinkAction(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		std::function<void()> callback) {
	const auto button = container->add(
		object_ptr<Ui::LinkButton>(
			container,
			text,
			st::defaultLinkButton),
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
	button->setClickedCallback(std::move(callback));
	return button;
}

not_null<Ui::SettingsButton*> AddChoiceButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &description,
		const style::icon *icon,
		std::function<void()> callback) {
	auto descriptor = Settings::IconDescriptor();
	descriptor.icon = icon;
	const auto button = container->add(
		Settings::CreateButtonWithIcon(
			container,
			rpl::single(title),
			st::settingsButtonLight,
			std::move(descriptor)),
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
	button->setClickedCallback(std::move(callback));
	if (!description.isEmpty()) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(description),
				st::defaultFlatLabel),
			style::margins(
				OnboardingTextInset(),
				-2,
				OnboardingTextInset(),
				6),
			style::al_top);
	}
	return button;
}

not_null<Ui::AbstractButton*> AddPeerChoiceButton(
		not_null<Ui::VerticalLayout*> container,
		PeerData *peer,
		const QString &fallbackTitle,
		const QString &fallbackDescription,
		std::function<void()> callback) {
	const auto row = container->add(
		object_ptr<Ui::AbstractButton>::fromRaw(
			Ui::CreateSimpleSettingsButton(
				container,
				st::defaultRippleAnimation,
				st::defaultSettingsButton.textBgOver)),
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
	row->resize(row->width(), st::defaultPeerListItem.height);
	row->setClickedCallback(std::move(callback));

	const auto userpicSt = row->lifetime().make_state<style::UserpicButton>(
		st::defaultUserpicButton);
	userpicSt->photoSize = st::defaultPeerListItem.photoSize;
	userpicSt->size = QSize(userpicSt->photoSize, userpicSt->photoSize);

	struct State {
		std::shared_ptr<Ui::Text::String> title;
		std::shared_ptr<Ui::Text::String> status;
	};
	const auto state = row->lifetime().make_state<State>();
	state->title = std::make_shared<Ui::Text::String>();
	state->status = std::make_shared<Ui::Text::String>();
	const auto refresh = [=] {
		const auto texts = ComputePeerCardTexts(
			peer,
			fallbackTitle,
			fallbackDescription);
		state->title->setText(
			st::defaultPeerListItem.nameStyle,
			texts.title);
		state->status->setText(
			st::defaultTextStyle,
			texts.status);
		row->update();
	};

	if (peer) {
		using Button = Ui::UserpicButton;
		const auto userpic = Ui::CreateChild<Button>(row, peer, *userpicSt);
		userpic->move(st::defaultPeerListItem.photoPosition);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		peer->loadUserpic();
		peer->session().changes().peerUpdates(
			peer,
			Data::PeerUpdate::Flag::Name
				| Data::PeerUpdate::Flag::Username
				| Data::PeerUpdate::Flag::Photo
				| Data::PeerUpdate::Flag::Members
				| Data::PeerUpdate::Flag::FullInfo
		) | rpl::on_next([=](const Data::PeerUpdate &) {
			refresh();
		}, row->lifetime());
		if (const auto channel = peer->asChannel();
			channel && !channel->membersCountKnown()) {
			peer->session().api().requestFullPeer(channel);
		}
	}
	refresh();

	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		const auto &st = st::defaultPeerListItem;
		const auto availableWidth = row->width()
			- st::boxRowPadding.right()
			- st.namePosition.x();
		auto context = Ui::Text::PaintContext{
			.position = st.namePosition,
			.outerWidth = availableWidth,
			.availableWidth = availableWidth,
			.elisionLines = 1,
		};
		p.setPen(st.nameFg);
		state->title->draw(p, context);
		p.setPen(st.statusFg);
		context.position = st.statusPosition;
		state->status->draw(p, context);
	}, row->lifetime());

	return row;
}

void ClearLayout(not_null<Ui::VerticalLayout*> container) {
	while (container->count()) {
		delete container->widgetAt(0);
	}
}

} // namespace

void ShowAstrogramOnboardingBox(AstrogramOnboardingArgs args) {
	Expects(args.controller != nullptr);

	args.controller->show(Box([args = std::move(args)](
			not_null<Ui::GenericBox*> box) mutable {
		const auto controller = args.controller;
		const auto updateBoxWidth = [=] {
			box->setWidth(AdaptiveOnboardingWidth(controller));
		};
		updateBoxWidth();
		box->setNoContentMargin(true);
		box->setCloseByEscape(false);
		box->setCloseByOutsideClick(false);
		if (const auto body = controller->widget()->bodyWidget()) {
			body->widthValue() | rpl::on_next([=](int) {
				updateBoxWidth();
			}, box->lifetime());
		}

		const auto container = box->verticalLayout();
		const auto plugins = box->lifetime().make_state<
				std::vector<AstrogramOnboardingPlugin>>(std::move(args.plugins));

		struct State {
			Step step = Step::Welcome;
			PeerData *pluginsChannelPeer = nullptr;
			PeerData *officialChannelPeer = nullptr;
			QSet<qint64> requestedPluginPosts;
			bool reloadingPlugins = false;
			bool autoReloadTriggered = false;
			bool completionStored = false;
		};
		const auto state = box->lifetime().make_state<State>(State{
			.pluginsChannelPeer = args.pluginsChannelPeer,
			.officialChannelPeer = args.officialChannelPeer,
		});
		const auto weak = base::make_weak(box);

		const auto markCompleted = [=] {
			if (state->completionStored) {
				return;
			}
			state->completionStored = true;
			if (args.finished) {
				args.finished();
			}
		};
		const auto finish = [=] {
			markCompleted();
			box->closeBox();
		};
		const auto skip = [=] {
			markCompleted();
			box->closeBox();
		};
		const auto rebuild = std::make_shared<Fn<void()>>();
		const auto requestPluginsReload = std::make_shared<Fn<void()>>();
		*requestPluginsReload = [=] {
			if (state->reloadingPlugins || !args.reloadPlugins) {
				return;
			}
			state->reloadingPlugins = true;
			args.reloadPlugins([weak, rebuild, state, plugins](
					std::vector<AstrogramOnboardingPlugin> refreshed) {
				if (!weak.get()) {
					return;
				}
				*plugins = std::move(refreshed);
				state->reloadingPlugins = false;
				state->requestedPluginPosts.clear();
				(*rebuild)();
			});
		};

		*rebuild = [=]() mutable {
			ClearLayout(container);
			box->clearButtons();

			const auto addTitle = [&](const QString &title, const QString &subtitle) {
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(title),
						st::boxTitle),
					style::margins(
						OnboardingSidePadding(),
						14,
						OnboardingSidePadding(),
						0),
					style::al_top);
				if (subtitle.trimmed().isEmpty()) {
					return;
				}
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(subtitle),
						st::boxLabel),
					style::margins(
						OnboardingSidePadding(),
						4,
						OnboardingSidePadding(),
						0),
					style::al_top);
			};
			const auto addBlock = [&](const QString &title, const QString &text) {
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				Ui::AddDivider(container);
				Ui::AddSubsectionTitle(container, rpl::single(title));
				if (text.trimmed().isEmpty()) {
					return;
				}
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(text),
						st::boxLabel),
					style::margins(
						OnboardingSidePadding(),
						0,
						OnboardingSidePadding(),
						0),
					style::al_top);
			};
			const auto goToStep = [&](Step step) {
				state->step = step;
				(*rebuild)();
			};
			const auto setFooterButtons = [&](QString primaryText,
					Fn<void()> primaryCallback,
					QString secondaryText = QString(),
					Fn<void()> secondaryCallback = {}) {
				box->clearButtons();
				if (!secondaryText.isEmpty() && secondaryCallback) {
					const auto secondary = box->addLeftButton(
						rpl::single(secondaryText),
						std::move(secondaryCallback));
					secondary->setTextTransform(
						Ui::RoundButton::TextTransform::NoTransform);
				}
				if (!primaryText.isEmpty() && primaryCallback) {
					const auto primary = box->addButton(
						rpl::single(primaryText),
						std::move(primaryCallback));
					primary->setTextTransform(
						Ui::RoundButton::TextTransform::NoTransform);
				}
			};
			const auto pluginStatusText = [](const AstrogramOnboardingPlugin &plugin) {
				if (plugin.invalidServerData) {
					return RuEn(
						"нужно обновить",
						"needs refresh");
				} else if (plugin.pendingServerData) {
					return RuEn(
						"данные ещё загружаются",
						"data is still loading");
				}
				return RuEn(
					"рекомендация AstroPlugins",
					"AstroPlugins recommendation");
			};
			const auto applyShellPreset = [&](AstrogramOnboardingShellPreset preset) {
				if (args.applyShellPreset && !args.applyShellPreset(preset)) {
					controller->showToast(RuEn(
						"Не удалось сохранить пресет меню Astrogram.",
						"Failed to save the Astrogram menu preset."));
					return;
				}
				state->step = Step::ExperimentalTips;
				(*rebuild)();
			};

			switch (state->step) {
			case Step::Welcome: {
				addTitle(
					RuEn(
						"Добро пожаловать в Astrogram Desktop",
						"Welcome to Astrogram Desktop"),
					RuEn(
						"Быстро подготовим клиент к первому запуску.",
						"We'll quickly prepare the client for the first launch."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::Presets); },
					RuEn("Пропустить", "Skip"),
					skip);
			} break;
			case Step::Presets: {
				addTitle(
					RuEn("Выбери стартовый пресет", "Choose a startup preset"),
					RuEn(
						"Это только основа. Потом всё можно поменять вручную.",
						"This is only the baseline. Everything can be changed later."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Рекомендованный", "Recommended"),
					RuEn(
						"Комфортные дефолты Astrogram.",
						"Comfortable Astrogram defaults."),
					&st::menuIconPremium,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Recommended);
						}
						goToStep(Step::PluginsInfo);
					});
				AddChoiceButton(
					container,
					RuEn("Приватный", "Private"),
					RuEn(
						"Упор на приватность.",
						"Privacy-focused setup."),
					&st::menuIconLock,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Private);
						}
						goToStep(Step::PluginsInfo);
					});
				AddChoiceButton(
					container,
					RuEn("Минимальный", "Minimal"),
					RuEn(
						"Ближе всего к Telegram Desktop.",
						"Closest to Telegram Desktop."),
					&st::menuIconPalette,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Minimal);
						}
						goToStep(Step::PluginsInfo);
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Позже", "Later"),
					[=] { goToStep(Step::PluginsInfo); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::Welcome); });
			} break;
			case Step::PluginsInfo: {
				addTitle(
					RuEn("Плагины внутри Astrogram", "Plugins inside Astrogram"),
					RuEn(
						"Покажем доверенные .tgd-пакеты и предложим поставить их сразу.",
						"We'll show trusted .tgd packages and let you install them right away."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::PluginsInstall); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::Presets); });
			} break;
			case Step::PluginsInstall: {
				if (!state->autoReloadTriggered && args.reloadPlugins) {
					state->autoReloadTriggered = true;
					(*requestPluginsReload)();
				}
				addTitle(
					RuEn("3 рекомендации AstroPlugins", "3 AstroPlugins recommendations"),
					RuEn(
						"Выбери, что поставить сразу. Остальное можно открыть позже.",
						"Choose what to install right away. Everything else stays available later."));
				const auto fallbackPluginsTitle = args.pluginsChannelTitle.isEmpty()
					? RuEn("AstroPlugins", "AstroPlugins")
					: args.pluginsChannelTitle;
				if (state->pluginsChannelPeer) {
					AddPeerChoiceButton(
						container,
						state->pluginsChannelPeer,
						fallbackPluginsTitle,
						ChannelMetaText(state->pluginsChannelPeer, args.pluginsChannelId),
						[=] {
							if (args.subscribePluginsChannel) {
								args.subscribePluginsChannel();
							}
						});
				} else if (args.subscribePluginsChannel) {
					AddChoiceButton(
						container,
						fallbackPluginsTitle,
						ChannelMetaText(nullptr, args.pluginsChannelId),
						&st::menuIconIpAddress,
						[=] {
							args.subscribePluginsChannel();
						});
				}
				if (args.reloadPlugins) {
					AddLinkAction(container, RuEn("Обновить рекомендации", "Refresh recommendations"), [=] {
						(*requestPluginsReload)();
					});
				}
				if (state->reloadingPlugins) {
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(RuEn(
								"Обновляем рекомендации и карточки…",
								"Refreshing recommendations and cards…")),
							st::boxLabel),
						style::margins(
							OnboardingSidePadding(),
							8,
							OnboardingSidePadding(),
							0),
						style::al_top);
				}
				if (plugins->empty()) {
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(state->reloadingPlugins
								? RuEn(
									"Подтягиваем карточки…",
									"Loading package cards…")
								: RuEn(
									"Карточки появятся здесь автоматически, как только будут готовы.",
									"The cards will appear here automatically once they are ready.")),
							st::boxLabel),
						style::margins(
							OnboardingSidePadding(),
							8,
							OnboardingSidePadding(),
							0),
						style::al_top);
				} else {
					const auto visiblePlugins = std::min<int>(plugins->size(), 3);
					Ui::AddSkip(container, st::settingsCheckboxesSkip / 4);
					Ui::AddSubsectionTitle(
						container,
						rpl::single(RuEn("3 стартовые рекомендации", "3 starter recommendations")));
					const auto channel = state->pluginsChannelPeer
						? state->pluginsChannelPeer->asChannel()
						: nullptr;
					for (auto i = 0; i != visiblePlugins; ++i) {
						const auto &plugin = (*plugins)[i];
						if (channel
							&& (plugin.postId > 0)
							&& !state->requestedPluginPosts.contains(plugin.postId)
							&& !controller->session().data().message(
								channel,
								MsgId(plugin.postId))) {
							state->requestedPluginPosts.insert(plugin.postId);
							controller->session().api().requestMessageData(
								channel,
								MsgId(plugin.postId),
								[weak, rebuild] {
									if (weak.get()) {
										(*rebuild)();
									}
								});
						}
						const auto texts = ResolvePluginCardTexts(
							controller,
							state->pluginsChannelPeer,
							plugin,
							fallbackPluginsTitle);
						const auto action = plugin.invalidServerData
							? std::function<void()>([requestPluginsReload] {
								(*requestPluginsReload)();
							})
							: plugin.install;
						auto description = texts.description.trimmed();
						if (!texts.sourceLabel.trimmed().isEmpty()) {
							description = AppendLine(description, texts.sourceLabel);
						}
						description = AppendLine(description, pluginStatusText(plugin));
						AddChoiceButton(
							container,
							texts.title,
							description,
							&st::menuIconDownload,
							[action] {
								if (action) {
									action();
								}
							});
						Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
					}
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::MenuCustomization); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::PluginsInfo); });
			} break;
			case Step::MenuCustomization: {
				addTitle(
					RuEn("Кастомизируй меню Astrogram", "Customize Astrogram menus"),
					RuEn(
						"Боковую панель и контекстное меню потом можно спокойно докрутить вручную.",
						"You can fine-tune the side panel and context menu later by hand."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::ShellMode); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::PluginsInstall); });
			} break;
			case Step::ShellMode: {
				addTitle(
					RuEn("Выбери shell-режим", "Choose a shell mode"),
					RuEn(
						"Это только стартовый вид окна. Его можно поменять позже.",
						"This is only the starting shell layout. You can change it later."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Сбалансированный", "Balanced"),
					RuEn(
						"Спокойный базовый вариант.",
						"A calm default setup."),
					&st::menuIconPalette,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Balanced); });
				AddChoiceButton(
					container,
					RuEn("Фокус на меню", "Focused"),
					RuEn(
						"Шире и выразительнее, без левого края.",
						"Wider and bolder, without the left edge."),
					&st::menuIconCustomize,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Focused); });
				AddChoiceButton(
					container,
					RuEn("Широкий интерфейс", "Wide shell"),
					RuEn(
						"Самый широкий и заметный вариант.",
						"The widest and most expressive option."),
					&st::menuIconExperimental,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Wide); });
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::ExperimentalTips); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::MenuCustomization); });
			} break;
			case Step::ExperimentalTips: {
				addTitle(
					RuEn("Дальше всё в Experimental", "Next everything stays in Experimental"),
					RuEn(
						"После гайда останутся только обычные настройки клиента.",
						"After onboarding only the regular client settings remain."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::Finish); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::ShellMode); });
			} break;
			case Step::Finish: {
				addTitle(
					RuEn("Всё готово", "You're all set"),
					RuEn(
						"Плагины, пресет и меню уже подготовлены.",
						"Plugins, preset and menus are ready."));
				if (state->officialChannelPeer) {
					AddPeerChoiceButton(
						container,
						state->officialChannelPeer,
						args.officialChannelTitle.trimmed().isEmpty()
							? RuEn("Astrogram", "Astrogram")
							: args.officialChannelTitle.trimmed(),
						ChannelMetaText(state->officialChannelPeer, args.officialChannelId),
						[=] {
							if (args.openOfficialChannel) {
								args.openOfficialChannel();
							}
						});
				} else if (args.openOfficialChannel) {
					AddChoiceButton(
						container,
						args.officialChannelTitle.trimmed().isEmpty()
							? RuEn("Astrogram", "Astrogram")
							: args.officialChannelTitle.trimmed(),
						ChannelMetaText(nullptr, args.officialChannelId),
						&st::menuIconInfo,
						[=] {
							args.openOfficialChannel();
						});
				}
				if (args.openDonate) {
					AddChoiceButton(
						container,
						RuEn(
							"Поддержать Astrogram",
							"Support Astrogram"),
						RuEn(
							"Открывает поддержку.",
							"Opens support."),
						&st::menuIconGiftPremium,
						[=] {
							args.openDonate();
						});
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Завершить", "Finish"),
					finish,
					args.openOfficialChannel
						? RuEn("Канал", "Channel")
						: RuEn("Назад", "Back"),
					args.openOfficialChannel
						? Fn<void()>([=] { args.openOfficialChannel(); })
						: Fn<void()>([=] { goToStep(Step::ExperimentalTips); }));
			} break;
			}
		};

		if (!state->pluginsChannelPeer && args.resolvePluginsChannel) {
			args.resolvePluginsChannel([=](PeerData *peer) {
				if (!peer) {
					return;
				}
				state->pluginsChannelPeer = peer;
				if (weak.get() && (state->step == Step::PluginsInstall)) {
					(*rebuild)();
				}
			});
		}
		if (!state->officialChannelPeer && args.resolveOfficialChannel) {
			args.resolveOfficialChannel([=](PeerData *peer) {
				if (!peer) {
					return;
				}
				state->officialChannelPeer = peer;
				if (weak.get() && (state->step == Step::Finish)) {
					(*rebuild)();
				}
			});
		}
		(*rebuild)();
	}));
}

} // namespace Ui
