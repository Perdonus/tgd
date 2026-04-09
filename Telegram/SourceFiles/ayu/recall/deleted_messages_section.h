/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>

namespace Data {
class Thread;
} // namespace Data

namespace Window {
class SectionMemento;
} // namespace Window

namespace AyuRecall {

[[nodiscard]] std::shared_ptr<Window::SectionMemento> MakeDeletedMessagesSection(
	not_null<Data::Thread*> thread);

} // namespace AyuRecall
