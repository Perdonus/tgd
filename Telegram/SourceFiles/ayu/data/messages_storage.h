/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ayu/data/entities.h"

class HistoryItem;

namespace AyuMessages {

void addEditedMessage(not_null<HistoryItem*> item);
void addDeletedMessage(not_null<HistoryItem*> item);

} // namespace AyuMessages
