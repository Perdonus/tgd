/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_badge.h"

#include "data/data_changes.h"
#include "data/data_emoji_statuses.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "core/astrogram_channel_registry.h"
#include "info/profile/info_profile_values.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "main/main_session.h"
#include "styles/style_info.h"

#include <algorithm>

#include <QtCore/QHash>
#include <QtGui/QColor>
#include <QtGui/QPainter>

namespace Info::Profile {
namespace {

[[nodiscard]] bool HasPremiumClick(const Badge::Content &content) {
	return !content.astrogramMini
		&& (content.badge == BadgeType::Premium
			|| (content.badge == BadgeType::Verified
				&& content.emojiStatusId));
}

[[nodiscard]] QString BadgeTypeForLog(BadgeType type) {
	switch (type) {
	case BadgeType::None: return u"none"_q;
	case BadgeType::Verified: return u"verified"_q;
	case BadgeType::BotVerified: return u"bot_verified"_q;
	case BadgeType::Premium: return u"premium"_q;
	case BadgeType::Scam: return u"scam"_q;
	case BadgeType::Fake: return u"fake"_q;
	case BadgeType::Direct: return u"direct"_q;
	}
	return u"unknown"_q;
}

} // namespace

const QImage &AstrogramMiniBadgeBaseImage() {
	static const auto image = QImage(u":/gui/art/astrogram/mini_badge.png"_q);
	return image;
}

QImage AstrogramMiniBadgeImage(const QColor &color) {
	const auto &base = AstrogramMiniBadgeBaseImage();
	if (base.isNull() || !color.isValid()) {
		return base;
	}
	static auto cache = QHash<QRgb, QImage>();
	const auto key = color.rgba();
	if (const auto i = cache.constFind(key); i != cache.cend()) {
		return i.value();
	}
	auto tinted = QImage(base.size(), QImage::Format_ARGB32_Premultiplied);
	tinted.fill(Qt::transparent);
	{
		auto painter = QPainter(&tinted);
		painter.drawImage(0, 0, base);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(tinted.rect(), color);
	}
	cache.insert(key, tinted);
	return tinted;
}

QSize AstrogramMiniBadgeSize(const QSize &bounds) {
	auto result = AstrogramMiniBadgeBaseImage().size();
	if (result.isEmpty()) {
		return bounds;
	}
	result.scale(bounds, Qt::KeepAspectRatio);
	return result;
}

bool IsAstrogramMiniBadgeContent(
		not_null<PeerData*> peer,
		const Badge::Content &content) {
	if (content.astrogramMini) {
		return true;
	}
	switch (content.badge) {
	case BadgeType::Verified:
		return !peer->isVerified();
	case BadgeType::Premium:
		return !content.emojiStatusId
			|| (peer->emojiStatusId() != content.emojiStatusId);
	default:
		return false;
	}
}

Badge::Badge(
	not_null<QWidget*> parent,
	const style::InfoPeerBadge &st,
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	rpl::producer<Content> content,
	EmojiStatusPanel *emojiStatusPanel,
	Fn<bool()> animationPaused,
	int customStatusLoopsLimit,
	base::flags<BadgeType> allowed,
	bool useAstrogramMini)
: _parent(parent)
, _st(st)
, _session(session)
, _peer(peer)
, _emojiStatusPanel(emojiStatusPanel)
, _customStatusLoopsLimit(customStatusLoopsLimit)
, _useAstrogramMini(useAstrogramMini)
, _allowed(allowed)
, _animationPaused(std::move(animationPaused)) {
	std::move(
		content
	) | rpl::on_next([=](Content content) {
		setContent(content);
	}, _lifetime);
}

Badge::~Badge() = default;

Ui::RpWidget *Badge::widget() const {
	return _view.data();
}

void Badge::setContent(Content content) {
	const auto astrogramMini = content.astrogramMini;
	if (!astrogramMini && (!(_allowed & content.badge)
		|| (!_session->premiumBadgesShown()
			&& content.badge == BadgeType::Premium))) {
		content.badge = BadgeType::None;
	}
	if (!astrogramMini && !(_allowed & content.badge)) {
		content.badge = BadgeType::None;
	}
	if (_content == content) {
		return;
	}
	_content = content;
	_emojiStatus = nullptr;
	_view.destroy();
	if (_content.badge == BadgeType::None) {
		_updated.fire({});
		return;
	}
	_view.create(_parent);
	_view->show();
	switch (_content.badge) {
	case BadgeType::Verified:
	case BadgeType::BotVerified:
	case BadgeType::Premium: {
			const auto id = _content.emojiStatusId;
			const auto emoji = id
				? (Data::FrameSizeFromTag(sizeTag())
					/ style::DevicePixelRatio())
				: 0;
			const auto &style = st();
			const auto icon = (_content.badge == BadgeType::Premium)
				? &style.premium
				: (_content.badge == BadgeType::Verified
					|| _content.badge == BadgeType::BotVerified)
				? &style.verified
				: nullptr;
			const auto iconForeground = (_content.badge == BadgeType::Verified
				|| _content.badge == BadgeType::BotVerified)
				? &style.verifiedCheck
				: nullptr;
			const auto paintAstrogramMini = _useAstrogramMini
				&& IsAstrogramMiniBadgeContent(_peer, _content);
			if (id && !paintAstrogramMini) {
				_emojiStatus = _session->data().customEmojiManager().create(
					Data::EmojiStatusCustomId(id),
					[raw = _view.data()] { raw->update(); },
					sizeTag());
				if (_content.badge == BadgeType::BotVerified) {
					_emojiStatus = std::make_unique<Ui::Text::FirstFrameEmoji>(
						std::move(_emojiStatus));
				} else if (_customStatusLoopsLimit > 0) {
					_emojiStatus = std::make_unique<Ui::Text::LimitedLoopsEmoji>(
						std::move(_emojiStatus),
						_customStatusLoopsLimit);
				}
			}
			const auto width = std::max(emoji, icon ? icon->width() : 0);
			const auto height = std::max(emoji, icon ? icon->height() : 0);
			const auto miniSize = paintAstrogramMini
				? AstrogramMiniBadgeSize(QSize(width, height))
				: QSize(width, height);
			_view->resize(miniSize.width(), miniSize.height());
			_view->paintRequest(
			) | rpl::on_next([=, check = _view.data()] {
				if (paintAstrogramMini) {
					Painter p(check);
					const auto size = AstrogramMiniBadgeSize(check->size());
					const auto color = style.premiumFg->c;
					p.drawImage(
						QRect(
							(check->width() - size.width()) / 2,
							(check->height() - size.height()) / 2,
							size.width(),
							size.height()),
						AstrogramMiniBadgeImage(color));
					return;
				}
				const auto emojiReady = _emojiStatus && _emojiStatus->ready();
				if (emojiReady) {
					auto args = Ui::Text::CustomEmoji::Context{
						.textColor = style.premiumFg->c,
						.now = crl::now(),
						.paused = ((_animationPaused && _animationPaused())
							|| On(PowerSaving::kEmojiStatus)),
					};
					if (!_emojiStatusPanel
						|| !_emojiStatusPanel->paintBadgeFrame(check)) {
						Painter p(check);
						_emojiStatus->paint(p, args);
					}
				}
				if (icon && !emojiReady) {
					auto p = Painter(check);
					const auto left = std::max(
						(check->width() - icon->width()) / 2,
						0);
					const auto top = std::max(
						(check->height() - icon->height()) / 2,
						0);
					if (_overrideSt && !iconForeground) {
						icon->paint(
							p,
							left,
							top,
							check->width(),
							_overrideSt->premiumFg->c);
					} else {
						icon->paint(p, left, top, check->width());
					}
					if (iconForeground) {
						if (_overrideSt) {
							iconForeground->paint(
								p,
								left,
								top,
								check->width(),
								_overrideSt->premiumFg->c);
						} else {
							iconForeground->paint(p, left, top, check->width());
						}
					}
				}
			}, _view->lifetime());
	} break;
	case BadgeType::Scam:
	case BadgeType::Fake:
	case BadgeType::Direct: {
		const auto type = (_content.badge == BadgeType::Direct)
			? Ui::TextBadgeType::Direct
			: (_content.badge == BadgeType::Fake)
			? Ui::TextBadgeType::Fake
			: Ui::TextBadgeType::Scam;
		const auto size = Ui::TextBadgeSize(type);
		const auto skip = st::infoVerifiedCheckPosition.x();
		_view->resize(
			size.width() + 2 * skip,
			size.height() + 2 * skip);
		_view->paintRequest(
		) | rpl::on_next([=, badge = _view.data()]{
			Painter p(badge);
			Ui::DrawTextBadge(
				type,
				p,
				badge->rect().marginsRemoved({ skip, skip, skip, skip }),
				badge->width(),
				(type == Ui::TextBadgeType::Direct
					? st::windowSubTextFg
					: st::attentionButtonFg));
			}, _view->lifetime());
	} break;
	}

	if (!HasPremiumClick(_content) || !_premiumClickCallback) {
		_view->setAttribute(Qt::WA_TransparentForMouseEvents);
	} else {
		_view->setClickedCallback(_premiumClickCallback);
	}

	_updated.fire({});
}

void Badge::setPremiumClickCallback(Fn<void()> callback) {
	_premiumClickCallback = std::move(callback);
	if (_view && HasPremiumClick(_content)) {
		if (!_premiumClickCallback) {
			_view->setAttribute(Qt::WA_TransparentForMouseEvents);
		} else {
			_view->setAttribute(Qt::WA_TransparentForMouseEvents, false);
			_view->setClickedCallback(_premiumClickCallback);
		}
	}
}

void Badge::setOverrideStyle(const style::InfoPeerBadge *st) {
	const auto was = _content;
	_overrideSt = st;
	_content = {};
	setContent(was);
}

rpl::producer<> Badge::updated() const {
	return _updated.events();
}

void Badge::move(int left, int top, int bottom) {
	if (!_view) {
		return;
	}
	const auto &style = st();
	const auto customReady = _emojiStatus && _emojiStatus->ready();
	const auto star = !customReady
		&& (_content.badge == BadgeType::Premium
			|| _content.badge == BadgeType::Verified
			|| _content.badge == BadgeType::BotVerified);
	const auto fake = !customReady && !star;
	const auto skip = fake ? 0 : style.position.x();
	const auto badgeLeft = left + skip;
	const auto badgeTop = top
		+ (star
			? style.position.y()
			: (bottom - top - _view->height()) / 2);
	_view->moveToLeft(badgeLeft, badgeTop);
}

const style::InfoPeerBadge &Badge::st() const {
	return _overrideSt ? *_overrideSt : _st;
}

Data::CustomEmojiSizeTag Badge::sizeTag() const {
	using SizeTag = Data::CustomEmojiSizeTag;
	const auto &style = st();
	return (style.sizeTag == 2)
		? SizeTag::Isolated
		: (style.sizeTag == 1)
		? SizeTag::Large
		: SizeTag::Normal;
}

rpl::producer<Badge::Content> BadgeContentForPeer(not_null<PeerData*> peer) {
	const auto statusOnlyForPremium = peer->isUser();
	auto registryBadgeProducer = [&] {
		auto &registry = Core::AstrogramChannelRegistry::Registry::Instance();
		return rpl::merge(
			rpl::single(rpl::empty_value()),
			registry.updates(&peer->session()) | rpl::map([] {
				return rpl::empty_value();
			})
		) | rpl::map([=](rpl::empty_value) {
			return registry.badgeLookup(peer).badge;
		});
	}();
	return rpl::combine(
		BadgeValue(peer),
		EmojiStatusIdValue(peer),
		std::move(registryBadgeProducer)
	) | rpl::map([=](
			BadgeType badge,
			EmojiStatusId emojiStatusId,
			std::optional<EmojiStatusId> registryBadgeStatus) {
		auto astrogramMini = false;
		if (registryBadgeStatus.has_value()) {
			// Telegram channel registry is the only badge source of truth.
			astrogramMini = true;
			if (registryBadgeStatus->documentId) {
				badge = BadgeType::Premium;
				emojiStatusId = *registryBadgeStatus;
			} else {
				badge = BadgeType::Verified;
				emojiStatusId = EmojiStatusId();
			}
		} else {
			if (emojiStatusId.collectible) {
				return Badge::Content{
					.badge = BadgeType::Premium,
					.emojiStatusId = emojiStatusId,
				};
			}
			if (badge == BadgeType::Verified) {
				badge = BadgeType::None;
			}
			if (emojiStatusId && badge == BadgeType::None) {
				badge = BadgeType::Premium;
			}
		}
		if (statusOnlyForPremium && badge != BadgeType::Premium) {
			emojiStatusId = EmojiStatusId();
		}
		return Badge::Content{
			.badge = badge,
			.emojiStatusId = emojiStatusId,
			.astrogramMini = astrogramMini,
		};
	}) | rpl::distinct_until_changed() | rpl::map([=](Badge::Content content) {
		Logs::writeClient(QString::fromLatin1(
			"[badge] content resolved: peer=%1 badge=%2 emojiStatusId=%3 collectible=%4 astrogramMini=%5")
			.arg(QString::number(qulonglong(peer->id.value)))
			.arg(BadgeTypeForLog(content.badge))
			.arg(QString::number(content.emojiStatusId.documentId))
			.arg(content.emojiStatusId.collectible ? u"true"_q : u"false"_q)
			.arg(content.astrogramMini ? u"true"_q : u"false"_q));
		return content;
	});
}

rpl::producer<Badge::Content> VerifiedContentForPeer(
		not_null<PeerData*> peer) {
	auto registryBadgeProducer = [&] {
		auto &registry = Core::AstrogramChannelRegistry::Registry::Instance();
		return rpl::merge(
			rpl::single(rpl::empty_value()),
			registry.updates(&peer->session()) | rpl::map([] {
				return rpl::empty_value();
			})
		) | rpl::map([=](rpl::empty_value) {
			return registry.badgeLookup(peer).badge;
		});
	}();
	return rpl::combine(
		BadgeValue(peer),
		std::move(registryBadgeProducer),
		[=](
				BadgeType badge,
				std::optional<EmojiStatusId> registryBadgeStatus) {
			if ((badge != BadgeType::Verified)
				|| registryBadgeStatus.has_value()) {
				badge = BadgeType::None;
			}
			return Badge::Content{ .badge = badge };
		}
	) | rpl::distinct_until_changed();
}

rpl::producer<Badge::Content> BotVerifyBadgeForPeer(
		not_null<PeerData*> peer) {
	return peer->session().changes().peerFlagsValue(
		peer,
		Data::PeerUpdate::Flag::VerifyInfo
	) | rpl::map([=] {
		const auto info = peer->botVerifyDetails();
		return Badge::Content{
			.badge = info ? BadgeType::BotVerified : BadgeType::None,
			.emojiStatusId = { info ? info->iconId : DocumentId() },
		};
	});
}

} // namespace Info::Profile
