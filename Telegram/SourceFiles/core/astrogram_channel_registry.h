/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "apiwrap.h"
#include "base/timer.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/core_types.h"

#include <rpl/event_stream.h>
#include <rpl/producer.h>

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QString>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Core::AstrogramChannelRegistry {
namespace details {

inline constexpr auto kRegistryChannelBareId = ChannelId(BareId(3960998300ULL));
inline constexpr auto kRegistryChannelFullId = int64(-1003960998300LL);
inline constexpr auto kFetchLimit = 128;
inline constexpr auto kSuccessTtl = crl::time(5 * 60 * 1000);
inline constexpr auto kRetryTtl = crl::time(30 * 1000);
inline constexpr auto kRetryMaxTtl = crl::time(5 * 60 * 1000);

[[nodiscard]] inline PeerId RegistryPeerId() {
	return peerFromChannel(kRegistryChannelBareId);
}

[[nodiscard]] inline crl::time RetryDelay(int retryCount) {
	auto delay = kRetryTtl;
	for (auto i = 1; (i < retryCount) && (delay < kRetryMaxTtl); ++i) {
		delay = std::min(delay * 2, kRetryMaxTtl);
	}
	return delay;
}

[[nodiscard]] inline QString NormalizeSha256(QString value) {
	value = value.trimmed().toLower();
	if (value.startsWith(u"sha256:"_q)) {
		value = value.mid(7);
	}
	if (value.size() != 64) {
		return QString();
	}
	for (const auto ch : value) {
		const auto hex = ch.unicode();
		if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f'))) {
			return QString();
		}
	}
	return value;
}

[[nodiscard]] inline std::optional<uint64> ParsePositiveId(QString value) {
	value = value.trimmed();
	if (value.isEmpty()) {
		return std::nullopt;
	}
	auto ok = false;
	const auto parsed = value.toULongLong(&ok);
	return ok ? std::optional<uint64>(parsed) : std::nullopt;
}

[[nodiscard]] inline std::optional<int64> ParseSignedId(QString value) {
	value = value.trimmed();
	if (value.isEmpty()) {
		return std::nullopt;
	}
	auto ok = false;
	const auto parsed = value.toLongLong(&ok);
	return ok ? std::optional<int64>(parsed) : std::nullopt;
}

[[nodiscard]] inline std::optional<ChannelId> ParseChannelBareId(QString value) {
	value = value.trimmed();
	if (value.isEmpty()) {
		return std::nullopt;
	}
	if (value.startsWith(u"-100"_q)) {
		value = value.mid(4);
	} else if (value.startsWith(u'-')) {
		return std::nullopt;
	}
	if (const auto parsed = ParsePositiveId(value)) {
		return ChannelId(*parsed);
	}
	return std::nullopt;
}

[[nodiscard]] inline std::optional<int64> ParseFullChannelId(QString value) {
	if (const auto bare = ParseChannelBareId(std::move(value))) {
		auto ok = false;
		const auto full = (u"-100"_q + QString::number(qulonglong(bare->bare)))
			.toLongLong(&ok);
		if (ok) {
			return full;
		}
	}
	return std::nullopt;
}

[[nodiscard]] inline std::optional<uint64> ParseBadgePeerKey(
		const QString &kind,
		const QString &rawId) {
	const auto normalizedKind = kind.trimmed().toLower();
	if (normalizedKind == u"user"_q) {
		if (const auto parsed = ParsePositiveId(rawId)) {
			return uint64(peerFromUser(UserId(*parsed)).value);
		}
	} else if (normalizedKind == u"channel"_q) {
		if (const auto parsed = ParseChannelBareId(rawId)) {
			return uint64(peerFromChannel(*parsed).value);
		}
	} else if (normalizedKind == u"chat"_q) {
		if (const auto parsed = ParsePositiveId(rawId)) {
			return uint64(peerFromChat(ChatId(*parsed)).value);
		}
	}
	return std::nullopt;
}

[[nodiscard]] inline QString MessageText(const MTPmessage &message) {
	return message.match([](const MTPDmessage &data) {
		return qs(data.vmessage());
	}, [](const auto &) {
		return QString();
	});
}

[[nodiscard]] inline int64 MessageId(const MTPmessage &message) {
	return message.match([](const auto &data) {
		return int64(data.vid().v);
	});
}

struct TrustedPluginRecord {
	int64 sourceChannelId = 0;
	int64 registryMessageId = 0;

	friend inline bool operator==(
		const TrustedPluginRecord &,
		const TrustedPluginRecord &) = default;
};

struct Snapshot {
	QHash<uint64, std::optional<EmojiStatusId>> peerBadges;
	QSet<int64> trustedSourceChannels;
	QHash<QString, TrustedPluginRecord> trustedPluginHashes;
	int revision = 0;

	friend inline bool operator==(
		const Snapshot &,
		const Snapshot &) = default;
};

[[nodiscard]] inline bool ApplySimpleRegistryValue(
		Snapshot &snapshot,
		const QString &trimmed,
		int64 messageId) {
	if (const auto sha = NormalizeSha256(trimmed); !sha.isEmpty()) {
		snapshot.trustedPluginHashes.insert(sha, TrustedPluginRecord{
			.registryMessageId = messageId,
		});
		return true;
	}
	if (trimmed.startsWith(u"sha256:"_q)) {
		if (const auto sha = NormalizeSha256(trimmed); !sha.isEmpty()) {
			snapshot.trustedPluginHashes.insert(sha, TrustedPluginRecord{
				.registryMessageId = messageId,
			});
			return true;
		}
	}
	if (const auto signedId = ParseSignedId(trimmed)) {
		if (*signedId < 0) {
			if (const auto fullChannelId = ParseFullChannelId(trimmed)) {
				snapshot.trustedSourceChannels.insert(*fullChannelId);
				if (const auto bare = ParseChannelBareId(trimmed)) {
					snapshot.peerBadges.insert(
						uint64(peerFromChannel(*bare).value),
						std::optional<EmojiStatusId>(EmojiStatusId()));
				}
				return true;
			}
		} else if (*signedId > 0) {
			snapshot.peerBadges.insert(
				uint64(peerFromUser(UserId(uint64(*signedId))).value),
				std::optional<EmojiStatusId>(EmojiStatusId()));
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline std::vector<QString> SplitRegistryParts(QString line) {
	auto parts = line.split(u':' , Qt::KeepEmptyParts);
	for (auto &part : parts) {
		part = part.trimmed();
	}
	return { parts.begin(), parts.end() };
}

inline void ApplyRegistryLine(
		Snapshot &snapshot,
		const QString &line,
		int64 messageId) {
	auto trimmed = line.trimmed();
	if (trimmed.isEmpty()
		|| trimmed.startsWith(u'#')
		|| trimmed.startsWith(u"//"_q)) {
		return;
	}
	if (ApplySimpleRegistryValue(snapshot, trimmed, messageId)) {
		return;
	}
	const auto parts = SplitRegistryParts(trimmed);
	if (parts.empty()) {
		return;
	}
	const auto command = parts[0].trimmed().toLower();
	const auto isAdd = !command.startsWith(u"un"_q) && !command.startsWith(u"remove-"_q);

	if ((command == u"badge"_q || command == u"unbadge"_q)
		&& (parts.size() >= 3)) {
		if (const auto peerKey = ParseBadgePeerKey(parts[1], parts[2])) {
			auto badge = std::optional<EmojiStatusId>(EmojiStatusId());
			if ((parts.size() >= 5) && (parts[3].toLower() == u"emoji"_q)) {
				if (const auto emojiId = ParsePositiveId(parts[4])) {
					badge = EmojiStatusId{ .documentId = DocumentId(*emojiId) };
				}
			}
			if (isAdd) {
				snapshot.peerBadges.insert(*peerKey, badge);
			} else {
				snapshot.peerBadges.remove(*peerKey);
			}
		}
		return;
	}

	if ((command == u"trusted-source"_q
			|| command == u"untrusted-source"_q
			|| command == u"remove-trusted-source"_q)
		&& (parts.size() >= 2)) {
		if (const auto channelId = ParseFullChannelId(parts.back())) {
			if (isAdd) {
				snapshot.trustedSourceChannels.insert(*channelId);
			} else {
				snapshot.trustedSourceChannels.remove(*channelId);
			}
		}
		return;
	}

	if ((command == u"plugin-sha256"_q
			|| command == u"trusted-plugin"_q
			|| command == u"plugin"_q
			|| command == u"untrusted-plugin"_q
			|| command == u"remove-plugin"_q)
		&& (parts.size() >= 2)) {
		const auto sha = NormalizeSha256(parts[1]);
		if (sha.isEmpty()) {
			return;
		}
		if (!isAdd) {
			snapshot.trustedPluginHashes.remove(sha);
			return;
		}
		auto record = TrustedPluginRecord{
			.registryMessageId = messageId,
		};
		if ((parts.size() >= 4) && (parts[2].toLower() == u"channel"_q)) {
			if (const auto channelId = ParseFullChannelId(parts[3])) {
				record.sourceChannelId = *channelId;
			}
		}
		snapshot.trustedPluginHashes.insert(sha, record);
		return;
	}
}

[[nodiscard]] inline Snapshot ParseRegistryMessages(
		const std::vector<MTPMessage> &messages) {
	auto snapshot = Snapshot();
	for (auto i = messages.crbegin(); i != messages.crend(); ++i) {
		const auto text = MessageText(*i);
		if (text.isEmpty()) {
			continue;
		}
		const auto messageId = MessageId(*i);
		for (const auto &line : text.split(u'\n', Qt::KeepEmptyParts)) {
			ApplyRegistryLine(snapshot, line, messageId);
		}
	}
	return snapshot;
}

[[nodiscard]] inline QString ChannelTitle(
		Main::Session *session,
		int64 fullChannelId) {
	if (!session) {
		return QString();
	}
	const auto bare = ParseChannelBareId(QString::number(fullChannelId));
	if (!bare) {
		return QString();
	}
	if (const auto channel = session->data().channelLoaded(*bare)) {
		return channel->name();
	}
	return QString();
}

[[nodiscard]] inline QString ChannelUsername(
		Main::Session *session,
		int64 fullChannelId) {
	if (!session) {
		return QString();
	}
	const auto bare = ParseChannelBareId(QString::number(fullChannelId));
	if (!bare) {
		return QString();
	}
	if (const auto channel = session->data().channelLoaded(*bare)) {
		return channel->username();
	}
	return QString();
}

} // namespace details

struct BadgeLookup {
	bool loading = false;
	std::optional<EmojiStatusId> badge;
};

struct PluginTrustSnapshot {
	bool loading = false;
	bool verified = false;
	QString reason;
	QString details;
	int64 channelId = 0;
	int64 messageId = 0;
	QString channelTitle;
	QString channelUsername;
};

class Registry final {
public:
	[[nodiscard]] static Registry &Instance() {
		static auto instance = Registry();
		return instance;
	}

	void ensureLoaded(Main::Session *session) {
		if (!session) {
			return;
		}
		auto &state = stateFor(session);
		subscribeToChannelUpdates(state);
		if (state.inFlight) {
			return;
		}
		if (state.loaded && (state.nextRefreshAt > crl::now())) {
			return;
		}
		requestRefresh(state);
	}

	[[nodiscard]] rpl::producer<> updates(Main::Session *session) {
		auto &state = stateFor(session);
		subscribeToChannelUpdates(state);
		ensureLoaded(session);
		return state.updated.events();
	}

	[[nodiscard]] BadgeLookup badgeLookup(PeerData *peer) {
		if (!peer) {
			return {};
		}
		auto &state = stateFor(&peer->session());
		subscribeToChannelUpdates(state);
		ensureLoaded(&peer->session());
		auto result = BadgeLookup{
			.loading = !state.loaded,
		};
		if (const auto it = state.snapshot.peerBadges.constFind(uint64(peer->id.value));
			it != state.snapshot.peerBadges.cend()) {
			result.badge = it.value();
		}
		return result;
	}

	[[nodiscard]] PluginTrustSnapshot pluginTrustSnapshot(
			Main::Session *session,
			QString sha256,
			int64 sourceChannelIdHint = 0) {
		if (!session) {
			return {
				.loading = true,
				.reason = u"channel-feed-session-unavailable"_q,
				.channelId = sourceChannelIdHint,
			};
		}
		auto &state = stateFor(session);
		subscribeToChannelUpdates(state);
		ensureLoaded(session);
		auto result = PluginTrustSnapshot{
			.loading = !state.loaded,
			.reason = !state.loaded
				? u"channel-feed-refreshing"_q
				: state.lastError.isEmpty()
				? u"channel-feed-no-match"_q
				: u"channel-feed-refresh-failed"_q,
			.details = state.lastError,
			.channelId = sourceChannelIdHint,
		};
		sha256 = details::NormalizeSha256(std::move(sha256));
		if (!state.loaded) {
			return result;
		}
		if (const auto it = state.snapshot.trustedPluginHashes.constFind(sha256);
			it != state.snapshot.trustedPluginHashes.cend()) {
			result.verified = true;
			result.reason = u"channel-feed-exact-sha256"_q;
			result.channelId = it.value().sourceChannelId
				? it.value().sourceChannelId
				: sourceChannelIdHint;
			result.messageId = it.value().registryMessageId;
		} else if (sourceChannelIdHint
			&& state.snapshot.trustedSourceChannels.contains(sourceChannelIdHint)) {
			result.verified = true;
			result.reason = u"channel-feed-trusted-source"_q;
			result.channelId = sourceChannelIdHint;
		}
		result.channelTitle = details::ChannelTitle(session, result.channelId);
		result.channelUsername = details::ChannelUsername(session, result.channelId);
		if (result.verified) {
			if (!result.channelTitle.isEmpty()) {
				result.details = result.channelTitle;
			} else if (!result.channelUsername.isEmpty()) {
				result.details = u'@' + result.channelUsername;
			} else if (result.channelId) {
				result.details = QString::number(result.channelId);
			} else {
				result.details = result.reason;
			}
		} else if (result.details.isEmpty()) {
			result.details = result.reason;
		}
		return result;
	}

private:
	struct SessionState {
		uint64 key = 0;
		Main::Session *raw = nullptr;
		base::weak_ptr<Main::Session> weak;
		bool subscribed = false;
		bool loaded = false;
		bool inFlight = false;
		int retryCount = 0;
		crl::time nextRefreshAt = 0;
		QString lastError;
		details::Snapshot snapshot;
		rpl::event_stream<> updated;
		rpl::lifetime updatesLifetime;
		std::unique_ptr<base::Timer> refreshTimer;
	};

	Registry() = default;

	[[nodiscard]] SessionState &stateFor(Main::Session *session) {
		sweepDeadStates();
		const auto key = session ? session->uniqueId() : 0;
		auto &entry = _states[key];
		if (!entry) {
			entry = std::make_unique<SessionState>();
			entry->key = key;
			entry->raw = session;
			entry->weak = base::make_weak(session);
		}
		return *entry;
	}

	void sweepDeadStates() {
		for (auto i = _states.begin(); i != _states.end();) {
			if (!i->second || !i->second->weak.get()) {
				i = _states.erase(i);
			} else {
				++i;
			}
		}
	}

	void subscribeToChannelUpdates(SessionState &state) {
		if (state.subscribed) {
			return;
		}
		const auto session = state.weak.get();
		if (!session) {
			return;
		}
		state.subscribed = true;
		const auto refresh = [=](const Data::MessageUpdate &update) {
			const auto session = state.weak.get();
			if (!session || !update.item) {
				return;
			}
			const auto history = update.item->history();
			if (!history || !history->peer || (history->peer->id != details::RegistryPeerId())) {
				return;
			}
			invalidateAndRefresh(state.key);
		};
		session->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::on_next(refresh, state.updatesLifetime);
		session->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::Edited
		) | rpl::on_next(refresh, state.updatesLifetime);
		session->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::Destroyed
		) | rpl::on_next(refresh, state.updatesLifetime);
	}

	void scheduleRefresh(SessionState &state, crl::time delay) {
		if (!state.refreshTimer) {
			state.refreshTimer = std::make_unique<base::Timer>([=] {
				auto i = _states.find(state.key);
				if (i == _states.end() || !i->second) {
					return;
				}
				if (const auto session = i->second->weak.get()) {
					requestRefresh(*i->second);
				}
			});
		}
		state.refreshTimer->callOnce(std::max(delay, crl::time(0)));
	}

	void invalidateAndRefresh(uint64 key) {
		const auto i = _states.find(key);
		if (i == _states.end() || !i->second) {
			return;
		}
		i->second->nextRefreshAt = 0;
		if (const auto session = i->second->weak.get()) {
			requestRefresh(*i->second);
		}
	}

	void requestRefresh(SessionState &state) {
		const auto session = state.weak.get();
		if (!session || state.inFlight) {
			return;
		}
		if (state.refreshTimer) {
			state.refreshTimer->cancel();
		}
		state.inFlight = true;
		const auto key = state.key;
		resolveChannel(state, [=](ChannelData *channel) {
			const auto i = _states.find(key);
			if (i == _states.end() || !i->second) {
				return;
			}
			auto &state = *i->second;
			const auto session = state.weak.get();
			if (!session || !channel) {
				finishFailure(state, u"channel-feed-unavailable"_q);
				return;
			}
			session->api().request(MTPmessages_GetHistory(
				channel->input(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(details::kFetchLimit),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=](const MTPmessages_Messages &result) {
				const auto i = _states.find(key);
				if (i == _states.end() || !i->second) {
					return;
				}
				auto &state = *i->second;
				const auto session = state.weak.get();
				if (!session) {
					return;
				}
				auto messages = std::vector<MTPMessage>();
				result.match([&](const MTPDmessages_messagesNotModified &) {
				}, [&](const MTPDmessages_messages &data) {
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					const auto &loaded = data.vmessages().v;
					messages.assign(loaded.cbegin(), loaded.cend());
				}, [&](const MTPDmessages_messagesSlice &data) {
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					const auto &loaded = data.vmessages().v;
					messages.assign(loaded.cbegin(), loaded.cend());
				}, [&](const MTPDmessages_channelMessages &data) {
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					const auto &loaded = data.vmessages().v;
					messages.assign(loaded.cbegin(), loaded.cend());
				});
				auto snapshot = details::ParseRegistryMessages(messages);
				snapshot.revision = state.snapshot.revision + 1;
				finishSuccess(state, std::move(snapshot));
			}).fail([=](const MTP::Error &error) {
				const auto i = _states.find(key);
				if (i == _states.end() || !i->second) {
					return false;
				}
				auto &state = *i->second;
				finishFailure(state, error.type());
				return false;
			}).send();
		});
	}

	template <typename Done>
	void resolveChannel(SessionState &state, Done done) {
		const auto session = state.weak.get();
		if (!session) {
			done(nullptr);
			return;
		}
		if (const auto loaded = session->data().channelLoaded(details::kRegistryChannelBareId)) {
			done(loaded);
			return;
		}
		session->api().request(MTPchannels_GetChannels(
			MTP_vector<MTPInputChannel>(
				1,
				MTP_inputChannel(
					MTP_long(details::kRegistryChannelBareId.bare),
					MTP_long(0)))
		)).done([=](const MTPmessages_Chats &result) {
			const auto session = state.weak.get();
			if (!session) {
				return;
			}
			auto resolved = static_cast<ChannelData*>(nullptr);
			result.match([&](const auto &data) {
				const auto peer = session->data().processChats(data.vchats());
				if (peer && (peer->id == details::RegistryPeerId())) {
					resolved = peer->asChannel();
				}
			});
			done(resolved);
		}).fail([=](const MTP::Error &) {
			done(nullptr);
			return false;
		}).send();
	}

	void finishSuccess(SessionState &state, details::Snapshot snapshot) {
		state.inFlight = false;
		state.retryCount = 0;
		state.nextRefreshAt = crl::now() + details::kSuccessTtl;
		state.lastError.clear();
		const auto changed = !state.loaded || (state.snapshot != snapshot);
		state.loaded = true;
		state.snapshot = std::move(snapshot);
		scheduleRefresh(state, details::kSuccessTtl);
		if (changed) {
			state.updated.fire({});
		}
	}

	void finishFailure(SessionState &state, QString error) {
		state.inFlight = false;
		state.retryCount = std::min(state.retryCount + 1, 16);
		state.nextRefreshAt = crl::now() + details::RetryDelay(state.retryCount);
		state.lastError = error.trimmed();
		const auto firstResolve = !state.loaded;
		state.loaded = true;
		scheduleRefresh(state, state.nextRefreshAt - crl::now());
		if (firstResolve) {
			state.updated.fire({});
		}
	}

	std::map<uint64, std::unique_ptr<SessionState>> _states;
};

} // namespace Core::AstrogramChannelRegistry
