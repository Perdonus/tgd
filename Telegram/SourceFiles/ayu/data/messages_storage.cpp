/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/data/messages_storage.h"

#include "base/unixtime.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

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

} // namespace

void addEditedMessage(not_null<HistoryItem*> item) {
	AppendSnapshot(MapSnapshot(item, u"edited"_q));
}

void addDeletedMessage(not_null<HistoryItem*> item) {
	AppendSnapshot(MapSnapshot(item, u"deleted"_q));
}

} // namespace AyuMessages
