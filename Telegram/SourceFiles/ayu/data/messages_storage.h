/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ayu/data/entities.h"

#include <vector>

class HistoryItem;

namespace AyuMessages {

void addEditedMessage(not_null<HistoryItem*> item);
std::vector<MessageSnapshot> getEditedMessages(
	not_null<HistoryItem*> item,
	int totalLimit = 50);
bool hasRevisions(not_null<HistoryItem*> item);
void addDeletedMessage(not_null<HistoryItem*> item);

} // namespace AyuMessages
