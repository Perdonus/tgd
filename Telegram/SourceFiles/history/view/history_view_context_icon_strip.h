/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/history_view_context_menu.h"

class QPoint;
class QAction;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace HistoryView {

void MarkContextMenuAction(QAction *action, const QString &id);
[[nodiscard]] std::vector<QString> ResolveContextIconStripActionIds(
	const ContextMenuSurfaceLayout &layout,
	const ContextMenuResolvedLayout &resolved);

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
