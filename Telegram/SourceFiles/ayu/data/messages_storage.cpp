/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/data/messages_storage.h"

#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"

#include <algorithm>
#include <optional>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace AyuMessages {
namespace {

[[nodiscard]] QString StoragePath() {
	return u"./tdata/astro_recall_log.jsonl"_q;
}

[[nodiscard]] MessageSnapshot MapSnapshot(
		not_null<HistoryItem*> item,
		const QString &kind) {
	auto snapshot = MessageSnapshot();
	snapshot.kind = kind;
	snapshot.userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;
	snapshot.dialogId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	snapshot.peerId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	snapshot.fromId = item->from()->id.value & PeerId::kChatTypeMask;
	snapshot.topicId = item->topic() ? item->topicRootId().bare : 0;
	snapshot.messageId = item->id.bare;
	snapshot.date = item->date();
	snapshot.editDate = base::unixtime::now();
	snapshot.text = item->originalText().text;
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		snapshot.editDate = edited->date;
	}
	return snapshot;
}

void AppendSnapshot(const MessageSnapshot &snapshot) {
	if (snapshot.text.isEmpty()) {
		return;
	}

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
		{ u"messageId"_q, snapshot.messageId },
		{ u"date"_q, snapshot.date },
		{ u"editDate"_q, snapshot.editDate },
		{ u"text"_q, snapshot.text },
	};
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	file.write("\n");
	file.close();
}

[[nodiscard]] bool MatchesItem(
		not_null<HistoryItem*> item,
		const MessageSnapshot &snapshot) {
	const auto userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;
	const auto dialogId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	const auto topicId = item->topic() ? item->topicRootId().bare : 0;
	return (snapshot.kind == u"edited"_q)
		&& (snapshot.userId == userId)
		&& (snapshot.dialogId == dialogId)
		&& (snapshot.topicId == topicId)
		&& (snapshot.messageId == item->id.bare);
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
	snapshot.userId = object.value(u"userId"_q).toString().toLongLong();
	snapshot.dialogId = object.value(u"dialogId"_q).toString().toLongLong();
	snapshot.peerId = object.value(u"peerId"_q).toString().toLongLong();
	snapshot.fromId = object.value(u"fromId"_q).toString().toLongLong();
	snapshot.topicId = object.value(u"topicId"_q).toString().toLongLong();
	snapshot.messageId = object.value(u"messageId"_q).toInt();
	snapshot.date = object.value(u"date"_q).toInt();
	snapshot.editDate = object.value(u"editDate"_q).toInt();
	snapshot.text = object.value(u"text"_q).toString();
	return snapshot;
}

[[nodiscard]] bool MatchesPeer(
		not_null<PeerData*> peer,
		ID topicId,
		const MessageSnapshot &snapshot) {
	const auto userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	const auto dialogId = peer->id.value & PeerId::kChatTypeMask;
	return (snapshot.kind == u"deleted"_q)
		&& (snapshot.userId == userId)
		&& (snapshot.dialogId == dialogId)
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
	auto file = QFile(StoragePath());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return result;
	}
	while (!file.atEnd()) {
		const auto parsed = ParseSnapshotLine(file.readLine());
		if (!parsed || !MatchesItem(item, *parsed) || parsed->text.isEmpty()) {
			continue;
		}
		result.push_back(*parsed);
	}
	file.close();
	SortSnapshots(&result);
	if (totalLimit > 0 && int(result.size()) > totalLimit) {
		result.resize(totalLimit);
	}
	return result;
}

bool hasRevisions(not_null<HistoryItem*> item) {
	auto file = QFile(StoragePath());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return false;
	}
	while (!file.atEnd()) {
		const auto parsed = ParseSnapshotLine(file.readLine());
		if (parsed && MatchesItem(item, *parsed) && !parsed->text.isEmpty()) {
			file.close();
			return true;
		}
	}
	file.close();
	return false;
}

void addDeletedMessage(not_null<HistoryItem*> item) {
	AppendSnapshot(MapSnapshot(item, u"deleted"_q));
}

std::vector<MessageSnapshot> getDeletedMessages(
		not_null<PeerData*> peer,
		ID topicId,
		int totalLimit) {
	auto result = std::vector<MessageSnapshot>();
	auto file = QFile(StoragePath());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return result;
	}
	while (!file.atEnd()) {
		const auto parsed = ParseSnapshotLine(file.readLine());
		if (!parsed || !MatchesPeer(peer, topicId, *parsed) || parsed->text.isEmpty()) {
			continue;
		}
		result.push_back(*parsed);
	}
	file.close();
	SortSnapshots(&result);
	if (totalLimit > 0 && int(result.size()) > totalLimit) {
		result.resize(totalLimit);
	}
	return result;
}

bool hasDeletedMessages(
		not_null<PeerData*> peer,
		ID topicId) {
	auto file = QFile(StoragePath());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return false;
	}
	while (!file.atEnd()) {
		const auto parsed = ParseSnapshotLine(file.readLine());
		if (parsed && MatchesPeer(peer, topicId, *parsed) && !parsed->text.isEmpty()) {
			file.close();
			return true;
		}
	}
	file.close();
	return false;
}

} // namespace AyuMessages
