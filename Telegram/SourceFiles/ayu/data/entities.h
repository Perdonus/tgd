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
	QString text;
};

} // namespace AyuMessages
