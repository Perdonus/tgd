/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QString>

namespace AyuMessages {

using ID = long long;

enum class MediaKind : int {
	None = 0,
	Photo,
	Video,
	Voice,
	VideoNote,
	Audio,
	Gif,
	Sticker,
	File,
	Link,
	Poll,
	Contact,
	Location,
	Call,
	Game,
	Invoice,
	TodoList,
	Story,
	Other,
};

struct MessageSnapshot {
	QString kind;
	ID userId = 0;
	ID dialogId = 0;
	ID peerId = 0;
	ID fromId = 0;
	ID topicId = 0;
	int messageId = 0;
	int date = 0;
	int editDate = 0;
	MediaKind mediaKind = MediaKind::None;
	QString text;
	QString mediaFileName;
	QString mediaMimeType;
	long long mediaSize = 0;
	int mediaDuration = 0;
	int mediaWidth = 0;
	int mediaHeight = 0;
};

} // namespace AyuMessages
