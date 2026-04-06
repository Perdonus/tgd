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
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/profile/info_profile_values.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "lang/lang_keys.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "main/main_session.h"
#include "styles/style_info.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <map>

namespace Info::Profile {
namespace {

[[nodiscard]] bool HasPremiumClick(const Badge::Content &content) {
	return content.badge == BadgeType::Premium
		|| (content.badge == BadgeType::Verified && content.emojiStatusId);
}

class ServerSubscriberBadge final {
public:
	[[nodiscard]] static ServerSubscriberBadge &Instance() {
		static ServerSubscriberBadge instance;
		return instance;
	}

	[[nodiscard]] rpl::producer<std::optional<EmojiStatusId>> badgeValue(
			not_null<UserData*> user) {
		const auto key = uint64(user->id.value);
		auto i = _entries.find(key);
		if (i == end(_entries)) {
			_entries.emplace(key, std::make_unique<Entry>());
			i = _entries.find(key);
		}
		Expects(i != end(_entries));
		const auto entry = i->second.get();
		requestIfNeeded(key, entry);
		return rpl::single(entry->emojiStatusId) | rpl::then(
			entry->updated.events() | rpl::map([=] {
				return entry->emojiStatusId;
			}));
	}

private:
	struct Entry {
		bool inFlight = false;
		crl::time nextRequestAt = 0;
		std::optional<EmojiStatusId> emojiStatusId;
		rpl::event_stream<> updated;
	};

	[[nodiscard]] static QUrl BuildRequestUrl(uint64 userId) {
		// Server-side subscriber badge endpoint.
		// Expected JSON:
		// { "badge": true, "emoji_status_id": 1234567890 }
		// or { "subscriber": true, "emoji_status_id": 1234567890 }
		auto url = QUrl(u"https://sosiskibot.ru/api/astrogram/subscriber-badge"_q);
		auto query = QUrlQuery();
		query.addQueryItem(u"user_id"_q, QString::number(userId));
		url.setQuery(query);
		return url;
	}

	void requestIfNeeded(uint64 userId, Entry *entry) {
		Expects(entry != nullptr);
		if (entry->inFlight || entry->nextRequestAt > crl::now()) {
			return;
		}
		entry->inFlight = true;
		entry->nextRequestAt = crl::now() + 5 * 60 * 1000; // 5 min TTL.

		QNetworkRequest request(BuildRequestUrl(userId));
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		auto *reply = _manager.get(request);
		QObject::connect(
			reply,
			&QNetworkReply::finished,
			reply,
			[=] {
				auto i = _entries.find(userId);
				if (i == end(_entries)) {
					reply->deleteLater();
					return;
				}
				auto &stored = i->second;
				stored->inFlight = false;

				auto next = std::optional<EmojiStatusId>();
				if (reply->error() == QNetworkReply::NoError) {
					const auto parsed = QJsonDocument::fromJson(reply->readAll());
					if (parsed.isObject()) {
						const auto object = parsed.object();
						const auto enabled = object.value(u"badge"_q).toBool()
							|| object.value(u"subscriber"_q).toBool()
							|| object.value(u"enabled"_q).toBool();
						if (enabled) {
							EmojiStatusId id;
							id.documentId = object.value(u"emoji_status_id"_q)
								.toVariant()
								.toULongLong();
							next = id;
						}
					}
				}
				if (stored->emojiStatusId != next) {
					stored->emojiStatusId = next;
					stored->updated.fire({});
				}
				reply->deleteLater();
			});
	}

	std::map<uint64, std::unique_ptr<Entry>> _entries;
	QNetworkAccessManager _manager;
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
	const auto user = peer->asUser();
	if (user) {
		return rpl::combine(
			BadgeValue(peer),
			EmojiStatusIdValue(peer),
			ServerSubscriberBadge::Instance().badgeValue(user)
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
			if (statusOnlyForPremium && badge != BadgeType::Premium) {
				emojiStatusId = EmojiStatusId();
			} else if (emojiStatusId && badge == BadgeType::None) {
				badge = BadgeType::Premium;
			}
			if (serverBadgeStatus.has_value()) {
				// Server-side badge source of truth.
				badge = BadgeType::Premium;
				if (serverBadgeStatus->documentId) {
					emojiStatusId = *serverBadgeStatus;
				}
			}
			return Badge::Content{ badge, emojiStatusId };
		});
	}
	return rpl::combine(
		BadgeValue(peer),
		EmojiStatusIdValue(peer)
	) | rpl::map([=](BadgeType badge, EmojiStatusId emojiStatusId) {
		if (emojiStatusId.collectible) {
			return Badge::Content{ BadgeType::Premium, emojiStatusId };
		}
		if (badge == BadgeType::Verified) {
			badge = BadgeType::None;
		}
		if (statusOnlyForPremium && badge != BadgeType::Premium) {
			emojiStatusId = EmojiStatusId();
		} else if (emojiStatusId && badge == BadgeType::None) {
			badge = BadgeType::Premium;
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
