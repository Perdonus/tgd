/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/history_view_context_menu.h"

class QPoint;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace HistoryView {

enum class AttachContextIconStripResult {
	Skipped,
	Failed,
	Attached,
};

AttachContextIconStripResult AttachContextIconStripToMenu(
	not_null<Ui::PopupMenu*> menu,
	QPoint desiredPosition,
	const ContextMenuSurfaceLayout &layout,
	const ContextMenuResolvedLayout &resolved);

} // namespace HistoryView
