/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/unread_badge.h"

#include "data/data_emoji_statuses.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/profile/info_profile_badge.h"
#include "main/main_session.h"
#include "lang/lang_keys.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/power_saving.h"
#include "ui/unread_badge_paint.h"
#include "styles/style_dialogs.h"

#include <unordered_map>

namespace Ui {
namespace {

constexpr auto kPlayStatusLimit = 2;

struct PeerBadgeRuntimeState {
	PeerData *peer = nullptr;
	Info::Profile::Badge::Content content;
	Fn<void()> repaint;
	rpl::lifetime lifetime;
};

[[nodiscard]] auto &PeerBadgeRuntimeStates() {
	static auto states
		= std::unordered_map<const PeerBadge*, PeerBadgeRuntimeState>();
	return states;
}

void ResetPeerBadgeRuntimeState(const PeerBadge *badge) {
	auto &states = PeerBadgeRuntimeStates();
	if (const auto i = states.find(badge); i != end(states)) {
		i->second.lifetime.destroy();
		states.erase(i);
	}
}

void EnsurePeerBadgeRuntimeState(
		const PeerBadge *badge,
		not_null<PeerData*> peer,
		Fn<void()> repaint) {
	auto &state = PeerBadgeRuntimeStates()[badge];
	state.repaint = std::move(repaint);
	if (state.peer == peer.get()) {
		return;
	}
	state.lifetime.destroy();
	state.peer = peer;
	state.content = {};
	Info::Profile::BadgeContentForPeer(
		peer
	) | rpl::on_next([badge, peer](Info::Profile::Badge::Content content) {
		const auto i = PeerBadgeRuntimeStates().find(badge);
		if (i == end(PeerBadgeRuntimeStates()) || (i->second.peer != peer)) {
			return;
		}
		if (i->second.content == content) {
			return;
		}
		i->second.content = content;
		if (i->second.repaint) {
			i->second.repaint();
		}
	}, state.lifetime);
}

[[nodiscard]] const PeerBadgeRuntimeState *FindPeerBadgeRuntimeState(
		const PeerBadge *badge) {
	const auto &states = PeerBadgeRuntimeStates();
	const auto i = states.find(badge);
	return (i != end(states)) ? &i->second : nullptr;
}

} // namespace

struct PeerBadge::EmojiStatus {
	EmojiStatusId id;
	std::unique_ptr<Ui::Text::CustomEmoji> emoji;
	int skip = 0;
};

struct PeerBadge::BotVerifiedData {
	QImage cache;
	std::unique_ptr<Text::CustomEmoji> icon;
};

void UnreadBadge::setText(const QString &text, bool active) {
	_text = text;
	_active = active;
	const auto st = Dialogs::Ui::UnreadBadgeStyle();
	resize(
		std::max(st.font->width(text) + 2 * st.padding, st.size),
		st.size);
	update();
}

int UnreadBadge::textBaseline() const {
	const auto st = Dialogs::Ui::UnreadBadgeStyle();
	return ((st.size - st.font->height) / 2) + st.font->ascent;
}

void UnreadBadge::paintEvent(QPaintEvent *e) {
	if (_text.isEmpty()) {
		return;
	}

	auto p = QPainter(this);

	UnreadBadgeStyle unreadSt;
	unreadSt.muted = !_active;
	auto unreadRight = width();
	auto unreadTop = 0;
	PaintUnreadBadge(
		p,
		_text,
		unreadRight,
		unreadTop,
		unreadSt);
}

QString TextBadgeText(TextBadgeType type) {
	switch (type) {
	case TextBadgeType::Fake: return tr::lng_fake_badge(tr::now);
	case TextBadgeType::Scam: return tr::lng_scam_badge(tr::now);
	case TextBadgeType::Direct: return tr::lng_direct_badge(tr::now);
	}
	Unexpected("Type in TextBadgeText.");
}

QSize TextBadgeSize(TextBadgeType type) {
	const auto phrase = TextBadgeText(type);
	const auto phraseWidth = st::dialogsScamFont->width(phrase);
	const auto width = st::dialogsScamPadding.left()
		+ phraseWidth
		+ st::dialogsScamPadding.right();
	const auto height = st::dialogsScamPadding.top()
		+ st::dialogsScamFont->height
		+ st::dialogsScamPadding.bottom();
	return { width, height };
}

void DrawTextBadge(
		Painter &p,
		QRect rect,
		int outerWidth,
		const style::color &color,
		const QString &phrase,
		int phraseWidth) {
	PainterHighQualityEnabler hq(p);
	auto pen = color->p;
	pen.setWidth(st::lineWidth);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(rect, st::dialogsScamRadius, st::dialogsScamRadius);
	p.setFont(st::dialogsScamFont);
	if (style::DevicePixelRatio() > 1) {
		p.drawText(
			QRect(
				rect.x() + st::dialogsScamPadding.left(),
				rect.y() + st::dialogsScamPadding.top(),
				rect.width() - rect::m::sum::h(st::dialogsScamPadding),
				rect.height() - rect::m::sum::v(st::dialogsScamPadding)),
			Qt::AlignCenter,
			phrase);
	} else {
		p.drawTextLeft(
			rect.x() + st::dialogsScamPadding.left(),
			rect.y() + st::dialogsScamPadding.top(),
			outerWidth,
			phrase,
			phraseWidth);
	}
}

void DrawTextBadge(
		TextBadgeType type,
		Painter &p,
		QRect rect,
		int outerWidth,
		const style::color &color) {
	const auto phrase = TextBadgeText(type);
	DrawTextBadge(
		p,
		rect,
		outerWidth,
		color,
		phrase,
		st::dialogsScamFont->width(phrase));
}

PeerBadge::PeerBadge() = default;

PeerBadge::~PeerBadge() {
	ResetPeerBadgeRuntimeState(this);
}

int PeerBadge::drawGetWidth(Painter &p, Descriptor &&descriptor) {
	Expects(descriptor.customEmojiRepaint != nullptr);

	const auto peer = descriptor.peer;
	EnsurePeerBadgeRuntimeState(this, peer, descriptor.customEmojiRepaint);
	if ((descriptor.scam && (peer->isScam() || peer->isFake()))
		|| (descriptor.direct && peer->isMonoforum())) {
		return drawTextBadge(p, descriptor);
	}
	if (const auto state = FindPeerBadgeRuntimeState(this)
		; state && Info::Profile::IsAstrogramMiniBadgeContent(peer, state->content)) {
		_emojiStatus = nullptr;
		const auto rectForName = descriptor.rectForName;
		const auto bounds = QSize(
			descriptor.premium
				? std::max(descriptor.premium->width(), rectForName.height())
				: rectForName.height(),
			descriptor.premium
				? std::max(descriptor.premium->height(), rectForName.height())
				: rectForName.height());
		const auto size = Info::Profile::AstrogramMiniBadgeSize(bounds);
		const auto iconx = rectForName.x()
			+ qMin(
				descriptor.nameWidth,
				std::max(rectForName.width() - size.width(), 0));
		const auto icony = rectForName.y()
			+ (rectForName.height() - size.height()) / 2;
		p.drawImage(
			QRect(iconx, icony, size.width(), size.height()),
			Info::Profile::AstrogramMiniBadgeImage(
				descriptor.premiumFg
					? (*descriptor.premiumFg)->c
					: st::windowActiveTextFg->c));
		return size.width();
	}
	const auto verifyCheck = descriptor.verified && peer->isVerified();
	const auto premiumMark = descriptor.premium
		&& peer->session().premiumBadgesShown();
	const auto emojiStatus = premiumMark
		&& peer->emojiStatusId()
		&& (peer->isPremium() || peer->isChannel());
	const auto premiumStar = premiumMark
		&& !emojiStatus
		&& peer->isPremium();

	const auto paintVerify = verifyCheck
		&& (descriptor.prioritizeVerification
			|| descriptor.bothVerifyAndStatus
			|| !emojiStatus);
	const auto paintEmoji = emojiStatus
		&& (!paintVerify || descriptor.bothVerifyAndStatus);
	const auto paintStar = premiumStar && !paintVerify;

	auto result = 0;
	if (paintEmoji) {
		auto &rectForName = descriptor.rectForName;
		const auto verifyWidth = descriptor.verified->width();
		if (paintVerify) {
			rectForName.setWidth(rectForName.width() - verifyWidth);
		}
		result += drawPremiumEmojiStatus(p, descriptor);
		if (!paintVerify) {
			return result;
		}
		rectForName.setWidth(rectForName.width() + verifyWidth);
		descriptor.nameWidth += result;
	}
	if (paintVerify) {
		result += drawVerifyCheck(p, descriptor);
		return result;
	} else if (paintStar) {
		return drawPremiumStar(p, descriptor);
	}
	return 0;
}

int PeerBadge::drawTextBadge(Painter &p, const Descriptor &descriptor) {
	const auto type = [&] {
		if (descriptor.peer->isScam()) {
			return TextBadgeType::Scam;
		} else if (descriptor.peer->isFake()) {
			return TextBadgeType::Fake;
		}
		return TextBadgeType::Direct;
	}();
	const auto phrase = TextBadgeText(type);
	const auto phraseWidth = st::dialogsScamFont->width(phrase);
	const auto width = st::dialogsScamPadding.left()
		+ phraseWidth
		+ st::dialogsScamPadding.right();
	const auto height = st::dialogsScamPadding.top()
		+ st::dialogsScamFont->height
		+ st::dialogsScamPadding.bottom();
	const auto rectForName = descriptor.rectForName;
	const auto rect = QRect(
		(rectForName.x()
			+ qMin(
				descriptor.nameWidth + st::dialogsScamSkip,
				rectForName.width() - width)),
		rectForName.y() + (rectForName.height() - height) / 2,
		width,
		height);
	DrawTextBadge(
		p,
		rect,
		descriptor.outerWidth,
		*((type == TextBadgeType::Direct)
			? descriptor.direct
			: descriptor.scam),
		phrase,
		phraseWidth);
	return st::dialogsScamSkip + width;
}

int PeerBadge::drawVerifyCheck(Painter &p, const Descriptor &descriptor) {
	const auto iconw = descriptor.verified->width();
	const auto rectForName = descriptor.rectForName;
	const auto nameWidth = descriptor.nameWidth;
	descriptor.verified->paint(
		p,
		rectForName.x() + qMin(nameWidth, rectForName.width() - iconw),
		rectForName.y(),
		descriptor.outerWidth);
	return iconw;
}

int PeerBadge::drawPremiumEmojiStatus(
		Painter &p,
		const Descriptor &descriptor) {
	const auto peer = descriptor.peer;
	const auto id = peer->emojiStatusId();
	const auto rectForName = descriptor.rectForName;
	const auto iconw = descriptor.premium->width();
	const auto iconx = rectForName.x()
		+ qMin(descriptor.nameWidth, rectForName.width() - iconw);
	const auto icony = rectForName.y();
	if (!_emojiStatus) {
		_emojiStatus = std::make_unique<EmojiStatus>();
		const auto size = st::emojiSize;
		const auto emoji = Ui::Text::AdjustCustomEmojiSize(size);
		_emojiStatus->skip = (size - emoji) / 2;
	}
	if (_emojiStatus->id != id) {
		using namespace Ui::Text;
		auto &manager = peer->session().data().customEmojiManager();
		_emojiStatus->id = id;
		_emojiStatus->emoji = std::make_unique<LimitedLoopsEmoji>(
			manager.create(
				Data::EmojiStatusCustomId(id),
				descriptor.customEmojiRepaint),
			kPlayStatusLimit);
	}
	_emojiStatus->emoji->paint(p, {
		.textColor = (*descriptor.premiumFg)->c,
		.now = descriptor.now,
		.position = QPoint(
			iconx - 2 * _emojiStatus->skip,
			icony + _emojiStatus->skip),
		.paused = descriptor.paused || On(PowerSaving::kEmojiStatus),
	});
	return iconw - 4 * _emojiStatus->skip;
}

int PeerBadge::drawPremiumStar(Painter &p, const Descriptor &descriptor) {
	const auto rectForName = descriptor.rectForName;
	const auto iconw = descriptor.premium->width();
	const auto iconx = rectForName.x()
		+ qMin(descriptor.nameWidth, rectForName.width() - iconw);
	const auto icony = rectForName.y();
	_emojiStatus = nullptr;
	descriptor.premium->paint(p, iconx, icony, descriptor.outerWidth);
	return iconw;
}

void PeerBadge::unload() {
	_emojiStatus = nullptr;
	ResetPeerBadgeRuntimeState(this);
}

bool PeerBadge::ready(const BotVerifyDetails *details) const {
	if (!details || !*details) {
		_botVerifiedData = nullptr;
		return true;
	} else if (!_botVerifiedData) {
		return false;
	}
	if (!details->iconId) {
		_botVerifiedData->icon = nullptr;
	} else if (!_botVerifiedData->icon
		|| (_botVerifiedData->icon->entityData()
			!= Data::SerializeCustomEmojiId(details->iconId))) {
		return false;
	}
	return true;
}

void PeerBadge::set(
		not_null<const BotVerifyDetails*> details,
		Ui::Text::CustomEmojiFactory factory,
		Fn<void()> repaint) {
	if (!_botVerifiedData) {
		_botVerifiedData = std::make_unique<BotVerifiedData>();
	}
	if (details->iconId) {
		_botVerifiedData->icon = std::make_unique<Ui::Text::FirstFrameEmoji>(
			factory(
				Data::SerializeCustomEmojiId(details->iconId),
				{ .repaint = repaint }));
	}
}

int PeerBadge::drawVerified(
		QPainter &p,
		QPoint position,
		const style::VerifiedBadge &st) {
	const auto data = _botVerifiedData.get();
	if (!data) {
		return 0;
	}
	if (const auto icon = data->icon.get()) {
		icon->paint(p, {
			.textColor = st.color->c,
			.now = crl::now(),
			.position = position,
		});
		return icon->width();
	}
	return 0;
}

} // namespace Ui
