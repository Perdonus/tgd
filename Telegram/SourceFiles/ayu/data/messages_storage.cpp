/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/data/messages_storage.h"

#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

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

[[nodiscard]] MediaKind MediaKindFor(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return MediaKind::None;
	}
	if (media->photo()) {
		return MediaKind::Photo;
	}
	if (const auto document = media->document()) {
		if (document->isVoiceMessage()) {
			return MediaKind::Voice;
		} else if (document->isVideoMessage()) {
			return MediaKind::VideoNote;
		} else if (document->isVideoFile()) {
			return MediaKind::Video;
		} else if (document->isAudioFile() || document->isSong()) {
			return MediaKind::Audio;
		} else if (document->isAnimation()) {
			return MediaKind::Gif;
		} else if (document->sticker()) {
			return MediaKind::Sticker;
		}
		return MediaKind::File;
	}
	if (media->webpage()) {
		return MediaKind::Link;
	} else if (media->poll()) {
		return MediaKind::Poll;
	} else if (media->sharedContact()) {
		return MediaKind::Contact;
	} else if (media->location()) {
		return MediaKind::Location;
	} else if (media->call()) {
		return MediaKind::Call;
	} else if (media->game()) {
		return MediaKind::Game;
	} else if (media->invoice()) {
		return MediaKind::Invoice;
	} else if (media->todolist()) {
		return MediaKind::TodoList;
	} else if (media->storyId()) {
		return MediaKind::Story;
	}
	return MediaKind::Other;
}

void FillMediaMetadata(
		not_null<HistoryItem*> item,
		MessageSnapshot *snapshot) {
	const auto media = item->media();
	if (!media) {
		return;
	}
	if (const auto document = media->document()) {
		snapshot->mediaFileName = document->filename();
		snapshot->mediaMimeType = document->mimeString();
		snapshot->mediaSize = document->size;
		snapshot->mediaDuration = int(document->duration());
		snapshot->mediaWidth = document->dimensions.width();
		snapshot->mediaHeight = document->dimensions.height();
	}
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
	snapshot.fromId = item->from()->id.value;
	snapshot.topicId = item->topic() ? item->topicRootId().bare : 0;
	snapshot.messageId = item->id.bare;
	snapshot.date = item->date();
	snapshot.editDate = base::unixtime::now();
	const auto &originalText = item->originalText();
	snapshot.text = originalText.text;
	snapshot.textEntities = TextUtilities::ConvertEntitiesToTextTags(
		originalText.entities);
	snapshot.views = item->viewsCount();
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (forwarded->originalSender) {
			snapshot.fwdFromId = forwarded->originalSender->id.value;
			snapshot.fwdName = forwarded->originalSender->name();
		}
		if (!forwarded->originalPostAuthor.isEmpty()) {
			snapshot.fwdName = forwarded->originalPostAuthor;
		}
		snapshot.fwdDate = forwarded->originalDate;
	}
	if (const auto reply = item->Get<HistoryMessageReply>()) {
		snapshot.replyMessageId = reply->messageId().bare;
		snapshot.replyTopId = reply->topMessageId().bare;
	}
	snapshot.mediaKind = MediaKindFor(item);
	FillMediaMetadata(item, &snapshot);
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		snapshot.editDate = edited->date;
	}
	return snapshot;
}

void AppendSnapshot(const MessageSnapshot &snapshot) {
	if (snapshot.text.isEmpty() && snapshot.mediaKind == MediaKind::None) {
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
		{ u"media"_q, int(snapshot.mediaKind) },
		{ u"text"_q, snapshot.text },
		{ u"textEntities"_q, snapshot.textEntities },
		{ u"views"_q, snapshot.views },
		{ u"fwdFromId"_q, QString::number(snapshot.fwdFromId) },
		{ u"fwdName"_q, snapshot.fwdName },
		{ u"fwdDate"_q, snapshot.fwdDate },
		{ u"replyMessageId"_q, snapshot.replyMessageId },
		{ u"replyTopId"_q, snapshot.replyTopId },
		{ u"mediaFileName"_q, snapshot.mediaFileName },
		{ u"mediaMimeType"_q, snapshot.mediaMimeType },
		{ u"mediaSize"_q, QString::number(snapshot.mediaSize) },
		{ u"mediaDuration"_q, snapshot.mediaDuration },
		{ u"mediaWidth"_q, snapshot.mediaWidth },
		{ u"mediaHeight"_q, snapshot.mediaHeight },
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
	const auto mediaValue = object.value(u"media"_q).toInt(int(MediaKind::None));
	snapshot.mediaKind = (mediaValue >= int(MediaKind::None)
		&& mediaValue <= int(MediaKind::Other))
		? MediaKind(mediaValue)
		: MediaKind::None;
	snapshot.text = object.value(u"text"_q).toString();
	snapshot.textEntities = object.value(u"textEntities"_q).toString();
	snapshot.views = object.value(u"views"_q).toInt();
	snapshot.fwdFromId = object.value(u"fwdFromId"_q).toString().toLongLong();
	snapshot.fwdName = object.value(u"fwdName"_q).toString();
	snapshot.fwdDate = object.value(u"fwdDate"_q).toInt();
	snapshot.replyMessageId = object.value(u"replyMessageId"_q).toInt();
	snapshot.replyTopId = object.value(u"replyTopId"_q).toInt();
	snapshot.mediaFileName = object.value(u"mediaFileName"_q).toString();
	snapshot.mediaMimeType = object.value(u"mediaMimeType"_q).toString();
	snapshot.mediaSize = object.value(u"mediaSize"_q).toString().toLongLong();
	snapshot.mediaDuration = object.value(u"mediaDuration"_q).toInt();
	snapshot.mediaWidth = object.value(u"mediaWidth"_q).toInt();
	snapshot.mediaHeight = object.value(u"mediaHeight"_q).toInt();
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

template <typename Predicate>
void RewriteSnapshotsKeeping(Predicate &&keep) {
	const auto path = StoragePath();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return;
	}
	auto lines = std::vector<QByteArray>();
	while (!file.atEnd()) {
		const auto line = file.readLine();
		const auto parsed = ParseSnapshotLine(line);
		if (!parsed || keep(*parsed)) {
			lines.push_back(line);
		}
	}
	file.close();

	auto out = QFile(path);
	if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		return;
	}
	for (const auto &line : lines) {
		out.write(line);
	}
	out.close();
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
		if (!parsed
			|| !MatchesPeer(peer, topicId, *parsed)
			|| (parsed->text.isEmpty()
				&& parsed->mediaKind == MediaKind::None)) {
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
		if (parsed
			&& MatchesPeer(peer, topicId, *parsed)
			&& (!parsed->text.isEmpty()
				|| parsed->mediaKind != MediaKind::None)) {
			file.close();
			return true;
		}
	}
	file.close();
	return false;
}

TextWithEntities SnapshotText(const MessageSnapshot &snapshot) {
	return TextWithEntities{
		snapshot.text,
		TextUtilities::ConvertTextTagsToEntities(snapshot.textEntities),
	};
}

void clearDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	RewriteSnapshotsKeeping([&](const MessageSnapshot &snapshot) {
		return !MatchesPeer(peer, topicId, snapshot);
	});
}

} // namespace AyuMessages
