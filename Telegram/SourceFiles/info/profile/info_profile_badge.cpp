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
#include "info/profile/info_profile_values.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "lang/lang_keys.h"
#include "logs.h"
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

[[nodiscard]] bool JsonTruthy(const QJsonValue &value) {
	if (value.isBool()) {
		return value.toBool();
	} else if (value.isDouble()) {
		return (value.toInt() != 0);
	} else if (value.isString()) {
		const auto lowered = value.toString().trimmed().toLower();
		return (lowered == u"1"_q
			|| lowered == u"true"_q
			|| lowered == u"yes"_q
			|| lowered == u"on"_q);
	}
	return false;
}

[[nodiscard]] uint64 JsonToUint64(const QJsonValue &value) {
	if (value.isDouble()) {
		const auto asInt = value.toVariant().toULongLong();
		return asInt;
	} else if (value.isString()) {
		auto ok = false;
		const auto parsed = value.toString().trimmed().toULongLong(&ok);
		return ok ? parsed : 0;
	}
	return 0;
}

constexpr auto kServerBadgeSuccessTtl = crl::time(5 * 60 * 1000);
constexpr auto kServerBadgeRetryTtl = crl::time(30 * 1000);

[[nodiscard]] QString ServerBadgeLogText(QByteArray body) {
	auto result = QString::fromUtf8(body.left(512));
	result.replace(u'\n', u' ');
	result.replace(u'\r', u' ');
	return result;
}

[[nodiscard]] QString PeerTypeForLog(PeerId peerId) {
	return peerId.is<UserId>()
		? u"user"_q
		: peerId.is<ChatId>()
		? u"chat"_q
		: peerId.is<ChannelId>()
		? u"channel"_q
		: u"unknown"_q;
}

[[nodiscard]] uint64 PeerBareId(PeerId peerId) {
	return peerId.is<UserId>()
		? uint64(peerToUser(peerId).bare)
		: peerId.is<ChatId>()
		? uint64(peerToChat(peerId).bare)
		: peerId.is<ChannelId>()
		? uint64(peerToChannel(peerId).bare)
		: 0;
}

[[nodiscard]] uint64 ServerBadgeEmojiStatusId(const QJsonObject &object);

[[nodiscard]] uint64 ServerBadgeEmojiStatusId(const QJsonValue &value) {
	if (value.isObject()) {
		return ServerBadgeEmojiStatusId(value.toObject());
	}
	return JsonToUint64(value);
}

[[nodiscard]] uint64 ServerBadgeEmojiStatusId(const QJsonObject &object) {
	for (const auto &key : {
		u"emoji_status_id"_q,
		u"emojiStatusId"_q,
		u"emoji_status_document_id"_q,
		u"emojiStatusDocumentId"_q,
		u"document_id"_q,
		u"documentId"_q,
		u"custom_emoji_id"_q,
		u"customEmojiId"_q,
		u"emoji_id"_q,
		u"emojiId"_q,
		u"id"_q,
	}) {
		if (const auto value = object.value(key); !value.isUndefined()) {
			if (const auto parsed = ServerBadgeEmojiStatusId(value)) {
				return parsed;
			}
		}
	}
	for (const auto &key : {
		u"emoji_status"_q,
		u"emojiStatus"_q,
		u"badge"_q,
		u"subscriber_badge"_q,
		u"subscriberBadge"_q,
	}) {
		if (const auto value = object.value(key); value.isObject()) {
			if (const auto parsed = ServerBadgeEmojiStatusId(value.toObject())) {
				return parsed;
			}
		}
	}
	return 0;
}

[[nodiscard]] std::optional<std::optional<EmojiStatusId>> ParseServerBadgeObject(
		const QJsonObject &object) {
	for (const auto &key : {
		u"badge"_q,
		u"subscriber_badge"_q,
		u"subscriberBadge"_q,
		u"subscriber"_q,
		u"emoji_status"_q,
		u"emojiStatus"_q,
	}) {
		const auto value = object.value(key);
		if (value.isObject()) {
			if (const auto parsed = ParseServerBadgeObject(value.toObject())) {
				return parsed;
			}
		}
	}
	const auto hasEnabledField = object.contains(u"badge"_q)
		|| object.contains(u"subscriber"_q)
		|| object.contains(u"subscriber_badge"_q)
		|| object.contains(u"subscriberBadge"_q)
		|| object.contains(u"enabled"_q);
	const auto hasStatusIdField = object.contains(u"emoji_status_id"_q)
		|| object.contains(u"emojiStatusId"_q)
		|| object.contains(u"emoji_status_document_id"_q)
		|| object.contains(u"emojiStatusDocumentId"_q)
		|| object.contains(u"document_id"_q)
		|| object.contains(u"documentId"_q)
		|| object.contains(u"custom_emoji_id"_q)
		|| object.contains(u"customEmojiId"_q)
		|| object.contains(u"emoji_id"_q)
		|| object.contains(u"emojiId"_q)
		|| object.contains(u"emoji_status"_q)
		|| object.contains(u"emojiStatus"_q);
	if (!hasEnabledField && !hasStatusIdField) {
		return std::nullopt;
	}
	const auto statusId = ServerBadgeEmojiStatusId(object);
	const auto enabled = hasEnabledField
		? (JsonTruthy(object.value(u"badge"_q))
			|| JsonTruthy(object.value(u"subscriber"_q))
			|| JsonTruthy(object.value(u"subscriber_badge"_q))
			|| JsonTruthy(object.value(u"subscriberBadge"_q))
			|| JsonTruthy(object.value(u"enabled"_q))
			|| (statusId != 0))
		: (statusId != 0);
	if (!enabled) {
		return std::optional<std::optional<EmojiStatusId>>(
			std::optional<EmojiStatusId>());
	}
	auto result = EmojiStatusId();
	result.documentId = statusId;
	return std::optional<std::optional<EmojiStatusId>>(result);
}

[[nodiscard]] std::optional<std::optional<EmojiStatusId>> ParseServerBadgeResponse(
		const QJsonObject &root) {
	if (const auto parsed = ParseServerBadgeObject(root)) {
		return parsed;
	}
	for (const auto &key : {
		u"data"_q,
		u"result"_q,
		u"payload"_q,
		u"response"_q,
		u"badge"_q,
		u"subscriber_badge"_q,
		u"subscriberBadge"_q,
	}) {
		const auto value = root.value(key);
		if (value.isObject()) {
			if (const auto parsed = ParseServerBadgeObject(value.toObject())) {
				return parsed;
			}
		}
	}
	return std::nullopt;
}

class ServerSubscriberBadge final {
public:
	[[nodiscard]] static ServerSubscriberBadge &Instance() {
		static ServerSubscriberBadge instance;
		return instance;
	}

	[[nodiscard]] rpl::producer<std::optional<EmojiStatusId>> badgeValue(
			not_null<PeerData*> peer) {
		const auto key = uint64(peer->id.value);
		auto i = _entries.find(key);
		if (i == end(_entries)) {
			_entries.emplace(key, std::make_unique<Entry>());
			i = _entries.find(key);
		}
		Expects(i != end(_entries));
		const auto entry = i->second.get();
		requestIfNeeded(peer->id, entry);
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

	[[nodiscard]] static QUrl BuildRequestUrl(PeerId peerId) {
		// Server-side subscriber badge endpoint.
		// Expected JSON:
		// { "badge": true, "emoji_status_id": 1234567890 }
		// or { "subscriber": true, "emoji_status_id": 1234567890 }
		auto url = QUrl(u"https://sosiskibot.ru/api/astrogram/subscriber-badge"_q);
		auto query = QUrlQuery();
		query.addQueryItem(
			u"peer_id"_q,
			QString::number(qulonglong(peerId.value)));
		query.addQueryItem(
			u"peer_type"_q,
			PeerTypeForLog(peerId));
		if (const auto bareId = PeerBareId(peerId); bareId != 0) {
			query.addQueryItem(
				u"peer_bare_id"_q,
				QString::number(qulonglong(bareId)));
		}
		if (peerId.is<UserId>()) {
			query.addQueryItem(
				u"user_id"_q,
				QString::number(qulonglong(peerToUser(peerId).bare)));
		} else if (peerId.is<ChatId>()) {
			query.addQueryItem(
				u"chat_id"_q,
				QString::number(qulonglong(peerToChat(peerId).bare)));
		} else if (peerId.is<ChannelId>()) {
			query.addQueryItem(
				u"channel_id"_q,
				QString::number(qulonglong(peerToChannel(peerId).bare)));
		}
		url.setQuery(query);
		return url;
	}

	void requestIfNeeded(PeerId peerId, Entry *entry) {
		Expects(entry != nullptr);
		if (entry->inFlight || entry->nextRequestAt > crl::now()) {
			return;
		}
		entry->inFlight = true;
		const auto peerKey = uint64(peerId.value);
		Logs::writeClient(QString::fromLatin1(
			"[badge] request started: peer=%1 bare=%2 type=%3")
			.arg(QString::number(qulonglong(peerId.value)))
			.arg(QString::number(qulonglong(PeerBareId(peerId))))
			.arg(PeerTypeForLog(peerId)));

		QNetworkRequest request(BuildRequestUrl(peerId));
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		auto *reply = _manager.get(request);
		QObject::connect(
			reply,
			&QNetworkReply::finished,
			reply,
			[=] {
				auto i = _entries.find(peerKey);
				if (i == end(_entries)) {
					reply->deleteLater();
					return;
				}
				auto &stored = i->second;
				stored->inFlight = false;

				auto next = stored->emojiStatusId;
				auto resolved = false;
				auto nextRequestAt = crl::now() + kServerBadgeRetryTtl;
				const auto statusCode = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				const auto body = reply->readAll();
				if (reply->error() == QNetworkReply::NoError) {
					const auto parsed = QJsonDocument::fromJson(body);
					if (parsed.isObject()) {
						if (const auto parsedBadge = ParseServerBadgeResponse(
								parsed.object())) {
							next = *parsedBadge;
							resolved = true;
							nextRequestAt = crl::now() + kServerBadgeSuccessTtl;
						}
					}
					Logs::writeClient(QString::fromLatin1(
						"[badge] request finished: peer=%1 http=%2 resolved=%3 enabled=%4 emojiStatusId=%5")
						.arg(QString::number(qulonglong(peerId.value)))
						.arg(statusCode)
						.arg(resolved ? u"true"_q : u"false"_q)
						.arg(next.has_value() ? u"true"_q : u"false"_q)
						.arg(next ? QString::number(next->documentId) : QStringLiteral("0")));
					if (!resolved) {
						Logs::writeClient(QString::fromLatin1(
							"[badge] response not recognized: peer=%1 body=%2")
							.arg(QString::number(qulonglong(peerId.value)))
							.arg(ServerBadgeLogText(body)));
					}
				} else {
					Logs::writeClient(QString::fromLatin1(
						"[badge] request failed: peer=%1 http=%2 reason=%3")
						.arg(QString::number(qulonglong(peerId.value)))
						.arg(statusCode)
						.arg(reply->errorString()));
				}
				stored->nextRequestAt = nextRequestAt;
				if (resolved && stored->emojiStatusId != next) {
					stored->emojiStatusId = next;
					stored->updated.fire({});
				}
				reply->deleteLater();
			});
	}
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
			const auto width = std::max(emoji, icon ? icon->width() : 0);
			const auto height = std::max(emoji, icon ? icon->height() : 0);
			_view->resize(width, height);
			_view->paintRequest(
			) | rpl::on_next([=, check = _view.data()] {
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
	return rpl::combine(
		BadgeValue(peer),
		EmojiStatusIdValue(peer),
		ServerSubscriberBadge::Instance().badgeValue(peer)
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
			} else {
				emojiStatusId = EmojiStatusId();
			}
		}
		if (statusOnlyForPremium && badge != BadgeType::Premium) {
			emojiStatusId = EmojiStatusId();
		}
		return Badge::Content{ badge, emojiStatusId };
	}) | rpl::distinct_until_changed();
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
