/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "history/view/history_view_element.h"
#include "ui/style/style_core.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <memory>
#include <vector>

namespace Data {
struct ReactionId;
} // namespace Data


namespace Main {
class Session;
class SessionShow;
} // namespace Main

namespace Ui {
class PopupMenu;
enum class ReportReason;
} // namespace Ui

namespace Window {
class SessionNavigation;
class SessionController;
} // namespace Main

namespace HistoryView {

enum class Context : char;
enum class PointState : char;
class ListWidget;
class Element;
struct SelectedItem;
using SelectedItems = std::vector<SelectedItem>;

enum class ContextMenuSurface : char {
	Message,
	Selection,
};

enum class ContextMenuLane : char {
	Menu,
	Strip,
};

struct ContextMenuLayoutEntry {
	QString id;
	bool visible = true;
	// Keep old layout files compatible: custom separators are still identified
	// by their persisted custom_separator_* ids even before runtime normalization.
	bool separator = id.startsWith(QStringLiteral("custom_separator_"));

	[[nodiscard]] QString normalizedId() const {
		return id.trimmed();
	}
	[[nodiscard]] QString duplicateKey() const {
		return separator ? QString() : normalizedId();
	}
	[[nodiscard]] bool participatesInUnifiedLayout() const {
		return visible && !duplicateKey().isEmpty();
	}
	[[nodiscard]] bool conflictsWith(const ContextMenuLayoutEntry &other) const {
		const auto key = duplicateKey();
		return !key.isEmpty() && (key == other.duplicateKey());
	}
};

struct ContextMenuSurfaceLayout {
	std::vector<ContextMenuLayoutEntry> menu;
	std::vector<ContextMenuLayoutEntry> strip;

	[[nodiscard]] bool hasVisibleUnifiedDuplicate(
			const QString &id,
			ContextMenuLane lane) const {
		const auto normalized = id.trimmed();
		if (normalized.isEmpty()) {
			return false;
		}
		const auto &entries = (lane == ContextMenuLane::Strip) ? strip : menu;
		for (const auto &entry : entries) {
			if (entry.participatesInUnifiedLayout()
				&& (entry.normalizedId() == normalized)) {
				return true;
			}
		}
		return false;
	}
	[[nodiscard]] bool hasCrossLaneUnifiedDuplicates() const {
		for (const auto &entry : menu) {
			if (!entry.participatesInUnifiedLayout()) {
				continue;
			}
			if (hasVisibleUnifiedDuplicate(entry.id, ContextMenuLane::Strip)) {
				return true;
			}
		}
		return false;
	}
};

struct ContextMenuCustomizationLayout {
	int version = 1;
	ContextMenuSurfaceLayout message;
	ContextMenuSurfaceLayout selection;
};

struct ContextMenuResolvedAction {
	QString id;
	QString text;
	const style::icon *icon = nullptr;
	std::shared_ptr<Fn<void()>> trigger;
	bool stripEligible = false;
};

struct ContextMenuResolvedLayout {
	ContextMenuSurface surface = ContextMenuSurface::Message;
	std::vector<ContextMenuResolvedAction> actions;
};

struct ContextMenuRequest {
	explicit ContextMenuRequest(
		not_null<Window::SessionNavigation*> navigation);

	const not_null<Window::SessionNavigation*> navigation;
	ClickHandlerPtr link;
	Element *view = nullptr;
	HistoryItem *item = nullptr;
	SelectedItems selectedItems;
	TextForMimeData selectedText;
	SelectedQuote quote;
	bool overSelection = false;
	PointState pointState = PointState();
};

[[nodiscard]] ContextMenuCustomizationLayout
DefaultContextMenuCustomizationLayout();
[[nodiscard]] QString ContextMenuCustomizationLayoutPath();
[[nodiscard]] ContextMenuCustomizationLayout
LoadContextMenuCustomizationLayout(bool *changed = nullptr);
[[nodiscard]] bool SaveContextMenuCustomizationLayout(
	const ContextMenuCustomizationLayout &layout);
[[nodiscard]] bool ResetContextMenuCustomizationLayout();
[[nodiscard]] ContextMenuCustomizationLayout
ParseContextMenuCustomizationLayout(const QJsonObject &json);
[[nodiscard]] QJsonObject SerializeContextMenuCustomizationLayout(
	const ContextMenuCustomizationLayout &layout);
[[nodiscard]] const ContextMenuSurfaceLayout &LookupContextMenuSurfaceLayout(
	const ContextMenuCustomizationLayout &layout,
	ContextMenuSurface surface);

base::unique_qptr<Ui::PopupMenu> FillContextMenu(
	not_null<ListWidget*> list,
	const ContextMenuRequest &request,
	ContextMenuResolvedLayout *resolved = nullptr);

void CopyPostLink(
	not_null<Window::SessionController*> controller,
	FullMsgId itemId,
	Context context,
	std::optional<TimeId> videoTimestamp = {});
void CopyPostLink(
	std::shared_ptr<Main::SessionShow> show,
	FullMsgId itemId,
	Context context,
	std::optional<TimeId> videoTimestamp = {});
void CopyStoryLink(
	std::shared_ptr<Main::SessionShow> show,
	FullStoryId storyId);
void AddPollActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<PollData*> poll,
	not_null<HistoryItem*> item,
	Context context,
	not_null<Window::SessionController*> controller);
void AddSaveSoundForNotifications(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	not_null<DocumentData*> document,
	not_null<Window::SessionController*> controller);
void AddWhoReactedAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<QWidget*> context,
	not_null<HistoryItem*> item,
	not_null<Window::SessionController*> controller);
void MaybeAddWhenEditedForwardedAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	not_null<Window::SessionController*> controller);
void ShowEditHistoryBox(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);
void ShowWhoReactedMenu(
	not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
	QPoint position,
	not_null<QWidget*> context,
	not_null<HistoryItem*> item,
	const Data::ReactionId &id,
	not_null<Window::SessionController*> controller,
	rpl::lifetime &lifetime);
void ShowTagInListMenu(
	not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
	QPoint position,
	not_null<QWidget*> context,
	const Data::ReactionId &id,
	not_null<Window::SessionController*> controller);
void AddCopyFilename(
	not_null<Ui::PopupMenu*> menu,
	not_null<DocumentData*> document,
	Fn<bool()> showCopyRestrictionForSelected);

enum class EmojiPacksSource {
	Message,
	Reaction,
	Reactions,
	Tag,
};
[[nodiscard]] std::vector<StickerSetIdentifier> CollectEmojiPacks(
	not_null<HistoryItem*> item,
	EmojiPacksSource source);
void AddEmojiPacksAction(
	not_null<Ui::PopupMenu*> menu,
	std::vector<StickerSetIdentifier> packIds,
	EmojiPacksSource source,
	not_null<Window::SessionController*> controller);
void AddEmojiPacksAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	EmojiPacksSource source,
	not_null<Window::SessionController*> controller);
void AddSelectRestrictionAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item,
	bool addIcon);

[[nodiscard]] TextWithEntities TransribedText(not_null<HistoryItem*> item);

[[nodiscard]] bool ItemHasTtl(HistoryItem *item);

} // namespace HistoryView
