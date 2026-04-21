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
#include "base/timer.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "main/main_session.h"
#include "styles/style_info.h"

#include <algorithm>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QJsonArray>
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
			|| lowered == u"on"_q
			|| lowered == u"enabled"_q
			|| lowered == u"active"_q
			|| lowered == u"premium"_q
			|| lowered == u"subscriber"_q);
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
constexpr auto kServerBadgeRetryMaxTtl = crl::time(5 * 60 * 1000);
constexpr auto kServerBadgeChangesRetryTtl = crl::time(5 * 1000);
constexpr auto kServerBadgeChangesLongPollSeconds = 25;
constexpr auto kAstrogramServerBaseUrlFallback = "https://astrogram.su/api/astrogram";

[[nodiscard]] crl::time ServerBadgeRetryDelay(int retryCount) {
	auto delay = kServerBadgeRetryTtl;
	for (auto i = 1; (i < retryCount) && (delay < kServerBadgeRetryMaxTtl); ++i) {
		delay = std::min(delay * 2, kServerBadgeRetryMaxTtl);
	}
	return delay;
}

[[nodiscard]] QString ServerBadgeLogText(QByteArray body) {
	auto result = QString::fromUtf8(body.left(512));
	result.replace(u'\n', u' ');
	result.replace(u'\r', u' ');
	return result;
}

[[nodiscard]] QString NormalizeAstrogramServerBaseUrl(QString value) {
	value = value.trimmed();
	while (value.endsWith(u'/')) {
		value.chop(1);
	}
	return value;
}

[[nodiscard]] QString AstrogramServerBaseUrl(not_null<Main::Session*> session) {
	for (const auto &key : {
		u"astrogram_server_base_url"_q,
		u"astrogram_badge_server_base_url"_q,
		u"astrogram_server_url"_q,
	}) {
		const auto value = NormalizeAstrogramServerBaseUrl(
			session->appConfig().get<QString>(key, QString()));
		if (!value.isEmpty()) {
			return value;
		}
	}
	return QString::fromLatin1(kAstrogramServerBaseUrlFallback);
}

[[nodiscard]] QString AstrogramServerApiBaseUrl(not_null<Main::Session*> session) {
	auto base = AstrogramServerBaseUrl(session);
	if (!base.endsWith(u"/v1"_q)) {
		base += u"/v1"_q;
	}
	return base;
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

[[nodiscard]] QString PeerRefForServer(PeerId peerId) {
	const auto bareId = PeerBareId(peerId);
	return (bareId != 0)
		? (PeerTypeForLog(peerId) + u':' + QString::number(qulonglong(bareId)))
		: QString();
}

[[nodiscard]] QString JsonStringOrEmpty(const QJsonValue &value) {
	return value.isString() ? value.toString().trimmed() : QString();
}

[[nodiscard]] QString ServerPeerTypeFromChange(const QJsonObject &object) {
	for (const auto &key : {
		u"peer_type"_q,
		u"peerType"_q,
		u"type"_q,
	}) {
		if (const auto value = JsonStringOrEmpty(object.value(key)); !value.isEmpty()) {
			return value.toLower();
		}
	}
	if (const auto peer = object.value(u"peer"_q); peer.isObject()) {
		if (const auto value = JsonStringOrEmpty(
				peer.toObject().value(u"type"_q)); !value.isEmpty()) {
			return value.toLower();
		}
	}
	if (const auto value = object.value(u"value"_q); value.isObject()) {
		return ServerPeerTypeFromChange(value.toObject());
	}
	return QString();
}

[[nodiscard]] uint64 NumericPeerIdFromServerString(
		QString value,
		QString peerType) {
	value = value.trimmed();
	peerType = peerType.trimmed().toLower();
	if (value.isEmpty()) {
		return 0;
	}
	const auto parseUnsigned = [&](QString raw) -> uint64 {
		raw = raw.trimmed();
		auto ok = false;
		const auto parsed = raw.toULongLong(&ok);
		return ok ? parsed : 0;
	};
	const auto separator = value.indexOf(u':');
	if (separator > 0) {
		peerType = value.mid(0, separator).trimmed().toLower();
		value = value.mid(separator + 1).trimmed();
	}
	if ((peerType == u"channel"_q) || value.startsWith(u"-100"_q)) {
		if (value.startsWith(u"-100"_q)) {
			value.remove(0, 4);
		}
		if (const auto parsed = parseUnsigned(value)) {
			return uint64(peerFromChannel(ChannelId(parsed)).value);
		}
		return 0;
	} else if (peerType == u"user"_q) {
		if (const auto parsed = parseUnsigned(value)) {
			return uint64(peerFromUser(UserId(parsed)).value);
		}
		return 0;
	} else if (peerType == u"chat"_q) {
		if (const auto parsed = parseUnsigned(value)) {
			return uint64(peerFromChat(ChatId(parsed)).value);
		}
		return 0;
	}
	return parseUnsigned(value);
}

[[nodiscard]] uint64 PeerIdFromServerChange(const QJsonObject &object) {
	const auto peerType = ServerPeerTypeFromChange(object);
	for (const auto &key : {
		u"peer_id"_q,
		u"peerId"_q,
		u"id"_q,
	}) {
		const auto value = object.value(key);
		if (value.isString()) {
			if (const auto parsed = NumericPeerIdFromServerString(
					value.toString(),
					peerType); parsed != 0) {
				return parsed;
			}
		} else if (const auto parsed = JsonToUint64(value); parsed != 0) {
			return parsed;
		}
	}
	if (const auto value = object.value(u"value"_q); value.isObject()) {
		return PeerIdFromServerChange(value.toObject());
	}
	return 0;
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
		u"status_emoji_id"_q,
		u"statusEmojiId"_q,
		u"status_document_id"_q,
		u"statusDocumentId"_q,
		u"premium_emoji_status_id"_q,
		u"premiumEmojiStatusId"_q,
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
		u"premium_emoji_status"_q,
		u"premiumEmojiStatus"_q,
		u"custom_emoji"_q,
		u"customEmoji"_q,
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
		u"enabled"_q,
		u"active"_q,
		u"is_subscriber"_q,
		u"isSubscriber"_q,
		u"has_badge"_q,
		u"hasBadge"_q,
		u"verified"_q,
		u"trusted"_q,
		u"confirmed"_q,
		u"premium"_q,
		u"emoji_status"_q,
		u"emojiStatus"_q,
		u"premium_emoji_status"_q,
		u"premiumEmojiStatus"_q,
		u"custom_emoji"_q,
		u"customEmoji"_q,
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
		|| object.contains(u"enabled"_q)
		|| object.contains(u"active"_q)
		|| object.contains(u"is_subscriber"_q)
		|| object.contains(u"isSubscriber"_q)
		|| object.contains(u"has_badge"_q)
		|| object.contains(u"hasBadge"_q)
		|| object.contains(u"verified"_q)
		|| object.contains(u"trusted"_q)
		|| object.contains(u"confirmed"_q)
		|| object.contains(u"premium"_q);
	const auto hasStatusIdField = object.contains(u"emoji_status_id"_q)
		|| object.contains(u"emojiStatusId"_q)
		|| object.contains(u"emoji_status_document_id"_q)
		|| object.contains(u"emojiStatusDocumentId"_q)
		|| object.contains(u"status_emoji_id"_q)
		|| object.contains(u"statusEmojiId"_q)
		|| object.contains(u"status_document_id"_q)
		|| object.contains(u"statusDocumentId"_q)
		|| object.contains(u"premium_emoji_status_id"_q)
		|| object.contains(u"premiumEmojiStatusId"_q)
		|| object.contains(u"document_id"_q)
		|| object.contains(u"documentId"_q)
		|| object.contains(u"custom_emoji_id"_q)
		|| object.contains(u"customEmojiId"_q)
		|| object.contains(u"emoji_id"_q)
		|| object.contains(u"emojiId"_q)
		|| object.contains(u"emoji_status"_q)
		|| object.contains(u"emojiStatus"_q)
		|| object.contains(u"premium_emoji_status"_q)
		|| object.contains(u"premiumEmojiStatus"_q)
		|| object.contains(u"custom_emoji"_q)
		|| object.contains(u"customEmoji"_q);
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
			|| JsonTruthy(object.value(u"active"_q))
			|| JsonTruthy(object.value(u"is_subscriber"_q))
			|| JsonTruthy(object.value(u"isSubscriber"_q))
			|| JsonTruthy(object.value(u"has_badge"_q))
			|| JsonTruthy(object.value(u"hasBadge"_q))
			|| JsonTruthy(object.value(u"verified"_q))
			|| JsonTruthy(object.value(u"trusted"_q))
			|| JsonTruthy(object.value(u"confirmed"_q))
			|| JsonTruthy(object.value(u"premium"_q))
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
	if (root.contains(u"found"_q) && !JsonTruthy(root.value(u"found"_q))) {
		return std::optional<std::optional<EmojiStatusId>>(
			std::optional<EmojiStatusId>());
	}
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
		entry->apiBaseUrl = AstrogramServerApiBaseUrl(&peer->session());
		if (_changesApiBaseUrl.isEmpty()) {
			_changesApiBaseUrl = entry->apiBaseUrl;
		}
		requestIfNeeded(peer->id, entry);
		requestChangesFeedIfNeeded();
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
		std::unique_ptr<base::Timer> retryTimer;
		int retryCount = 0;
		QString apiBaseUrl;
	};

	[[nodiscard]] static QString PeerPathForServer(PeerId peerId) {
		if (const auto bareId = PeerBareId(peerId); bareId != 0) {
			if (peerId.is<UserId>()) {
				return u"/users/"_q + QString::number(qulonglong(bareId)) + u"/badge"_q;
			} else if (peerId.is<ChannelId>()) {
				return u"/channels/-100"_q + QString::number(qulonglong(bareId)) + u"/badge"_q;
			} else if (peerId.is<ChatId>()) {
				return u"/peers/chat:"_q + QString::number(qulonglong(bareId)) + u"/badge"_q;
			}
		}
		return u"/peers/"_q + QString::number(qulonglong(peerId.value)) + u"/badge"_q;
	}

	[[nodiscard]] static QUrl BuildRequestUrl(
			const QString &apiBaseUrl,
			PeerId peerId) {
		auto url = QUrl(apiBaseUrl + PeerPathForServer(peerId));
		auto query = QUrlQuery();
		if (const auto peerRef = PeerRefForServer(peerId); !peerRef.isEmpty()) {
			query.addQueryItem(u"peer_ref"_q, peerRef);
		}
		query.addQueryItem(
			u"peer_type"_q,
			PeerTypeForLog(peerId));
		if (const auto bareId = PeerBareId(peerId); bareId != 0) {
			query.addQueryItem(
				u"bare_id"_q,
				QString::number(qulonglong(bareId)));
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

	[[nodiscard]] static QUrl BuildChangesUrl(
			const QString &apiBaseUrl,
			int sinceRevision) {
		auto url = QUrl(apiBaseUrl + u"/changes"_q);
		auto query = QUrlQuery();
		query.addQueryItem(
			u"since_revision"_q,
			QString::number(std::max(sinceRevision, 0)));
		query.addQueryItem(
			u"timeout"_q,
			QString::number(kServerBadgeChangesLongPollSeconds));
		url.setQuery(query);
		return url;
	}

	void scheduleChangesFeedRetry(const QString &reason, crl::time delay) {
		if (_entries.empty()) {
			return;
		}
		if (!_changesRetryTimer) {
			_changesRetryTimer = std::make_unique<base::Timer>([=] {
				requestChangesFeedIfNeeded();
			});
		}
		_changesRetryTimer->callOnce(std::max(delay, crl::time(0)));
		Logs::writeClient(QString::fromLatin1(
			"[badge] changes feed scheduled: delay_ms=%1 reason=%2 tracked=%3 revision=%4")
			.arg(QString::number(delay))
			.arg(reason)
			.arg(QString::number(_entries.size()))
			.arg(QString::number(_changesRevision)));
	}

	void requestChangesFeedIfNeeded() {
		if (_changesInFlight || _entries.empty()) {
			return;
		}
		if (_changesRetryTimer) {
			_changesRetryTimer->cancel();
		}
		_changesInFlight = true;

		if (_changesApiBaseUrl.isEmpty()) {
			_changesApiBaseUrl = QString::fromLatin1(kAstrogramServerBaseUrlFallback)
				+ u"/v1"_q;
		}
		const auto url = BuildChangesUrl(_changesApiBaseUrl, _changesRevision);
		QNetworkRequest request(url);
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		request.setRawHeader("Accept", "application/json");
		request.setRawHeader("User-Agent", "AstrogramDesktop");
		request.setRawHeader("Cache-Control", "no-cache");

		Logs::writeClient(QString::fromLatin1(
			"[badge] changes feed started: revision=%1 tracked=%2 url=%3")
			.arg(QString::number(_changesRevision))
			.arg(QString::number(_entries.size()))
			.arg(url.toString(QUrl::FullyEncoded)));

		auto *reply = _manager.get(request);
		QObject::connect(
			reply,
			&QNetworkReply::finished,
			reply,
			[=] {
				_changesInFlight = false;

				auto retryDelay = crl::time(0);
				auto retryReason = u"success-refresh"_q;
				auto success = false;
				const auto statusCode = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				const auto body = reply->readAll();

				if (reply->error() == QNetworkReply::NoError) {
					const auto parsed = QJsonDocument::fromJson(body);
					if (parsed.isObject()) {
						const auto root = parsed.object();
						_changesRevision = std::max(
							_changesRevision,
							root.value(u"revision"_q).toInt(_changesRevision));
						const auto peerChanges = root.value(u"changes"_q)
							.toObject()
							.value(u"peer_badges"_q)
							.toArray();
						for (const auto &change : peerChanges) {
							if (!change.isObject()) {
								continue;
							}
							const auto peerKey = PeerIdFromServerChange(change.toObject());
							if (!peerKey) {
								continue;
							}
							const auto it = _entries.find(peerKey);
							if (it == end(_entries)) {
								continue;
							}
							it->second->nextRequestAt = 0;
							it->second->retryCount = 0;
							requestIfNeeded(PeerId(PeerIdHelper{ peerKey }), it->second.get());
						}
						success = true;
						Logs::writeClient(QString::fromLatin1(
							"[badge] changes feed finished: http=%1 revision=%2 changed=%3 peerChanges=%4")
							.arg(statusCode)
							.arg(QString::number(_changesRevision))
							.arg(root.value(u"changed"_q).toBool() ? u"true"_q : u"false"_q)
							.arg(QString::number(peerChanges.size())));
					}
				}
				if (!success) {
					retryDelay = kServerBadgeChangesRetryTtl;
					retryReason = reply->error() == QNetworkReply::NoError
						? u"invalid-response"_q
						: reply->errorString();
					Logs::writeClient(QString::fromLatin1(
						"[badge] changes feed failed: http=%1 reason=%2 body=%3")
						.arg(QString::number(statusCode))
						.arg(retryReason)
						.arg(ServerBadgeLogText(body)));
				}
				scheduleChangesFeedRetry(retryReason, retryDelay);
				reply->deleteLater();
			});
	}

	void scheduleRetry(PeerId peerId, Entry *entry, const QString &reason) {
		Expects(entry != nullptr);
		const auto delay = std::max(entry->nextRequestAt - crl::now(), crl::time(0));
		if (!entry->retryTimer) {
			entry->retryTimer = std::make_unique<base::Timer>([=] {
				const auto i = _entries.find(uint64(peerId.value));
				if (i == end(_entries)) {
					return;
				}
				requestIfNeeded(peerId, i->second.get());
			});
		}
		entry->retryTimer->callOnce(delay);
		Logs::writeClient(QString::fromLatin1(
			"[badge] retry scheduled: peer=%1 delay_ms=%2 attempt=%3 reason=%4")
			.arg(QString::number(qulonglong(peerId.value)))
			.arg(QString::number(delay))
			.arg(QString::number(entry->retryCount))
			.arg(reason));
	}

	void requestIfNeeded(PeerId peerId, Entry *entry) {
		Expects(entry != nullptr);
		if (entry->inFlight) {
			return;
		}
		if (entry->nextRequestAt > crl::now()) {
			scheduleRetry(peerId, entry, u"backoff_window"_q);
			return;
		}
		if (entry->retryTimer) {
			entry->retryTimer->cancel();
		}
		entry->inFlight = true;
		const auto peerKey = uint64(peerId.value);
		if (entry->apiBaseUrl.isEmpty()) {
			entry->apiBaseUrl = QString::fromLatin1(kAstrogramServerBaseUrlFallback)
				+ u"/v1"_q;
		}
		if (_changesApiBaseUrl.isEmpty()) {
			_changesApiBaseUrl = entry->apiBaseUrl;
		}
		const auto url = BuildRequestUrl(entry->apiBaseUrl, peerId);
		Logs::writeClient(QString::fromLatin1(
			"[badge] request started: peer=%1 bare=%2 type=%3 ref=%4 url=%5")
			.arg(QString::number(qulonglong(peerId.value)))
			.arg(QString::number(qulonglong(PeerBareId(peerId))))
			.arg(PeerTypeForLog(peerId))
			.arg(PeerRefForServer(peerId))
			.arg(url.toString(QUrl::FullyEncoded)));

		QNetworkRequest request(url);
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		request.setRawHeader("Accept", "application/json");
		request.setRawHeader("User-Agent", "AstrogramDesktop");
		request.setRawHeader("Cache-Control", "no-cache");
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
				auto nextRequestAt = crl::now() + ServerBadgeRetryDelay(stored->retryCount + 1);
				auto retryReason = QString();
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
							stored->retryCount = 0;
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
						retryReason = u"unrecognized_response"_q;
						Logs::writeClient(QString::fromLatin1(
							"[badge] response not recognized: peer=%1 body=%2")
							.arg(QString::number(qulonglong(peerId.value)))
							.arg(ServerBadgeLogText(body)));
					}
				} else {
					retryReason = reply->errorString();
					Logs::writeClient(QString::fromLatin1(
						"[badge] request failed: peer=%1 http=%2 reason=%3 body=%4")
						.arg(QString::number(qulonglong(peerId.value)))
						.arg(statusCode)
						.arg(reply->errorString())
						.arg(ServerBadgeLogText(body)));
				}
				if (!resolved) {
					stored->retryCount = std::min(stored->retryCount + 1, 16);
					nextRequestAt = crl::now() + ServerBadgeRetryDelay(stored->retryCount);
				}
				stored->nextRequestAt = nextRequestAt;
				if (resolved && stored->emojiStatusId != next) {
					stored->emojiStatusId = next;
					stored->updated.fire({});
				}
				scheduleRetry(
					peerId,
					stored.get(),
					resolved ? u"success-refresh"_q : retryReason);
				reply->deleteLater();
			});
	}

	std::map<uint64, std::unique_ptr<Entry>> _entries;
	bool _changesInFlight = false;
	int _changesRevision = 0;
	std::unique_ptr<base::Timer> _changesRetryTimer;
	QString _changesApiBaseUrl;
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
			if (serverBadgeStatus->documentId) {
				badge = BadgeType::Premium;
				emojiStatusId = *serverBadgeStatus;
			} else {
				badge = BadgeType::Verified;
				emojiStatusId = EmojiStatusId();
			}
		}
		if (statusOnlyForPremium && badge != BadgeType::Premium) {
			emojiStatusId = EmojiStatusId();
		}
		return Badge::Content{ badge, emojiStatusId };
	}) | rpl::distinct_until_changed() | rpl::map([=](Badge::Content content) {
		Logs::writeClient(QString::fromLatin1(
			"[badge] content resolved: peer=%1 badge=%2 emojiStatusId=%3 collectible=%4")
			.arg(QString::number(qulonglong(peer->id.value)))
			.arg(BadgeTypeForLog(content.badge))
			.arg(QString::number(content.emojiStatusId.documentId))
			.arg(content.emojiStatusId.collectible ? u"true"_q : u"false"_q));
		return content;
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
