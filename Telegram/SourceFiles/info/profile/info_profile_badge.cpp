/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_badge.h"

#include "apiwrap.h"
#include "base/weak_ptr.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_emoji_statuses.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/profile/info_profile_values.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "main/main_session.h"
#include "styles/style_info.h"

#include <QSet>
#include <type_traits>

namespace Info::Profile {
namespace {

[[nodiscard]] bool HasPremiumClick(const Badge::Content &content) {
	return content.badge == BadgeType::Premium
		|| (content.badge == BadgeType::Verified && content.emojiStatusId);
}

constexpr auto kBadgeChannelBareId = ChannelId(3960998300ULL);
constexpr auto kBadgeHistoryLimit = 100;
constexpr auto kBadgeSuccessTtl = crl::time(5 * 60 * 1000);
constexpr auto kBadgeRetryTtl = crl::time(30 * 1000);

// Parses a peer id posted into the badge channel. Supports all forms:
//  123456789       -> bare id 123456789 (user)
//  -1234567890     -> bare id 1234567890 (chat)
//  -1003960998300  -> bare id 3960998300 (channel)
//  3960998300      -> bare id 3960998300 (channel without -100 prefix)
[[nodiscard]] std::optional<uint64> ParseBadgeBareId(const QString &raw) {
	const auto text = raw.trimmed();
	if (text.isEmpty()) {
		return std::nullopt;
	}
	auto ok = false;
	const auto value = text.toLongLong(&ok);
	if (!ok || !value) {
		return std::nullopt;
	}
	auto positive = (value < 0) ? -value : value;
	constexpr auto kBotApiChannelOffset = 1000000000000LL;
	if (positive >= kBotApiChannelOffset) {
		positive -= kBotApiChannelOffset;
	}
	if (positive <= 0) {
		return std::nullopt;
	}
	return uint64(positive);
}

class ChannelSubscriberBadge final {
public:
	[[nodiscard]] static ChannelSubscriberBadge &Instance() {
		static ChannelSubscriberBadge instance;
		return instance;
	}

	// Returns an optional emoji status: std::nullopt means "no badge",
	// a value (possibly with documentId == 0) means "subscriber badge".
	[[nodiscard]] rpl::producer<std::optional<EmojiStatusId>> badgeValue(
			not_null<PeerData*> peer) {
		const auto session = &peer->session();
		ensureSession(session);
		const auto key = peer->id.value & PeerId::kChatTypeMask;
		const auto current = [=]() -> std::optional<EmojiStatusId> {
			return _badgedBareIds.contains(key)
				? std::optional<EmojiStatusId>(EmojiStatusId())
				: std::nullopt;
		};
		return rpl::single(current()) | rpl::then(
			_updated.events() | rpl::map(current));
	}

private:
	void ensureSession(not_null<Main::Session*> session) {
		if (_session.get() == session) {
			if (!_fetching && _nextRequestAt <= crl::now()) {
				refresh(session);
			}
			return;
		}
		_session = base::make_weak(session);
		_badgedBareIds.clear();
		_fetching = false;
		_nextRequestAt = 0;
		refresh(session);
	}

	void refresh(not_null<Main::Session*> session) {
		if (_fetching) {
			return;
		}
		_fetching = true;
		if (const auto loaded = session->data().channelLoaded(
				kBadgeChannelBareId)) {
			fetchHistory(session, loaded);
			return;
		}
		session->api().request(MTPchannels_GetChannels(
			MTP_vector<MTPInputChannel>(
				1,
				MTP_inputChannel(
					MTP_long(kBadgeChannelBareId.bare),
					MTP_long(0)))
		)).done([=](const MTPmessages_Chats &result) {
			const auto session = _session.get();
			if (!session) {
				_fetching = false;
				return;
			}
			auto channel = (ChannelData*)nullptr;
			result.match([&](const auto &data) {
				const auto peer = session->data().processChats(data.vchats());
				if (peer
					&& peer->id == peerFromChannel(kBadgeChannelBareId)) {
					channel = peer->asChannel();
				}
			});
			if (channel) {
				fetchHistory(session, channel);
			} else {
				_fetching = false;
				_nextRequestAt = crl::now() + kBadgeRetryTtl;
			}
		}).fail([=] {
			_fetching = false;
			_nextRequestAt = crl::now() + kBadgeRetryTtl;
		}).send();
	}

	void fetchHistory(
			not_null<Main::Session*> session,
			not_null<ChannelData*> channel) {
		session->api().request(MTPmessages_GetHistory(
			channel->input(),
			MTP_int(0), // offset_id
			MTP_int(0), // offset_date
			MTP_int(0), // add_offset
			MTP_int(kBadgeHistoryLimit), // limit
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_long(0) // hash
		)).done([=](const MTPmessages_Messages &result) {
			_fetching = false;
			_nextRequestAt = crl::now() + kBadgeSuccessTtl;
			applyMessages(session, result);
		}).fail([=] {
			_fetching = false;
			_nextRequestAt = crl::now() + kBadgeRetryTtl;
		}).send();
	}

	void applyMessages(
			not_null<Main::Session*> session,
			const MTPmessages_Messages &result) {
		auto badged = QSet<uint64>();
		result.match(
			[&](const MTPDmessages_messages &data) {
				collectBadgedIds(data.vmessages().v, badged);
			},
			[&](const MTPDmessages_messagesSlice &data) {
				collectBadgedIds(data.vmessages().v, badged);
			},
			[&](const MTPDmessages_channelMessages &data) {
				collectBadgedIds(data.vmessages().v, badged);
			},
			[](const MTPDmessages_messagesNotModified &) {});
		if (badged != _badgedBareIds) {
			_badgedBareIds = std::move(badged);
			_updated.fire({});
		}
		Logs::writeClient(QString::fromLatin1(
			"[badge] refreshed: %1 ids from channel %2")
			.arg(_badgedBareIds.size())
			.arg(kBadgeChannelBareId.bare));
	}

	void collectBadgedIds(
			const auto &messages,
			QSet<uint64> &badged) {
		for (const auto &message : messages) {
			const auto bareId = message.match(
				[](const MTPDmessage &data) -> std::optional<uint64> {
					return ParseBadgeBareId(data.vmessage().v);
				},
				[](const auto &) -> std::optional<uint64> {
					return std::nullopt;
				});
			if (bareId) {
				badged.insert(*bareId);
			}
		}
	}

	base::weak_ptr<Main::Session> _session;
	bool _fetching = false;
	crl::time _nextRequestAt = 0;
	QSet<uint64> _badgedBareIds;
	rpl::event_stream<> _updated;
};

} // namespace

Badge::Badge(
	not_null<QWidget*> parent,
	const style::InfoPeerBadge &st,
	not_null<Main::Session*> session,
	rpl::producer<Content> content,
	EmojiStatusPanel *emojiStatusPanel,
	Fn<bool()> animationPaused,
	int customStatusLoopsLimit,
	base::flags<BadgeType> allowed)
: _parent(parent)
, _st(st)
, _session(session)
, _emojiStatusPanel(emojiStatusPanel)
, _customStatusLoopsLimit(customStatusLoopsLimit)
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
	if (!(_allowed & content.badge)
		|| (!_session->premiumBadgesShown()
			&& content.badge == BadgeType::Premium)) {
		content.badge = BadgeType::None;
	}
	if (!(_allowed & content.badge)) {
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
		const auto icon = (_content.badge == BadgeType::Verified)
			? &style.verified
			: id
			? nullptr
			: &style.premium;
		const auto iconForeground = (_content.badge == BadgeType::Verified)
			? &style.verifiedCheck
			: nullptr;
		if (id) {
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
		const auto width = emoji + (icon ? icon->width() : 0);
		const auto height = std::max(emoji, icon ? icon->height() : 0);
		_view->resize(width, height);
		_view->paintRequest(
		) | rpl::on_next([=, check = _view.data()]{
			if (_emojiStatus) {
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
			if (icon) {
				auto p = Painter(check);
				if (_overrideSt && !iconForeground) {
					icon->paint(
						p,
						emoji,
						0,
						check->width(),
						_overrideSt->premiumFg->c);
				} else {
					icon->paint(p, emoji, 0, check->width());
				}
				if (iconForeground) {
					if (_overrideSt) {
						iconForeground->paint(
							p,
							emoji,
							0,
							check->width(),
							_overrideSt->premiumFg->c);
					} else {
						iconForeground->paint(p, emoji, 0, check->width());
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
	const auto star = !_emojiStatus
		&& (_content.badge == BadgeType::Premium
			|| _content.badge == BadgeType::Verified);
	const auto fake = !_emojiStatus && !star;
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
	return rpl::combine(
		BadgeValue(peer),
		EmojiStatusIdValue(peer),
		ChannelSubscriberBadge::Instance().badgeValue(peer)
	) | rpl::map([=](
			BadgeType badge,
			EmojiStatusId emojiStatusId,
			std::optional<EmojiStatusId> serverBadgeStatus) {
		if (emojiStatusId.collectible) {
			return Badge::Content{ BadgeType::Premium, emojiStatusId };
		}
		if (badge == BadgeType::Verified) {
			badge = BadgeType::None;
		}
		if (emojiStatusId && badge == BadgeType::None) {
			badge = BadgeType::Premium;
		}
		if (serverBadgeStatus.has_value()) {
			// Server-side badge source of truth.
			badge = BadgeType::Premium;
			if (serverBadgeStatus->documentId) {
				emojiStatusId = *serverBadgeStatus;
			}
		}
		if (statusOnlyForPremium && badge != BadgeType::Premium) {
			emojiStatusId = EmojiStatusId();
		}
		return Badge::Content{ badge, emojiStatusId };
	});
}

rpl::producer<Badge::Content> VerifiedContentForPeer(
		not_null<PeerData*> peer) {
	return BadgeValue(peer) | rpl::map([=](BadgeType badge) {
		if (badge != BadgeType::Verified) {
			badge = BadgeType::None;
		}
		return Badge::Content{ badge };
	});
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
