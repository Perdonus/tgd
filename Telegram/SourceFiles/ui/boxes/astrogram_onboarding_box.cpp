/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/boxes/astrogram_onboarding_box.h"

#include "ui/boxes/about_cocoon_box.h"

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
#include "settings/settings_experimental.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/ministar_particles.h"
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
#include <QtCore/QTimer>
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

[[nodiscard]] int StepIndex(Step step) {
	switch (step) {
	case Step::Welcome: return 1;
	case Step::Presets: return 2;
	case Step::PluginsInfo: return 3;
	case Step::PluginsInstall: return 4;
	case Step::MenuCustomization: return 5;
	case Step::ShellMode: return 6;
	case Step::ExperimentalTips: return 7;
	case Step::Finish: return 8;
	}
	Unexpected("Unknown onboarding step.");
}

[[nodiscard]] QString StepBadgeText(Step step) {
	constexpr auto kTotalSteps = 8;
	return RuEn("Шаг %1 из %2", "Step %1 of %2").arg(StepIndex(step)).arg(kTotalSteps);
}

[[nodiscard]] OnboardingBadgeTone StepBadgeTone(Step step) {
	return (step == Step::Finish)
		? OnboardingBadgeTone::Official
		: OnboardingBadgeTone::Pending;
}

[[nodiscard]] QString StepShortTitle(Step step) {
	switch (step) {
	case Step::Welcome: return RuEn("Старт", "Start");
	case Step::Presets: return RuEn("Пресет", "Preset");
	case Step::PluginsInfo: return RuEn("Плагины", "Plugins");
	case Step::PluginsInstall: return RuEn("Установка", "Install");
	case Step::MenuCustomization: return RuEn("Меню", "Menus");
	case Step::ShellMode: return RuEn("Shell", "Shell");
	case Step::ExperimentalTips: return RuEn("Тюнинг", "Tune");
	case Step::Finish: return RuEn("Финиш", "Finish");
	}
	Unexpected("Unknown onboarding step.");
}

[[nodiscard]] std::optional<Step> PreviousStep(Step step) {
	switch (step) {
	case Step::Welcome: return std::nullopt;
	case Step::Presets: return Step::Welcome;
	case Step::PluginsInfo: return Step::Presets;
	case Step::PluginsInstall: return Step::PluginsInfo;
	case Step::MenuCustomization: return Step::PluginsInstall;
	case Step::ShellMode: return Step::MenuCustomization;
	case Step::ExperimentalTips: return Step::ShellMode;
	case Step::Finish: return Step::ExperimentalTips;
	}
	Unexpected("Unknown onboarding step.");
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

[[nodiscard]] QString ChannelCardBadgeText(
		PeerData *peer,
		bool official) {
	if (!peer) {
		return RuEn(
			"Карточка канала загружается",
			"Channel card is loading");
	}
	return official
		? RuEn(
			"Официальный канал Astrogram",
			"Official Astrogram channel")
		: RuEn(
			"Подтверждённый канал Astrogram",
			"Trusted Astrogram channel");
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
				"Эта карточка живёт от server-driven app config: название, аватар и подписчики подставляются прямо из доверенного канала.",
				"This card is driven by server app config: the title, avatar and audience come directly from the trusted channel."));
	return result;
}

[[nodiscard]] QString PluginSourceBadgeText(
		const AstrogramOnboardingPlugin &plugin) {
	if (plugin.invalidServerData) {
		return RuEn(
			"Неподтверждённый источник",
			"Unverified source");
	} else if (plugin.pendingServerData) {
		return RuEn(
			"Карточка загружается с сервера",
			"Server card is loading");
	}
	return RuEn(
		"Подтверждённый источник",
		"Verified source");
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

[[nodiscard]] QString PluginSourceBadgeDetailText(
		const AstrogramOnboardingPlugin &plugin) {
	if (plugin.invalidServerData) {
		return RuEn(
			"Сервер отдал пост, но в этой карточке пока нет корректного .tgd-пакета. Обнови рекомендации или поправь пост-источник.",
			"The server returned a post, but this card still has no valid .tgd package. Refresh the recommendations or fix the source post.");
	} else if (plugin.pendingServerData) {
		return RuEn(
			"Server app config уже передал post id. Сейчас подтягиваем сам пост, описание и .tgd-пакет без перезапуска клиента.",
			"Server app config already provided the post id. The post, description and .tgd package are being fetched right now without restarting the client.");
	}
	return RuEn(
		"Эта рекомендация приходит из доверенного Astrogram-канала, а название и описание читаются прямо из живой карточки поста.",
		"This recommendation comes from a trusted Astrogram channel, and its title and description are taken straight from the live post card.");
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
		Ui::Animations::Basic animation;
		std::optional<Ui::StarParticles> particles;
		style::owned_color subtitleFg = style::owned_color{ QColor(0xe5, 0xf1, 0xee) };
		style::owned_color subtitleBoldFg = style::owned_color{ QColor(0xff, 0xff, 0xff) };
		style::FlatLabel subtitleSt = st::cocoonSubtitle;
	};
	const auto state = cover->lifetime().make_state<State>();
	state->subtitleSt.textFg = state->subtitleFg.color();
	state->subtitleSt.palette.linkFg = state->subtitleBoldFg.color();
	state->animation.init([=] {
		cover->update();
		if (anim::Disabled()) {
			state->animation.stop();
		}
	});

	const auto ratio = style::DevicePixelRatio();
	state->logo = AstrogramLogo().scaled(
		QSize(logoSize, logoSize) * ratio,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	state->logo.setDevicePixelRatio(ratio);

	state->particles.emplace(
		Ui::StarParticles::Type::RadialInside,
		54,
		st::cocoonLogoSize / 12);
	state->particles->setSpeed(0.05);
	state->particles->setColors({
		gradientLeft,
		anim::color(gradientLeft, gradientRight, 0.45),
		anim::color(gradientLeft, gradientRight, 0.75),
		gradientRight,
	});

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

			auto gradient = QLinearGradient(left, 0, left + textw, 0);
			gradient.setStops({
				{ 0., gradientLeft },
				{ 1., gradientRight },
			});
			q.setPen(QPen(QBrush(gradient), 1.));
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
		const auto paddingAdd = int(base::SafeRound(logoTop * 1.2));
		const auto particlesRect = logoRect.marginsAdded(
			{ paddingAdd, paddingAdd, paddingAdd, paddingAdd });
		state->particles->paint(p, particlesRect, crl::now(), false);
		if (!anim::Disabled() && !state->animation.animating()) {
			state->animation.start();
		}
		p.drawImage(logoRect, state->logo);
	}, cover->lifetime());
}

not_null<Ui::RpWidget*> AddInfoCard(
		not_null<Ui::VerticalLayout*> container,
		InfoCardDescriptor descriptor,
		style::margins margins = style::margins(12, 0, 12, 0)) {
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
		auto gradient = QLinearGradient(QPointF(0., 0.), QPointF(width, height));
		gradient.setColorAt(0., palette.top);
		gradient.setColorAt(1., palette.bottom);
		p.setPen(QPen(palette.border, 1.));
		p.setBrush(gradient);
		p.drawRoundedRect(outer, 22., 22.);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(palette.accent.red(), palette.accent.green(), palette.accent.blue(), 32));
		p.drawEllipse(QRectF(width - 82., 14., 64., 64.));
		if (descriptor.icon) {
			const auto bubble = QRect(18, 18, 42, 42);
			p.setBrush(QColor(255, 255, 255, 165));
			p.drawEllipse(bubble);
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
		style::margins margins = style::margins(12, 0, 12, 0)) {
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
		auto gradient = QLinearGradient(QPointF(0., 0.), QPointF(width, height));
		gradient.setColorAt(0., palette.top);
		gradient.setColorAt(1., palette.bottom);
		p.setPen(QPen(palette.border, 1.));
		p.setBrush(gradient);
		p.drawRoundedRect(outer, 24., 24.);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(badgePalette.fill.red(), badgePalette.fill.green(), badgePalette.fill.blue(), 46));
		p.drawEllipse(QRectF(width - 96., 12., 76., 76.));
		if (!peer) {
			const auto userpicRect = QRect(18, 20, 58, 58);
			p.setBrush(QColor(255, 255, 255, 176));
			p.drawEllipse(userpicRect);
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
		style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
	raw->resizeToWidth(container->width()
		- st::boxRowPadding.left()
		- st::boxRowPadding.right());
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
		style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
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
		style::margins(12, 0, 12, 0));
	button->setClickedCallback(std::move(callback));
	if (!description.isEmpty()) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(description),
				st::defaultFlatLabel),
			style::margins(26, -2, 26, 6),
			style::al_top);
	}
	return button;
}

not_null<Ui::RpWidget*> AddBadgePill(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		OnboardingBadgeTone tone,
		style::margins margins = style::margins(26, 4, 26, 0)) {
	const auto badge = container->add(
		object_ptr<Ui::RpWidget>(container),
		margins,
		style::al_top);
	const auto palette = BadgeColors(tone);
	const auto badgeHeight = st::semiboldFont->height + 12;
	const auto horizontalPadding = 12;
	container->widthValue() | rpl::on_next([=](int width) {
		badge->resize(std::max(0, width), badgeHeight);
	}, badge->lifetime());
	badge->resize(std::max(0, container->width()), badgeHeight);
	badge->paintRequest() | rpl::on_next([=] {
		auto p = Painter(badge);
		auto hq = PainterHighQualityEnabler(p);
		p.setFont(st::semiboldFont);
		const auto pillWidth = std::min(
			badge->width(),
			st::semiboldFont->width(text) + (horizontalPadding * 2));
		const auto rect = QRectF(0, 0, pillWidth, badge->height() - 1)
			.adjusted(0.5, 0.5, -0.5, -0.5);
		p.setPen(QPen(palette.border, 1.));
		p.setBrush(palette.fill);
		p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
		p.setPen(palette.fg);
		p.drawText(
			QRect(
				horizontalPadding,
				0,
				std::max(1, pillWidth - (horizontalPadding * 2)),
				badge->height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			text);
	}, badge->lifetime());
	return badge;
}

not_null<Ui::RpWidget*> AddStepTimeline(
		not_null<Ui::VerticalLayout*> container,
		Step current,
		style::margins margins = style::margins(26, 4, 26, 4)) {
	const auto wrap = container->add(
		object_ptr<Ui::RpWidget>(container),
		margins,
		style::al_top);
	const auto steps = std::vector<Step>{
		Step::Welcome,
		Step::Presets,
		Step::PluginsInfo,
		Step::PluginsInstall,
		Step::MenuCustomization,
		Step::ShellMode,
		Step::ExperimentalTips,
		Step::Finish,
	};
	struct State {
		std::vector<QRect> rects;
		int height = 0;
	};
	const auto state = wrap->lifetime().make_state<State>();
	const auto currentIndex = StepIndex(current);
	const auto relayout = [=](int width) {
		const auto available = std::max(width, 220);
		const auto metrics = QFontMetrics(st::semiboldFont->f);
		const auto pillHeight = metrics.height() + 12;
		const auto gap = 8;
		const auto horizontalPadding = 12;
		auto x = 0;
		auto y = 0;
		state->rects.clear();
		state->rects.reserve(steps.size());
		for (const auto step : steps) {
			const auto text = StepShortTitle(step);
			const auto pillWidth = std::min(
				available,
				metrics.horizontalAdvance(text) + (horizontalPadding * 2));
			if (x && (x + pillWidth > available)) {
				x = 0;
				y += pillHeight + gap;
			}
			state->rects.push_back(QRect(x, y, pillWidth, pillHeight));
			x += pillWidth + gap;
		}
		state->height = y + pillHeight;
		wrap->resize(width, state->height);
		wrap->update();
	};
	wrap->widthValue() | rpl::on_next(relayout, wrap->lifetime());
	relayout(std::max(wrap->width(), container->width() - 52));
	wrap->paintRequest() | rpl::on_next([=] {
		auto p = Painter(wrap);
		auto hq = PainterHighQualityEnabler(p);
		p.setFont(st::semiboldFont);
		for (auto i = 0, count = int(steps.size()); i != count; ++i) {
			const auto step = steps[i];
			const auto &rect = state->rects[i];
			const auto index = StepIndex(step);
			const auto active = (index == currentIndex);
			const auto completed = (index < currentIndex);
			const auto palette = BadgeColors(
				active
					? StepBadgeTone(step)
					: (completed
						? OnboardingBadgeTone::Trusted
						: OnboardingBadgeTone::Pending));
			auto fill = palette.fill;
			if (!active && !completed) {
				fill = QColor(255, 255, 255, 180);
			} else if (active) {
				fill = QColor(
					palette.fill.red(),
					palette.fill.green(),
					palette.fill.blue(),
					68);
			}
			p.setPen(QPen(palette.border, 1.));
			p.setBrush(fill);
			p.drawRoundedRect(
				QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
				rect.height() / 2.,
				rect.height() / 2.);
			p.setPen(palette.fg);
			p.drawText(rect, Qt::AlignCenter, StepShortTitle(step));
		}
	}, wrap->lifetime());
	return wrap;
}

not_null<Ui::FlatLabel*> AddSecondaryNote(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		style::margins margins = style::margins(26, -2, 26, 4)) {
	return container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(text),
			st::defaultFlatLabel),
		margins,
		style::al_top);
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
		style::margins(12, 0, 12, 0));
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
		box->setWidth(st::boxWideWidth * 5 / 4);
		box->setStyle(st::stakeBox);
		box->setNoContentMargin(true);

		const auto container = box->verticalLayout();
		AddUniqueCloseButton(box);
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
		const auto controller = args.controller;

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
		const auto openExperimental = [=] {
			markCompleted();
			box->closeBox();
			const auto weakController = base::make_weak(controller);
			QTimer::singleShot(0, [=] {
				if (const auto controller = weakController.get()) {
					controller->showSettings(Settings::Experimental::Id());
				}
			});
		};
		box->boxClosing(
		) | rpl::on_next([=] {
			if ((state->step == Step::Finish) && !state->completionStored) {
				markCompleted();
			}
		}, box->lifetime());

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

			const auto addTitle = [&](const QString &title, const QString &subtitle) {
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(title),
						st::sessionBigName),
					style::margins(st::boxRowPadding.left(), 12, st::boxRowPadding.right(), 0),
					style::al_top);
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(subtitle),
						st::boxLabel),
					style::margins(st::boxRowPadding.left(), 6, st::boxRowPadding.right(), 0),
					style::al_top);
			};
			const auto addStepBadge = [&](Step step) {
				AddBadgePill(
					container,
					StepBadgeText(step),
					StepBadgeTone(step),
					style::margins(
						st::boxRowPadding.left(),
						12,
						st::boxRowPadding.right(),
						0));
			};
			const auto addStepTimeline = [&](Step step) {
				AddStepTimeline(
					container,
					step,
					style::margins(
						st::boxRowPadding.left(),
						6,
						st::boxRowPadding.right(),
						0));
			};
			const auto goToStep = [&](Step step) {
				state->step = step;
				(*rebuild)();
			};
			const auto addBackLink = [&] {
				if (const auto previous = PreviousStep(state->step)) {
					AddLinkAction(container, RuEn("Назад", "Back"), [=] {
						goToStep(*previous);
					});
				}
			};
			const auto pluginStatusText = [](const AstrogramOnboardingPlugin &plugin) {
				if (plugin.invalidServerData) {
					return RuEn(
						"серверная карточка требует обновления",
						"server card needs refresh");
				} else if (plugin.pendingServerData) {
					return RuEn(
						"карточка ещё загружается с сервера",
						"card is still loading from the server");
				}
				return RuEn(
					"рекомендация сервера",
					"server recommendation");
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
				AddHeroCover(
					container,
					RuEn("Добро пожаловать в", "Welcome to"),
					RuEn(
						"Astrogram Desktop\nСейчас аккуратно соберём твой старт: пресет, доверенные плагины, меню и финальные точки входа без резкой ломки привычного Telegram Desktop.",
						"Astrogram Desktop\nLet's assemble your starting setup: preset, trusted plugins, menus and final entry points without abruptly breaking the familiar Telegram Desktop feel."),
					QColor(0x08, 0x15, 0x11),
					QColor(0x10, 0x2b, 0x20),
					QColor(0x2d, 0xd1, 0x85),
					QColor(0xa1, 0xff, 0xcc));
				addStepBadge(Step::Welcome);
				addStepTimeline(Step::Welcome);
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Astrogram settings", "Astrogram settings"),
						.title = RuEn(
							"Главная точка входа Astrogram",
							"Your main Astrogram home"),
						.description = RuEn(
							"Здесь живут anti-recall, ghost mode, local premium, плагины, экспериментальные меню и будущие клиентские функции. Гайд просто раскладывает старт по понятным шагам.",
							"This is where anti-recall, ghost mode, local premium, plugins, experimental menus and future client-only features live. The guide just lays out the start in a clean order."),
						.footer = RuEn("Дом Astrogram-функций", "Home of Astrogram features"),
						.icon = &st::menuIconSettings,
						.tone = OnboardingBadgeTone::Official,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Быстрый старт", "Fast start"),
						.title = RuEn(
							"Сначала пресет, потом плагины и меню",
							"Preset first, then plugins and menu"),
						.description = RuEn(
							"Шаги не свалены в кучу: сначала базовый профиль, потом доверенные рекомендации AstroPlugins, затем ручная настройка меню и только после этого shell-режимы.",
							"The steps are not mashed together: first a baseline profile, then trusted AstroPlugins recommendations, then manual menu tuning, and only after that shell modes."),
						.footer = RuEn("Нормальный дефолт без лишней агрессии", "A sane default without aggressive changes"),
						.icon = &st::menuIconCustomize,
						.tone = OnboardingBadgeTone::Trusted,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Experimental", "Experimental"),
						.title = RuEn(
							"Живые editor-ы и shell-режимы никуда не спрятаны",
							"Live editors and shell modes stay visible"),
						.description = RuEn(
							"Гайд ведёт в реальные поверхности клиента: после него можно открыть Experimental и дотюнить боковую панель, shell и карточки меню уже без скрытых жестов.",
							"The guide leads into real client surfaces: after it you can open Experimental and fine-tune the side panel, shell and menu cards without hidden gestures."),
						.footer = RuEn("Guide only shortens the path", "The guide only shortens the path"),
						.icon = &st::menuIconExperimental,
						.tone = OnboardingBadgeTone::Pending,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddBadgePill(
					container,
					RuEn("anti-recall · ghost mode · local premium", "anti-recall · ghost mode · local premium"),
					OnboardingBadgeTone::Trusted,
					style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
				AddSecondaryNote(
					container,
					RuEn(
						"Ничего из этого не забирает возможность вернуться к более спокойной раскладке позже.",
						"None of this removes your ability to go back to a calmer setup later."),
					style::margins(st::boxRowPadding.left(), -2, st::boxRowPadding.right(), 4));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Продолжить", "Continue"), [=] {
					goToStep(Step::Presets);
				});
			} break;
			case Step::Presets: {
				addStepBadge(Step::Presets);
				addStepTimeline(Step::Presets);
				addTitle(
					RuEn("Выбери стартовый пресет", "Choose a startup preset"),
					RuEn(
						"Это только стартовые prefs клиента: выбор нужен, чтобы сразу не собирать базу по десятку переключателей вручную.",
						"These are only the client's starter prefs: the choice is here so you don't have to assemble the basics through ten different toggles right away."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Что меняется", "What changes"),
						.title = RuEn(
							"Только стартовые привычки клиента",
							"Only the client's starter habits"),
						.description = RuEn(
							"Пресет не закрывает путь назад: он просто раскладывает стартовый набор опций, а дальше всё можно вручную подкрутить в Astrogram settings.",
							"A preset does not close the way back: it simply lays out the starter option set, and then everything can still be tuned manually in Astrogram settings."),
						.footer = RuEn("Любой вариант можно поменять позже", "Any choice can be changed later"),
						.icon = &st::menuIconSettings,
						.tone = OnboardingBadgeTone::Pending,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Рекомендованный", "Recommended"),
					RuEn(
						"local premium, секунды сообщений, результаты опросов до голосования, anti-recall и нормальные Astrogram-дефолты.",
						"local premium, message seconds, poll results before voting, anti-recall and sane Astrogram defaults."),
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
						"ghost mode, скрытие read/online/typing, anti-recall и более мягкая история удалённых сообщений.",
						"ghost mode, hidden read/online/typing, anti-recall and a softer deleted-message history."),
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
						"Только чистая база Astrogram и максимальная близость к Telegram Desktop по ощущению.",
						"Only the clean Astrogram basics and the closest feel to Telegram Desktop."),
					&st::menuIconPalette,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Minimal);
						}
						goToStep(Step::PluginsInfo);
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddLinkAction(container, RuEn("Выбрать позже", "Choose later"), [=] {
					goToStep(Step::PluginsInfo);
				});
				addBackLink();
				Ui::AddSkip(container, st::settingsCheckboxesSkip);
			} break;
			case Step::PluginsInfo: {
				addStepBadge(Step::PluginsInfo);
				addStepTimeline(Step::PluginsInfo);
				addTitle(
					RuEn("Плагины внутри Astrogram", "Plugins inside Astrogram"),
					RuEn(
						"Раздел Plugins / Плагины — это встроенный менеджер пакетов, документации и диагностики. Сейчас быстро покажем, что именно в нём живёт.",
						"The Plugins / Плагины section is a built-in package, documentation and diagnostics manager. Let's quickly show what actually lives there."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Plugins / Плагины", "Plugins / Плагины"),
						.title = RuEn(
							".tgd-пакеты живут прямо в клиенте",
							".tgd packages live directly in the client"),
						.description = RuEn(
							"Установка идёт из менеджера плагинов, а каждый пакет дальше можно открыть отдельно в Settings > Plugins без обходных путей.",
							"Installation happens through the plugin manager, and each package can then be opened separately in Settings > Plugins without detours."),
						.footer = RuEn("Встроенный менеджер пакетов", "Built-in package manager"),
						.icon = &st::menuIconDownload,
						.tone = OnboardingBadgeTone::Trusted,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Верхнее меню", "Top bar menu"),
						.title = RuEn(
							"Documentation, Runtime & Diagnostics, папка и safe mode",
							"Documentation, Runtime & Diagnostics, folder and safe mode"),
						.description = RuEn(
							"В разделе Plugins сверху уже есть реальные точки входа: Documentation, Runtime & Diagnostics, Open Plugins Folder и Enable/Disable Safe Mode.",
							"The Plugins section already exposes real top-bar entries: Documentation, Runtime & Diagnostics, Open Plugins Folder and Enable/Disable Safe Mode."),
						.footer = RuEn("Без скрытых unlock-жестов", "No hidden unlock gestures"),
						.icon = &st::menuIconFaq,
						.tone = OnboardingBadgeTone::Official,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("AstroPlugins", "AstroPlugins"),
						.title = RuEn(
							"Первые 3 рекомендации приходят server-driven",
							"The first 3 recommendations arrive server-driven"),
						.description = RuEn(
							"Onboarding берёт стартовые пакеты из доверенного канала AstroPlugins: @astroplugin / -1003814280064. Следующий экран уже покажет живые карточки постов и сами .tgd-пакеты.",
							"The onboarding flow pulls starter packages from the trusted AstroPlugins channel: @astroplugin / -1003814280064. The next screen will already show the live post cards and the actual .tgd packages."),
						.footer = RuEn("Доверенный канал рекомендаций", "Trusted recommendation channel"),
						.icon = &st::menuIconIpAddress,
						.tone = OnboardingBadgeTone::Pending,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Показать 3 рекомендации", "Show 3 recommendations"), [=] {
					goToStep(Step::PluginsInstall);
				});
				if (args.openAllPlugins) {
					AddLinkAction(container, RuEn("Открыть раздел Plugins", "Open Plugins"), [=] {
						args.openAllPlugins();
					});
				}
				AddLinkAction(container, RuEn("Позже", "Later"), [=] {
					goToStep(Step::MenuCustomization);
				});
				addBackLink();
			} break;
			case Step::PluginsInstall: {
				if (!state->autoReloadTriggered && args.reloadPlugins) {
					state->autoReloadTriggered = true;
					(*requestPluginsReload)();
				}
				addStepBadge(Step::PluginsInstall);
				addStepTimeline(Step::PluginsInstall);
				addTitle(
					RuEn("3 рекомендации AstroPlugins", "3 AstroPlugins recommendations"),
					RuEn(
						"Карточка канала и первые пакеты подтягиваются server-driven из app config и живых постов AstroPlugins прямо во время онбординга, без перезапуска клиента.",
						"The channel card and the first packages are pulled server-driven from app config plus live AstroPlugins posts right during onboarding, without restarting the client."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				const auto fallbackPluginsTitle = args.pluginsChannelTitle.isEmpty()
					? RuEn("AstroPlugins", "AstroPlugins")
					: args.pluginsChannelTitle;
				AddChannelCard(
					container,
					state->pluginsChannelPeer,
					args.pluginsChannelId,
					fallbackPluginsTitle,
					args.pluginsChannelSubtitle,
					false,
					[=] {
						if (args.subscribePluginsChannel) {
							args.subscribePluginsChannel();
						}
					});
				AddBadgePill(
					container,
					ChannelCardBadgeText(state->pluginsChannelPeer, false),
					ChannelCardBadgeTone(state->pluginsChannelPeer, false));
				AddSecondaryNote(
					container,
					ChannelCardDetailText(
						state->pluginsChannelPeer,
						args.pluginsChannelId,
						false));
				if (args.subscribePluginsChannel) {
					AddPrimaryButton(container, RuEn("Открыть AstroPlugins", "Open AstroPlugins"), [=] {
						args.subscribePluginsChannel();
					});
				}
				if (args.reloadPlugins) {
					AddLinkAction(container, RuEn("Обновить рекомендации с сервера", "Refresh recommendations from server"), [=] {
						(*requestPluginsReload)();
					});
				}
				if (state->reloadingPlugins) {
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(RuEn(
								"Проверяем свежие рекомендации и подтягиваем карточки постов…",
								"Checking fresh recommendations and fetching post cards…")),
							st::boxLabel),
						style::margins(st::boxRowPadding.left(), 8, st::boxRowPadding.right(), 0),
						style::al_top);
				}
				if (plugins->empty()) {
					AddInfoCard(
						container,
						InfoCardDescriptor{
							.eyebrow = RuEn("Server-driven feed", "Server-driven feed"),
							.title = state->reloadingPlugins
								? RuEn(
									"Тянем живые карточки из AstroPlugins",
									"Fetching live AstroPlugins cards")
								: RuEn(
									"Рекомендации ещё не приехали",
									"Recommendations have not arrived yet"),
							.description = state->reloadingPlugins
								? RuEn(
									"Сейчас клиент сверяет post id из app config, подтягивает посты канала и собирает .tgd-карточки прямо на этом экране.",
									"The client is now resolving post ids from app config, fetching the channel posts and assembling the .tgd cards right on this screen.")
								: RuEn(
									"Как только сервер отдаст `astrogram_onboarding_plugin_post_ids`, этот экран сам наполнится карточками и кнопками установки.",
									"As soon as the server provides `astrogram_onboarding_plugin_post_ids`, this screen will populate itself with cards and install buttons."),
							.footer = RuEn("Окно не требует перезапуска", "This screen does not require a restart"),
							.icon = &st::menuIconDownload,
							.tone = OnboardingBadgeTone::Pending,
						});
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(state->reloadingPlugins
								? RuEn(
									"Ждём ответ сервера с рекомендованными пакетами…",
									"Waiting for the server to provide recommended packages…")
								: RuEn(
									"Сервер ещё не передал рекомендованные пакеты. Когда появятся `astrogram_onboarding_plugin_post_ids`, они подтянутся сюда автоматически.",
									"The server has not provided recommended packages yet. Once `astrogram_onboarding_plugin_post_ids` appears, they will show up here automatically.")),
							st::boxLabel),
						style::margins(st::boxRowPadding.left(), 8, st::boxRowPadding.right(), 0),
						style::al_top);
				} else {
					const auto visiblePlugins = std::min<int>(plugins->size(), 3);
					Ui::AddSkip(container, st::settingsCheckboxesSkip / 4);
					Ui::AddSubsectionTitle(
						container,
						rpl::single(RuEn("3 стартовые рекомендации", "3 starter recommendations")));
					AddSecondaryNote(
						container,
						RuEn(
							"Каждая карточка ниже собирается из живого поста AstroPlugins и проверенного .tgd-пакета. Тут сознательно показываем только первые три стартовые рекомендации.",
							"Each card below is assembled from a live AstroPlugins post plus a verified .tgd package. We intentionally show only the first three starter recommendations here."),
						style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 6));
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
						const auto actionText = plugin.invalidServerData
							? RuEn("Обновить список", "Refresh list")
							: RuEn("Установить", "Install");
						AddInfoCard(
							container,
							InfoCardDescriptor{
								.eyebrow = pluginStatusText(plugin),
								.title = texts.title,
								.description = texts.description,
								.footer = texts.sourceLabel,
								.icon = &st::menuIconDownload,
								.tone = PluginSourceBadgeTone(plugin),
							});
						AddBadgePill(
							container,
							PluginSourceBadgeText(plugin),
							PluginSourceBadgeTone(plugin));
						AddSecondaryNote(
							container,
							PluginSourceBadgeDetailText(plugin));
						AddPrimaryButton(
							container,
							actionText,
							[action] {
								if (action) {
									action();
								}
							});
						Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
					}
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Продолжить к меню", "Continue to menu"), [=] {
					goToStep(Step::MenuCustomization);
				});
				if (args.openAllPlugins) {
					AddLinkAction(container, RuEn("Открыть весь список в Plugins", "Open the full list in Plugins"), [=] {
						args.openAllPlugins();
					});
				}
				addBackLink();
			} break;
			case Step::MenuCustomization: {
				addStepBadge(Step::MenuCustomization);
				addStepTimeline(Step::MenuCustomization);
				addTitle(
					RuEn("Кастомизируй меню Astrogram", "Customize Astrogram menus"),
					RuEn(
						"Сначала разложим меню и боковую панель, а уже потом выберем стартовый shell-режим. Здесь важна именно понятная ручная раскладка.",
						"First lay out the menus and side panel, then choose the starter shell mode. The point here is a clean manual layout, not a fake preview."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Experimental -> Боковая панель", "Experimental -> Side panel"),
						.title = RuEn(
							"Ручная раскладка боковой панели уже в клиенте",
							"Manual side panel layout is already inside the client"),
						.description = RuEn(
							"Внутри Experimental можно менять порядок пунктов боковой панели руками, скрывать их и сразу видеть скрытые элементы отдельными таблетками внизу без отдельного visual editor.",
							"Inside Experimental you can reorder side panel items by hand, hide them, and immediately see hidden items as separate pills at the bottom without a separate visual editor."),
						.footer = RuEn("Порядок, скрытие и возврат пунктов без лишнего визуального редактора", "Order, hide and restore items without an extra visual editor"),
						.icon = &st::menuIconEdit,
						.tone = OnboardingBadgeTone::Official,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Experimental -> Настройки оболочки", "Experimental -> Shell settings"),
						.title = RuEn(
							"Широкая оболочка и левый край настраиваются отдельно",
							"The wider shell and left-edge mode are configured separately"),
						.description = RuEn(
							"В том же разделе лежат expanded side panel, left-edge и widened settings, чтобы после гайда быстро дотюнить оболочку под себя.",
							"In the same section you will find expanded side panel, left-edge and widened settings, so after onboarding you can fine-tune the shell quickly."),
						.footer = RuEn("Только реальные runtime-настройки без лишних preview-поверхностей", "Only real runtime settings without extra preview surfaces"),
						.icon = &st::menuIconCustomize,
						.tone = OnboardingBadgeTone::Trusted,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Experimental shell features", "Experimental shell features"),
						.title = RuEn(
							"Пресеты живут в гайде, ручная доводка остаётся в настройках",
							"Presets live in onboarding, manual tuning stays in settings"),
						.description = RuEn(
							"На следующем шаге ты выберешь стартовый shell-пресет прямо здесь, а внутри Experimental останутся только обычные нативные настройки без пресетов и без visual editor-мусора.",
							"On the next step you will choose the starter shell preset right here, while Experimental keeps only regular native settings without presets and without visual-editor clutter."),
						.footer = RuEn("Гайд даёт старт, Experimental оставляет только нативную ручную настройку", "Onboarding gives the start, Experimental keeps only native manual tuning"),
						.icon = &st::menuIconExperimental,
						.tone = OnboardingBadgeTone::Pending,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddBadgePill(
					container,
					RuEn(
						"ручная раскладка меню · скрытые элементы снизу · shell-настройки",
						"manual menu layout · hidden pills below · shell settings"),
					OnboardingBadgeTone::Trusted,
					style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
				AddSecondaryNote(
					container,
					RuEn(
						"Сейчас гайд только даёт хороший старт. Точная ручная доводка меню и оболочки остаётся в Experimental без скрытых жестов и без отдельного visual editor.",
						"The guide only gives you a solid start. Precise manual menu and shell tuning stays in Experimental without hidden gestures and without a separate visual editor."),
					style::margins(st::boxRowPadding.left(), -2, st::boxRowPadding.right(), 4));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Продолжить к shell-режимам", "Continue to shell modes"), [=] {
					goToStep(Step::ShellMode);
				});
				AddLinkAction(container, RuEn("Открыть Experimental сейчас", "Open Experimental now"), openExperimental);
				addBackLink();
			} break;
			case Step::ShellMode: {
				addStepBadge(Step::ShellMode);
				addStepTimeline(Step::ShellMode);
				addTitle(
					RuEn("Выбери shell-режим", "Choose a shell mode"),
					RuEn(
						"После шага про меню здесь выбирается стартовый shell-пресет. Точная ручная доводка после этого остаётся в Experimental обычными нативными переключателями.",
						"After the menu step, choose the starter shell preset here. Precise manual tuning after that remains in Experimental through regular native switches."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Experimental", "Experimental"),
						.title = RuEn(
							"Ручная настройка боковой панели",
							"Manual side panel tuning"),
						.description = RuEn(
							"Внутри Experimental останутся только нативные настройки боковой панели: порядок, скрытие, разделители, профильный блок и нижние таблетки для скрытых пунктов.",
							"Inside Experimental only the native side panel settings remain: ordering, hiding, separators, profile block placement and bottom pills for hidden items."),
						.footer = RuEn("Experimental -> Боковая панель", "Experimental -> Side panel"),
						.icon = &st::menuIconEdit,
						.tone = OnboardingBadgeTone::Official,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Экспериментальные режимы оболочки", "Experimental shell modes"),
						.title = RuEn(
							"Expanded side panel, widened settings и left-edge",
							"Expanded side panel, widened settings and left-edge"),
						.description = RuEn(
							"Пресеты ниже заранее включают те же runtime-переключатели, которые потом можно дотюнить вручную в Experimental, если захочется больше или спокойнее.",
							"The presets below pre-apply the same runtime switches that you can later fine-tune by hand in Experimental if you want a stronger or calmer shell."),
						.footer = RuEn("Экспериментальные режимы оболочки / Experimental shell modes", "Experimental shell modes"),
						.icon = &st::menuIconExperimental,
						.tone = OnboardingBadgeTone::Pending,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Сбалансированный", "Balanced"),
					RuEn(
						"Компактная ширина и спокойный shell для повседневной работы.",
						"Compact width and a calm shell for everyday work."),
					&st::menuIconPalette,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Balanced); });
				AddChoiceButton(
					container,
					RuEn("Фокус на меню", "Focused"),
					RuEn(
						"Делает боковую панель шире и выразительнее, но не уводит настройки к левому краю.",
						"Makes the side panel wider and bolder without moving settings to the left edge yet."),
					&st::menuIconCustomize,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Focused); });
				AddChoiceButton(
					container,
					RuEn("Широкий интерфейс", "Wide shell"),
					RuEn(
						"Максимальный shell: widened settings, left-edge и самая заметная оболочка Astrogram.",
						"The fullest shell: widened settings, left-edge and the most expressive Astrogram shell."),
					&st::menuIconExperimental,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Wide); });
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Открыть Experimental сейчас", "Open Experimental now"), openExperimental);
				AddLinkAction(container, RuEn("Продолжить к подсказкам", "Continue to tips"), [=] {
					goToStep(Step::ExperimentalTips);
				});
				addBackLink();
			} break;
			case Step::ExperimentalTips: {
				addStepBadge(Step::ExperimentalTips);
				addStepTimeline(Step::ExperimentalTips);
				addTitle(
					RuEn("Живые подсказки по Experimental", "Live Experimental tips"),
					RuEn(
						"Experimental в Astrogram уже не набор demo-only флажков: онбординг ведёт в реальные runtime-поверхности клиента и оставляет только нужные ручные точки входа.",
						"Experimental in Astrogram is no longer a pile of demo-only flags: onboarding leads into real runtime surfaces of the client and leaves only the manual entry points you actually need."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Редактор бокового меню / Side menu editor", "Side menu editor"),
						.title = RuEn(
							"menu_layout.json, restore-tray и live preview",
							"menu_layout.json, restore tray and live preview"),
						.description = RuEn(
							"Редактор уже работает напрямую с menu_layout.json: видимые пункты остаются в основном списке, скрытые уходят в restore-tray, а выбранное действие сразу подсвечивается без переоткрытия поверхности.",
							"The editor already works directly with menu_layout.json: visible items stay in the main list, hidden ones move into the restore tray, and the selected action is highlighted immediately without reopening the surface."),
						.footer = RuEn("Редактор бокового меню", "Side menu editor"),
						.icon = &st::menuIconEdit,
						.tone = OnboardingBadgeTone::Official,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("Экспериментальные режимы оболочки / Experimental shell modes", "Experimental shell modes"),
						.title = RuEn(
							"Expanded side panel, left-edge и widened settings уже runtime",
							"Expanded side panel, left-edge and widened settings are already runtime"),
						.description = RuEn(
							"Эти переключатели больше не demo-only: они вынесены в явный верхний блок Experimental, пишутся в runtime prefs и сразу отражаются и в shell, и в preview.",
							"These switches are no longer demo-only: they now live in a dedicated top Experimental block, write into runtime prefs and immediately reflect in both the shell and the preview."),
						.footer = RuEn("Экспериментальные режимы оболочки", "Experimental shell modes"),
						.icon = &st::menuIconExperimental,
						.tone = OnboardingBadgeTone::Pending,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("После гайда / Manual tuning", "After onboarding / Manual tuning"),
						.title = RuEn(
							"После гайда остаётся только нативная ручная настройка",
							"After onboarding only native manual tuning remains"),
						.description = RuEn(
							"Этот гайд специально убирает лишнюю сложность на старте: после него в Experimental остаются только обычные нативные настройки оболочки и боковой панели без лишних дубликатов.",
							"This guide intentionally removes extra complexity at the start: after it, Experimental keeps only the regular native shell and side panel settings without duplicate clutter."),
						.footer = RuEn("Нативные настройки без visual editor", "Native settings without a visual editor"),
						.icon = &st::menuIconCustomize,
						.tone = OnboardingBadgeTone::Trusted,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Открыть Experimental", "Open Experimental"), openExperimental);
				AddLinkAction(container, RuEn("Продолжить к финалу", "Continue to final"), [=] {
					goToStep(Step::Finish);
				});
				addBackLink();
			} break;
			case Step::Finish: {
				AddHeroCover(
					container,
					RuEn("Всё готово", "You're all set"),
					RuEn(
						"Профиль, плагины и боковое меню уже разложены. Ниже оставили две понятные точки на будущее: официальный канал и поддержку Astrogram.",
						"Your profile, plugins and side menu are already laid out. Below are the two clean next stops: the official channel and Astrogram support."),
					QColor(0x08, 0x13, 0x1f),
					QColor(0x0f, 0x2b, 0x3f),
					QColor(0x44, 0xc0, 0xff),
					QColor(0x9c, 0xff, 0xd3));
				addStepBadge(Step::Finish);
				addStepTimeline(Step::Finish);
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddInfoCard(
					container,
					InfoCardDescriptor{
						.eyebrow = RuEn("После гайда", "After onboarding"),
						.title = RuEn(
							"Дальше достаточно двух точек входа",
							"Two entry points are enough from here"),
						.description = RuEn(
							"Официальный канал держит рядом сборки и новости Astrogram, а support-box ведёт в донат и серверный значок без лишнего поиска по меню.",
							"The official channel keeps Astrogram builds and news nearby, while the support box leads into donations and the server-side badge without making you hunt through menus."),
						.footer = RuEn("Канал + поддержка", "Channel + support"),
						.icon = &st::menuIconGiftPremium,
						.tone = OnboardingBadgeTone::Official,
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				AddChannelCard(
					container,
					state->officialChannelPeer,
					args.officialChannelId,
					args.officialChannelTitle.trimmed().isEmpty()
						? RuEn("Astrogram", "Astrogram")
						: args.officialChannelTitle.trimmed(),
					args.officialChannelSubtitle.trimmed().isEmpty()
						? RuEn(
							"Получай новости о сборках, клиентах и новых функциях Astrogram.",
							"Get updates about builds, client changes and new Astrogram features.")
						: args.officialChannelSubtitle,
					true,
					[=] {
						if (args.openOfficialChannel) {
							args.openOfficialChannel();
						}
					});
				AddBadgePill(
					container,
					ChannelCardBadgeText(state->officialChannelPeer, true),
					ChannelCardBadgeTone(state->officialChannelPeer, true));
				AddSecondaryNote(
					container,
					ChannelCardDetailText(
						state->officialChannelPeer,
						args.officialChannelId,
						true));
				if (args.openDonate) {
					AddChoiceButton(
						container,
						RuEn(
							"Поддержать разработку Astrogram",
							"Support Astrogram development"),
						RuEn(
							"Открывает донат-окно Astrogram с серверным значком подписчика.",
							"Opens the Astrogram support box with the server-side subscriber badge."),
						&st::menuIconGiftPremium,
						[=] {
							args.openDonate();
						});
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Завершить", "Finish"), finish);
				if (args.openOfficialChannel) {
					AddLinkAction(container, RuEn("Открыть канал Astrogram", "Open Astrogram channel"), [=] {
						args.openOfficialChannel();
					});
				}
				addBackLink();
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
