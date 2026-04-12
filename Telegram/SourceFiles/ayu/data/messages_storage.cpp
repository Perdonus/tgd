/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/data/messages_storage.h"

#include "base/unixtime.h"
#include "data/data_peer_id.h"
#include "lang/lang_instance.h"
#include "data/data_session.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "storage/storage_shared_media.h"

#include <algorithm>
#include <optional>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <type_traits>

#include <rpl/event_stream.h>

namespace AyuMessages {
namespace {

[[nodiscard]] QString StoragePath() {
	return u"./tdata/astro_recall_log.jsonl"_q;
}

struct StorageCache {
	QString filePath;
	QDateTime lastModified;
	qint64 size = -1;
	std::vector<MessageSnapshot> snapshots;
	bool loaded = false;
};

[[nodiscard]] StorageCache &SnapshotsCache() {
	static auto cache = StorageCache();
	return cache;
}

[[nodiscard]] rpl::event_stream<> &DeletedMessagesChangedStream() {
	static auto stream = rpl::event_stream<>();
	return stream;
}

void InvalidateSnapshotsCache() {
	auto &cache = SnapshotsCache();
	cache = StorageCache();
}

[[nodiscard]] bool RussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString StorageText(const char *en, const char *ru) {
	return RussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString MediaFallbackText(not_null<HistoryItem*> item) {
	using Type = Storage::SharedMediaType;
	const auto types = item->sharedMediaTypes();
	if (types.test(Type::RoundVoiceFile) || types.test(Type::RoundFile)) {
		return StorageText("Video message", "Видеосообщение");
	} else if (types.test(Type::VoiceFile)) {
		return StorageText("Voice message", "Голосовое сообщение");
	} else if (types.test(Type::MusicFile)) {
		return StorageText("Music", "Музыка");
	} else if (types.test(Type::GIF)) {
		return u"GIF"_q;
	} else if (types.test(Type::Video)) {
		return StorageText("Video", "Видео");
	} else if (types.test(Type::PhotoVideo)) {
		return StorageText("Photo or video", "Фото или видео");
	} else if (types.test(Type::Photo) || types.test(Type::ChatPhoto)) {
		return StorageText("Photo", "Фотография");
	} else if (types.test(Type::File)) {
		return StorageText("File", "Файл");
	} else if (types.test(Type::Link)) {
		return StorageText("Link", "Ссылка");
	} else if (types.test(Type::Pinned)) {
		return StorageText("Pinned message", "Закреплённое сообщение");
	} else if (item->media()) {
		return StorageText("Media message", "Медиасообщение");
	} else if (item->isService()) {
		return StorageText("Service message", "Служебное сообщение");
	}
	return StorageText("Saved message", "Сохранённое сообщение");
}

[[nodiscard]] QString SnapshotText(not_null<HistoryItem*> item) {
	auto text = item->originalText().text.trimmed();
	if (!text.isEmpty()) {
		return text;
	}
	text = item->notificationText().text.trimmed();
	if (!text.isEmpty()) {
		return text;
	}
	return MediaFallbackText(item);
}

[[nodiscard]] MessageSnapshot MapSnapshot(
		not_null<HistoryItem*> item,
		const QString &kind) {
	auto snapshot = MessageSnapshot();
	const auto sessionUserId = item->history()->owner().session().userId();
	const auto from = item->from();
	snapshot.kind = kind;
	snapshot.userId = sessionUserId.bare & PeerId::kChatTypeMask;
	snapshot.dialogId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	snapshot.peerId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	snapshot.fromId = from ? (from->id.value & PeerId::kChatTypeMask) : 0;
	snapshot.topicId = item->topic() ? item->topicRootId().bare : 0;
	snapshot.userSerialized = SerializePeerId(peerFromUser(sessionUserId));
	snapshot.dialogSerialized = SerializePeerId(item->history()->peer->id);
	snapshot.senderSerialized = from ? SerializePeerId(from->id) : 0;
	snapshot.messageId = item->id.bare;
	snapshot.date = item->date();
	snapshot.editDate = base::unixtime::now();
	snapshot.senderName = from ? from->name() : item->history()->peer->name();
	snapshot.text = SnapshotText(item);
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		snapshot.editDate = edited->date;
	}
	return snapshot;
}

void AppendSnapshot(const MessageSnapshot &snapshot) {
	QDir().mkpath(u"./tdata"_q);
	auto file = QFile(StoragePath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		return;
	}

	const auto object = QJsonObject{
		{ u"kind"_q, snapshot.kind },
		{ u"userId"_q, QString::number(snapshot.userId) },
		{ u"dialogId"_q, QString::number(snapshot.dialogId) },
		{ u"peerId"_q, QString::number(snapshot.peerId) },
		{ u"fromId"_q, QString::number(snapshot.fromId) },
		{ u"topicId"_q, QString::number(snapshot.topicId) },
		{ u"userSerialized"_q, QString::number(snapshot.userSerialized) },
		{ u"dialogSerialized"_q, QString::number(snapshot.dialogSerialized) },
		{ u"senderSerialized"_q, QString::number(snapshot.senderSerialized) },
		{ u"messageId"_q, snapshot.messageId },
		{ u"date"_q, snapshot.date },
		{ u"editDate"_q, snapshot.editDate },
		{ u"senderName"_q, snapshot.senderName },
		{ u"text"_q, snapshot.text },
	};
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	file.write("\n");
	file.close();
	InvalidateSnapshotsCache();
}

[[nodiscard]] bool MatchesItem(
		not_null<HistoryItem*> item,
		const MessageSnapshot &snapshot) {
	const auto userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;
	const auto dialogId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	const auto userSerialized = SerializePeerId(peerFromUser(
		item->history()->owner().session().userId()));
	const auto dialogSerialized = SerializePeerId(item->history()->peer->id);
	const auto topicId = item->topic() ? item->topicRootId().bare : 0;
	const auto sameUser = snapshot.userSerialized
		? (snapshot.userSerialized == userSerialized)
		: (snapshot.userId == userId);
	const auto sameDialog = snapshot.dialogSerialized
		? (snapshot.dialogSerialized == dialogSerialized)
		: (snapshot.dialogId == dialogId);
	return (snapshot.kind == u"edited"_q)
		&& sameUser
		&& sameDialog
		&& (snapshot.topicId == topicId)
		&& (snapshot.messageId == item->id.bare);
}

[[nodiscard]] bool MatchesDeletedItem(
		not_null<HistoryItem*> item,
		const MessageSnapshot &snapshot) {
	const auto userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;
	const auto dialogId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	const auto userSerialized = SerializePeerId(peerFromUser(
		item->history()->owner().session().userId()));
	const auto dialogSerialized = SerializePeerId(item->history()->peer->id);
	const auto topicId = item->topic() ? item->topicRootId().bare : 0;
	const auto sameUser = snapshot.userSerialized
		? (snapshot.userSerialized == userSerialized)
		: (snapshot.userId == userId);
	const auto sameDialog = snapshot.dialogSerialized
		? (snapshot.dialogSerialized == dialogSerialized)
		: (snapshot.dialogId == dialogId);
	return (snapshot.kind == u"deleted"_q)
		&& sameUser
		&& sameDialog
		&& (snapshot.topicId == topicId)
		&& (snapshot.messageId == item->id.bare);
}

template <typename Int>
[[nodiscard]] Int ParseJsonNumber(const QJsonValue &value) {
	if (value.isDouble()) {
		return static_cast<Int>(value.toDouble());
	} else if (value.isString()) {
		if constexpr (std::is_unsigned_v<Int>) {
			return static_cast<Int>(value.toString().toULongLong());
		} else {
			return static_cast<Int>(value.toString().toLongLong());
		}
	}
	return 0;
}

[[nodiscard]] std::optional<MessageSnapshot> ParseSnapshotLine(
		const QByteArray &line) {
	if (line.trimmed().isEmpty()) {
		return std::nullopt;
	}
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson(line, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	auto snapshot = MessageSnapshot();
	snapshot.kind = object.value(u"kind"_q).toString();
	snapshot.userId = ParseJsonNumber<ID>(object.value(u"userId"_q));
	snapshot.dialogId = ParseJsonNumber<ID>(object.value(u"dialogId"_q));
	snapshot.peerId = ParseJsonNumber<ID>(object.value(u"peerId"_q));
	snapshot.fromId = ParseJsonNumber<ID>(object.value(u"fromId"_q));
	snapshot.topicId = ParseJsonNumber<ID>(object.value(u"topicId"_q));
	snapshot.userSerialized = ParseJsonNumber<quint64>(
		object.value(u"userSerialized"_q));
	snapshot.dialogSerialized = ParseJsonNumber<quint64>(
		object.value(u"dialogSerialized"_q));
	snapshot.senderSerialized = ParseJsonNumber<quint64>(
		object.value(u"senderSerialized"_q));
	snapshot.messageId = ParseJsonNumber<int>(object.value(u"messageId"_q));
	snapshot.date = ParseJsonNumber<int>(object.value(u"date"_q));
	snapshot.editDate = ParseJsonNumber<int>(object.value(u"editDate"_q));
	snapshot.senderName = object.value(u"senderName"_q).toString();
	snapshot.text = object.value(u"text"_q).toString();
	return snapshot;
}

[[nodiscard]] QString SnapshotDedupKey(const MessageSnapshot &snapshot) {
	return QString::number(snapshot.userSerialized)
		+ u'|'
		+ QString::number(snapshot.dialogSerialized)
		+ u'|'
		+ QString::number(snapshot.senderSerialized)
		+ u'|'
		+ snapshot.kind
		+ u'|'
		+ QString::number(snapshot.topicId)
		+ u'|'
		+ QString::number(snapshot.messageId)
		+ u'|'
		+ QString::number(snapshot.date)
		+ u'|'
		+ QString::number(snapshot.editDate)
		+ u'|'
		+ snapshot.senderName
		+ u'|'
		+ snapshot.text;
}

[[nodiscard]] const std::vector<MessageSnapshot> &ReadAllSnapshots() {
	auto &cache = SnapshotsCache();
	const auto path = StoragePath();
	const auto info = QFileInfo(path);
	const auto filePath = info.exists()
		? info.canonicalFilePath()
		: info.absoluteFilePath();
	const auto lastModified = info.lastModified();
	const auto size = info.exists() ? info.size() : -1;
	if (cache.loaded
		&& (cache.filePath == filePath)
		&& (cache.lastModified == lastModified)
		&& (cache.size == size)) {
		return cache.snapshots;
	}

	cache = StorageCache();
	cache.filePath = filePath;
	cache.lastModified = lastModified;
	cache.size = size;
	cache.loaded = true;
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return cache.snapshots;
	}

	auto seen = QSet<QString>();
	while (!file.atEnd()) {
		const auto parsed = ParseSnapshotLine(file.readLine());
		if (!parsed) {
			continue;
		}
		const auto dedupKey = SnapshotDedupKey(*parsed);
		if (seen.contains(dedupKey)) {
			continue;
		}
		seen.insert(dedupKey);
		cache.snapshots.push_back(*parsed);
	}
	file.close();
	return cache.snapshots;
}

[[nodiscard]] bool MatchesDialog(
		not_null<PeerData*> peer,
		const MessageSnapshot &snapshot) {
	const auto matchesCandidate = [&](const PeerData *candidate) {
		if (!candidate) {
			return false;
		}
		const auto dialogSerialized = SerializePeerId(candidate->id);
		const auto dialogId = candidate->id.value & PeerId::kChatTypeMask;
		return snapshot.dialogSerialized
			? (snapshot.dialogSerialized == dialogSerialized)
			: (snapshot.dialogId == dialogId);
	};
	return matchesCandidate(peer)
		|| matchesCandidate(peer->migrateFrom())
		|| matchesCandidate(peer->migrateTo());
}

[[nodiscard]] bool MatchesPeer(
		not_null<PeerData*> peer,
		ID topicId,
		const MessageSnapshot &snapshot) {
	const auto userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	const auto userSerialized = SerializePeerId(peerFromUser(peer->session().userId()));
	const auto sameUser = snapshot.userSerialized
		? (snapshot.userSerialized == userSerialized)
		: (snapshot.userId == userId);
	return (snapshot.kind == u"deleted"_q)
		&& sameUser
		&& MatchesDialog(peer, snapshot)
		&& (snapshot.topicId == topicId);
}

void SortSnapshots(std::vector<MessageSnapshot> *snapshots) {
	std::sort(
		snapshots->begin(),
		snapshots->end(),
		[](const MessageSnapshot &a, const MessageSnapshot &b) {
			if (a.editDate != b.editDate) {
				return a.editDate > b.editDate;
			}
			return a.date > b.date;
		});
}

} // namespace

void addEditedMessage(not_null<HistoryItem*> item) {
	AppendSnapshot(MapSnapshot(item, u"edited"_q));
}

std::vector<MessageSnapshot> getEditedMessages(
		not_null<HistoryItem*> item,
		int totalLimit) {
	auto result = std::vector<MessageSnapshot>();
	for (const auto &snapshot : ReadAllSnapshots()) {
		if (!MatchesItem(item, snapshot)) {
			continue;
		}
		result.push_back(snapshot);
	}
	SortSnapshots(&result);
	if (totalLimit > 0 && int(result.size()) > totalLimit) {
		result.resize(totalLimit);
	}
	return result;
}

bool hasRevisions(not_null<HistoryItem*> item) {
	for (const auto &snapshot : ReadAllSnapshots()) {
		if (MatchesItem(item, snapshot)) {
			return true;
		}
	}
	return false;
}

void addDeletedMessage(not_null<HistoryItem*> item) {
	AppendSnapshot(MapSnapshot(item, u"deleted"_q));
	DeletedMessagesChangedStream().fire({});
}

std::vector<MessageSnapshot> getDeletedMessages(
		not_null<PeerData*> peer,
		ID topicId,
		int totalLimit) {
	auto result = std::vector<MessageSnapshot>();
	for (const auto &snapshot : ReadAllSnapshots()) {
		if (!MatchesPeer(peer, topicId, snapshot)) {
			continue;
		}
		result.push_back(snapshot);
	}
	SortSnapshots(&result);
	if (totalLimit > 0 && int(result.size()) > totalLimit) {
		result.resize(totalLimit);
	}
	return result;
}

bool hasDeletedMessages(
		not_null<PeerData*> peer,
		ID topicId) {
	for (const auto &snapshot : ReadAllSnapshots()) {
		if (MatchesPeer(peer, topicId, snapshot)) {
			return true;
		}
	}
	return false;
}

std::optional<MessageSnapshot> lookupDeletedMessage(not_null<HistoryItem*> item) {
	auto result = std::optional<MessageSnapshot>();
	for (const auto &snapshot : ReadAllSnapshots()) {
		if (!MatchesDeletedItem(item, snapshot)) {
			continue;
		}
		if (!result
			|| (snapshot.editDate > result->editDate)
			|| ((snapshot.editDate == result->editDate)
				&& (snapshot.date > result->date))) {
			result = snapshot;
		}
	}
	return result;
}

rpl::producer<> deletedMessagesChanged() {
	return DeletedMessagesChangedStream().events();
}

} // namespace AyuMessages
