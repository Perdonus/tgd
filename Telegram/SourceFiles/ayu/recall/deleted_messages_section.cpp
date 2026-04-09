/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/recall/deleted_messages_section.h"

#include "ayu/data/messages_storage.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_top_bar_widget.h"
#include "lang/lang_instance.h"
#include "main/main_session.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/vertical_layout.h"
#include "window/section_memento.h"
#include "window/section_widget.h"
#include "window/window_adaptive.h"
#include "window/window_session_controller.h"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace AyuRecall {
namespace {

constexpr auto kRowOuterLeft = 16;
constexpr auto kRowOuterRight = 16;
constexpr auto kRowOuterTop = 6;
constexpr auto kRowOuterBottom = 6;
constexpr auto kGroupedOuterSkip = 1;
constexpr auto kBubblePadding = QMargins(14, 10, 14, 10);
constexpr auto kBubbleInnerSkip = 4;
constexpr auto kBubbleMinWidth = 120;
constexpr auto kBubbleWidthFactor = 0.72;
constexpr auto kClusterGapSeconds = 15 * 60;
constexpr auto kIntroCardPadding = QMargins(18, 14, 18, 14);
constexpr auto kBubbleHeaderSkip = 5;
constexpr auto kChipHorizontalPadding = 10;
constexpr auto kChipVerticalPadding = 4;
constexpr auto kChipSpacing = 6;
constexpr auto kDayBadgePadding = QMargins(14, 8, 14, 8);
constexpr auto kScopeBarPadding = QMargins(16, 10, 16, 10);
constexpr auto kScopeBarSkip = 8;
constexpr auto kFooterBarPadding = QMargins(16, 10, 16, 10);
constexpr auto kFooterBarSkip = 8;
constexpr auto kJumpProbeRadius = 512;

struct DeletedMessagesSectionState {
	int scrollTop = -1;
	QString searchQuery;
	int anchorMessageId = 0;
	int anchorTimestamp = 0;
	int anchorOffset = 0;
};

struct DeletedMessageVisualGroup {
	bool groupedWithPrevious = false;
	bool groupedWithNext = false;
};

struct DeletedMessagesDaySummary {
	int count = 0;
	int firstTimestamp = 0;
	int lastTimestamp = 0;
};

enum class DeletedSnapshotKind {
	Text,
	Media,
	Service,
	Placeholder,
};

enum class DeletedJumpState {
	LocalOnly,
	ExactLoaded,
	NearestLoaded,
	RemoteLookup,
};

enum class ScopeChipTone {
	Accent,
	Info,
	Muted,
};

struct ScopeChipData {
	QString text;
	ScopeChipTone tone = ScopeChipTone::Muted;
};

struct DeletedJumpTarget {
	DeletedJumpState state = DeletedJumpState::LocalOnly;
	HistoryItem *item = nullptr;
	int requestedMessageId = 0;
	int openedMessageId = 0;
};

struct DeletedMessagePresentation {
	QString state;
	QString text;
	QString meta;
	QString tooltip;
	DeletedSnapshotKind kind = DeletedSnapshotKind::Text;
	DeletedJumpState jumpState = DeletedJumpState::LocalOnly;
};

[[nodiscard]] bool RussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString UiText(const char *en, const char *ru) {
	return RussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString TitleText(int visibleCount, int totalCount) {
	const auto base = UiText("Deleted Messages", "Удалённые сообщения");
	if (totalCount <= 0) {
		return base;
	}
	const auto counter = (visibleCount == totalCount)
		? QString::number(totalCount)
		: QString::number(visibleCount) + u"/"_q + QString::number(totalCount);
	return base + u" · "_q + counter;
}

[[nodiscard]] QString EmptyText(const QString &query) {
	if (!query.isEmpty()) {
		return UiText(
			"Nothing was found in the saved deleted messages for this chat.",
			"По этому запросу среди сохранённых удалённых сообщений ничего не найдено.");
	}
	return UiText(
		"No locally saved deleted messages were found for this chat.",
		"Для этого чата не найдено локально сохранённых удалённых сообщений.");
}

[[nodiscard]] QString OpenHint() {
	return UiText(
		"Open this place in the live chat",
		"Открыть это место в живом чате");
}

[[nodiscard]] QString OpenAttemptHint(not_null<Data::Thread*> thread) {
	return thread->asTopic()
		? UiText(
			"Trying to open this place in the topic.",
			"Пробую открыть это место в теме.")
		: UiText(
			"Trying to open this place in the chat.",
			"Пробую открыть это место в чате.");
}

[[nodiscard]] QString LocalCopyHint() {
	return UiText(
		"Only the locally saved copy is available.",
		"Доступна только локально сохранённая копия.");
}

[[nodiscard]] QString DeletedMessagePlaceholder() {
	return UiText(
		"No text was saved for this deleted message.",
		"Для этого удалённого сообщения текст не сохранился.");
}

[[nodiscard]] QString ScopeHeadline() {
	return UiText("Deleted Messages", "Удалённые сообщения");
}

[[nodiscard]] QString ScopeSubtitle(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return UiText(
			"Topic %1 in %2",
			"Тема %1 в %2").arg(topic->title(), thread->peer()->name());
	}
	return UiText(
		"Saved deleted messages from %1",
		"Сохранённые удалённые сообщения из %1").arg(thread->peer()->name());
}

[[nodiscard]] QString RangeLabel(int oldestTimestamp, int newestTimestamp) {
	if (oldestTimestamp <= 0 || newestTimestamp <= 0) {
		return QString();
	}
	const auto oldest = QDateTime::fromSecsSinceEpoch(
		oldestTimestamp).toLocalTime().date();
	const auto newest = QDateTime::fromSecsSinceEpoch(
		newestTimestamp).toLocalTime().date();
	if (oldest == newest) {
		return RussianUi()
			? oldest.toString(u"d MMMM yyyy"_q)
			: QLocale().toString(oldest, QLocale::LongFormat);
	}
	const auto left = RussianUi()
		? oldest.toString(oldest.year() == newest.year()
			? u"d MMMM"_q
			: u"d MMMM yyyy"_q)
		: QLocale().toString(oldest, QLocale::ShortFormat);
	const auto right = RussianUi()
		? newest.toString(u"d MMMM yyyy"_q)
		: QLocale().toString(newest, QLocale::ShortFormat);
	return left + u" — "_q + right;
}

[[nodiscard]] QString ScopeDetails(
		int visibleCount,
		int totalCount,
		int oldestTimestamp,
		int newestTimestamp) {
	const auto counter = (totalCount <= 0)
		? UiText("No saved messages yet", "Пока нет сохранённых сообщений")
		: (visibleCount == totalCount)
		? UiText("%1 saved locally", "%1 сохранено локально")
			.arg(totalCount)
		: UiText("Showing %1 of %2 saved locally", "Показано %1 из %2 локально сохранённых")
			.arg(visibleCount)
			.arg(totalCount);
	const auto range = RangeLabel(oldestTimestamp, newestTimestamp);
	return range.isEmpty() ? counter : (counter + u" · "_q + range);
}

[[nodiscard]] QString ScopeHint(not_null<Data::Thread*> thread) {
	return thread->asTopic()
		? UiText(
			"Only this topic is shown here. Tap a bubble to open its place in the real topic.",
			"Здесь показаны только сообщения из этой темы. Нажмите на пузырь, чтобы открыть это место в реальной теме.")
		: UiText(
			"Only locally saved deleted messages from this chat are shown here. Tap a bubble to open its place in the real chat.",
			"Здесь показаны только локально сохранённые удалённые сообщения из этого чата. Нажмите на пузырь, чтобы открыть это место в реальном чате.");
}

[[nodiscard]] QString ScopeTypeLabel(not_null<Data::Thread*> thread) {
	const auto peer = thread->peer();
	if (thread->asTopic()) {
		return UiText("Topic", "Тема");
	} else if (peer->isSelf()) {
		return UiText("Saved Messages", "Избранное");
	} else if (peer->isUser()) {
		return UiText("Private chat", "Личный чат");
	} else if (peer->isForum()) {
		return UiText("Forum", "Форум");
	} else if (peer->isMegagroup() || peer->isChat()) {
		return UiText("Group", "Группа");
	} else if (peer->isBroadcast()) {
		return UiText("Channel", "Канал");
	}
	return UiText("Chat", "Чат");
}

[[nodiscard]] QString ScopePrimaryChip(not_null<Data::Thread*> thread) {
	return thread->peer()->name();
}

[[nodiscard]] QString ScopeSecondaryChip(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return UiText("Topic · %1", "Тема · %1").arg(topic->title());
	}
	return ScopeTypeLabel(thread);
}

[[nodiscard]] QString SearchScopeDetails(
		const QString &query,
		int visibleCount,
		int totalCount) {
	if (query.isEmpty()) {
		return QString();
	}
	const auto shown = (visibleCount == totalCount)
		? UiText("%1 matches", "%1 совпадений").arg(visibleCount)
		: UiText("%1 of %2 matches", "%1 из %2 совпадений")
			.arg(visibleCount)
			.arg(totalCount);
	return UiText("Filter: “%1” · %2", "Фильтр: «%1» · %2").arg(query, shown);
}

[[nodiscard]] QString ScopeStateChip() {
	return UiText("Local archive", "Локальный архив");
}

[[nodiscard]] QString ScopeHistoryChip(bool searchActive) {
	return searchActive
		? UiText("Filter active", "Фильтр включён")
		: UiText("Newest at bottom", "Новые внизу");
}

[[nodiscard]] std::vector<ScopeChipData> IntroScopeChips(
		not_null<Data::Thread*> thread,
		const QString &searchQuery) {
	return {
		{ ScopePrimaryChip(thread), ScopeChipTone::Accent },
		{ ScopeSecondaryChip(thread), ScopeChipTone::Info },
		{ ScopeStateChip(), ScopeChipTone::Info },
		{ ScopeHistoryChip(!searchQuery.isEmpty()), ScopeChipTone::Muted },
	};
}

[[nodiscard]] std::vector<ScopeChipData> HeaderScopeChips(
		not_null<Data::Thread*> thread,
		const QString &searchQuery) {
	return {
		{ ScopeTypeLabel(thread), ScopeChipTone::Info },
		{ ScopeStateChip(), ScopeChipTone::Accent },
		{ ScopeHistoryChip(!searchQuery.isEmpty()), ScopeChipTone::Muted },
	};
}

[[nodiscard]] bool MatchesSnapshotText(
		const QString &value,
		const char *en,
		const char *ru) {
	return (value == QString::fromUtf8(en))
		|| (value == QString::fromUtf8(ru));
}

[[nodiscard]] bool LooksLikeMediaFallback(const QString &text) {
	return MatchesSnapshotText(text, "Video message", "Видеосообщение")
		|| MatchesSnapshotText(text, "Voice message", "Голосовое сообщение")
		|| MatchesSnapshotText(text, "Music", "Музыка")
		|| MatchesSnapshotText(text, "GIF", "GIF")
		|| MatchesSnapshotText(text, "Video", "Видео")
		|| MatchesSnapshotText(text, "Photo or video", "Фото или видео")
		|| MatchesSnapshotText(text, "Photo", "Фотография")
		|| MatchesSnapshotText(text, "File", "Файл")
		|| MatchesSnapshotText(text, "Link", "Ссылка")
		|| MatchesSnapshotText(text, "Media message", "Медиасообщение");
}

[[nodiscard]] bool LooksLikeServiceFallback(const QString &text) {
	return MatchesSnapshotText(text, "Pinned message", "Закреплённое сообщение")
		|| MatchesSnapshotText(text, "Service message", "Служебное сообщение");
}

[[nodiscard]] DeletedSnapshotKind SnapshotKind(
		const AyuMessages::MessageSnapshot &snapshot) {
	const auto text = snapshot.text.trimmed();
	if (text.isEmpty()
		|| MatchesSnapshotText(
			text,
			"Saved deleted message",
			"Сохранённое удалённое сообщение")) {
		return DeletedSnapshotKind::Placeholder;
	} else if (LooksLikeServiceFallback(text)) {
		return DeletedSnapshotKind::Service;
	} else if (LooksLikeMediaFallback(text)) {
		return DeletedSnapshotKind::Media;
	}
	return DeletedSnapshotKind::Text;
}

[[nodiscard]] QString DeletedStateLabel(const AyuMessages::MessageSnapshot &snapshot) {
	switch (SnapshotKind(snapshot)) {
	case DeletedSnapshotKind::Placeholder:
		return UiText("Local snapshot", "Локальный снимок");
	case DeletedSnapshotKind::Service:
		return UiText("Deleted service event", "Удалённое служебное событие");
	case DeletedSnapshotKind::Media:
		return UiText("Deleted media", "Удалённое медиа");
	case DeletedSnapshotKind::Text:
		return UiText("Deleted message", "Удалённое сообщение");
	}
	Unexpected("Snapshot kind.");
}

[[nodiscard]] QString FallbackBodyText(
		const AyuMessages::MessageSnapshot &snapshot) {
	const auto text = snapshot.text.trimmed();
	if (text.isEmpty()
		|| MatchesSnapshotText(
			text,
			"Saved deleted message",
			"Сохранённое удалённое сообщение")) {
		return DeletedMessagePlaceholder();
	} else if (MatchesSnapshotText(text, "Video message", "Видеосообщение")) {
		return UiText("Video message without text", "Видеосообщение без текста");
	} else if (MatchesSnapshotText(text, "Voice message", "Голосовое сообщение")) {
		return UiText("Voice message without text", "Голосовое сообщение без текста");
	} else if (MatchesSnapshotText(text, "Music", "Музыка")) {
		return UiText("Audio file without caption", "Аудиофайл без подписи");
	} else if (MatchesSnapshotText(text, "GIF", "GIF")) {
		return UiText("GIF without caption", "GIF без подписи");
	} else if (MatchesSnapshotText(text, "Video", "Видео")) {
		return UiText("Video without caption", "Видео без подписи");
	} else if (MatchesSnapshotText(text, "Photo or video", "Фото или видео")) {
		return UiText("Media without caption", "Медиа без подписи");
	} else if (MatchesSnapshotText(text, "Photo", "Фотография")) {
		return UiText("Photo without caption", "Фотография без подписи");
	} else if (MatchesSnapshotText(text, "File", "Файл")) {
		return UiText("File attachment without text", "Файл без текста");
	} else if (MatchesSnapshotText(text, "Link", "Ссылка")) {
		return UiText("Link preview without text", "Ссылка без текста");
	} else if (MatchesSnapshotText(text, "Pinned message", "Закреплённое сообщение")) {
		return UiText("Pinned message event", "Событие закрепления сообщения");
	} else if (MatchesSnapshotText(text, "Service message", "Служебное сообщение")) {
		return UiText("Service event snapshot", "Снимок служебного события");
	}
	return text;
}

[[nodiscard]] int TopicIdFor(not_null<Data::Thread*> thread) {
	const auto topic = thread->asTopic();
	return topic ? topic->rootId().bare : 0;
}

[[nodiscard]] QString ThreadStateKey(not_null<Data::Thread*> thread) {
	return QString::number(SerializePeerId(thread->peer()->id))
		+ u':'
		+ QString::number(TopicIdFor(thread));
}

[[nodiscard]] std::map<QString, DeletedMessagesSectionState> &RememberedStates() {
	static auto states = std::map<QString, DeletedMessagesSectionState>();
	return states;
}

[[nodiscard]] DeletedMessagesSectionState RememberedState(
		not_null<Data::Thread*> thread) {
	const auto &states = RememberedStates();
	const auto key = ThreadStateKey(thread);
	const auto i = states.find(key);
	return (i != states.end())
		? i->second
		: DeletedMessagesSectionState();
}

void SaveRememberedState(
		not_null<Data::Thread*> thread,
		const DeletedMessagesSectionState &state) {
	RememberedStates()[ThreadStateKey(thread)] = state;
}

[[nodiscard]] int DisplayTimestamp(const AyuMessages::MessageSnapshot &snapshot) {
	return snapshot.date ? snapshot.date : snapshot.editDate;
}

[[nodiscard]] QString DayLabel(int timestamp) {
	const auto date = QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime().date();
	return RussianUi()
		? date.toString(u"dd MMMM yyyy"_q)
		: QLocale().toString(date, QLocale::LongFormat);
}

[[nodiscard]] QString RelativeDayLabel(int timestamp) {
	if (timestamp <= 0) {
		return QString();
	}
	const auto date = QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime().date();
	const auto today = QDate::currentDate();
	if (date == today) {
		return UiText("Today", "Сегодня");
	} else if (date == today.addDays(-1)) {
		return UiText("Yesterday", "Вчера");
	}
	return DayLabel(timestamp);
}

[[nodiscard]] QString TimeLabel(int timestamp) {
	return QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime().toString(u"HH:mm"_q);
}

[[nodiscard]] QString DateTimeLabel(int timestamp) {
	if (timestamp <= 0) {
		return QString();
	}
	const auto moment = QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime();
	return RussianUi()
		? moment.toString(u"d MMMM yyyy, HH:mm"_q)
		: QLocale().toString(moment, QLocale::LongFormat);
}

[[nodiscard]] int DayKey(int timestamp) {
	if (timestamp <= 0) {
		return 0;
	}
	return QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime().date().toJulianDay();
}

[[nodiscard]] QString DayTimeRangeLabel(
		int firstTimestamp,
		int lastTimestamp) {
	if (firstTimestamp <= 0 && lastTimestamp <= 0) {
		return QString();
	}
	if (firstTimestamp <= 0 || lastTimestamp <= 0 || firstTimestamp == lastTimestamp) {
		return TimeLabel(std::max(firstTimestamp, lastTimestamp));
	}
	return TimeLabel(firstTimestamp) + u"–"_q + TimeLabel(lastTimestamp);
}

[[nodiscard]] std::map<int, DeletedMessagesDaySummary> BuildDaySummaries(
		const std::vector<AyuMessages::MessageSnapshot> &messages) {
	auto result = std::map<int, DeletedMessagesDaySummary>();
	for (const auto &message : messages) {
		const auto timestamp = DisplayTimestamp(message);
		const auto key = DayKey(timestamp);
		auto &summary = result[key];
		++summary.count;
		if (!summary.firstTimestamp || timestamp < summary.firstTimestamp) {
			summary.firstTimestamp = timestamp;
		}
		if (!summary.lastTimestamp || timestamp > summary.lastTimestamp) {
			summary.lastTimestamp = timestamp;
		}
	}
	return result;
}

[[nodiscard]] QString DaySummaryDetails(
		const DeletedMessagesDaySummary &summary,
		bool searchMode) {
	const auto counter = (summary.count <= 0)
		? QString()
		: searchMode
		? UiText("%1 matches", "%1 совпадений").arg(summary.count)
		: UiText("%1 local copies", "%1 локальных копий").arg(summary.count);
	const auto range = DayTimeRangeLabel(summary.firstTimestamp, summary.lastTimestamp);
	if (counter.isEmpty()) {
		return range;
	} else if (range.isEmpty()) {
		return counter;
	}
	return counter + u" · "_q + range;
}

[[nodiscard]] QString DayHeadlineText(int timestamp) {
	const auto relative = RelativeDayLabel(timestamp);
	const auto full = DayLabel(timestamp);
	if (relative.isEmpty() || relative == full) {
		return full;
	}
	return relative + u" · "_q + full;
}

[[nodiscard]] QString HistoryStatusText(
		const DeletedMessagesDaySummary *summary,
		int timestamp,
		bool searchMode,
		bool atLatest,
		bool hasMessages,
		const QString &query) {
	if (!hasMessages) {
		return query.isEmpty()
			? UiText(
				"This local archive is empty for the current chat or topic.",
				"Для текущего чата или темы этот локальный архив пуст.")
			: UiText(
				"No local deleted messages match this filter.",
				"Ни одно локально сохранённое удалённое сообщение не подходит под этот фильтр.");
	}
	if (!summary || timestamp <= 0) {
		return searchMode
			? UiText(
				"Showing filtered local deleted history.",
				"Показана отфильтрованная локальная история удалений.")
			: UiText(
				"Newest locally saved deletions are at the bottom.",
				"Новые локально сохранённые удаления находятся внизу.");
	}
	const auto day = DayHeadlineText(timestamp);
	const auto details = DaySummaryDetails(*summary, searchMode);
	const auto prefix = atLatest
		? UiText("Latest", "Последние")
		: UiText("Browsing", "Просмотр");
	return details.isEmpty()
		? (prefix + u" · "_q + day)
		: (prefix + u" · "_q + day + u" · "_q + details);
}

[[nodiscard]] QString ResolveSenderName(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread);

[[nodiscard]] std::vector<AyuMessages::MessageSnapshot> LoadMessages(
		not_null<Data::Thread*> thread) {
	auto result = AyuMessages::getDeletedMessages(
		thread->peer(),
		TopicIdFor(thread),
		0);
	std::sort(
		result.begin(),
		result.end(),
		[](const AyuMessages::MessageSnapshot &a, const AyuMessages::MessageSnapshot &b) {
			if (a.date != b.date) {
				return a.date < b.date;
			}
			if (a.messageId != b.messageId) {
				return a.messageId < b.messageId;
			}
			return a.editDate < b.editDate;
		});
	return result;
}

[[nodiscard]] bool MatchesSearchQuery(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread,
		const QString &query) {
	if (query.isEmpty()) {
		return true;
	}
	const auto sender = ResolveSenderName(snapshot, thread);
	const auto timestamp = DisplayTimestamp(snapshot);
	return snapshot.text.contains(query, Qt::CaseInsensitive)
		|| snapshot.senderName.contains(query, Qt::CaseInsensitive)
		|| sender.contains(query, Qt::CaseInsensitive)
		|| DayLabel(timestamp).contains(query, Qt::CaseInsensitive)
		|| TimeLabel(timestamp).contains(query, Qt::CaseInsensitive)
		|| QString::number(snapshot.messageId).contains(query);
}

[[nodiscard]] bool IsOutgoing(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread) {
	const auto selfBare = thread->session().userId().bare;
	if (snapshot.senderSerialized) {
		return DeserializePeerId(snapshot.senderSerialized)
			== peerFromUser(thread->session().userId());
	}
	return snapshot.fromId == selfBare;
}

[[nodiscard]] bool ItemMatchesThread(
		not_null<HistoryItem*> item,
		not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return (item->history()->migrateToOrMe()
				== thread->owningHistory()->migrateToOrMe())
			&& item->inThread(topic->rootId());
	} else if (const auto sublist = thread->asSublist()) {
		return item->savedSublist() == sublist;
	}
	return (item->history()->migrateToOrMe()
			== thread->owningHistory()->migrateToOrMe())
		&& !item->topicRootId()
		&& (item->savedSublist() == nullptr);
}

[[nodiscard]] std::vector<PeerId> JumpSearchPeers(
		not_null<Data::Thread*> thread) {
	auto result = std::vector<PeerId>();
	const auto push = [&](PeerId peerId) {
		if (peerId && (std::find(result.begin(), result.end(), peerId) == result.end())) {
			result.push_back(peerId);
		}
	};
	push(thread->peer()->id);
	if (!thread->asTopic() && !thread->asSublist()) {
		if (const auto previous = thread->peer()->migrateFrom()) {
			push(previous->id);
		}
		if (const auto next = thread->peer()->migrateTo()) {
			push(next->id);
		}
	}
	return result;
}

[[nodiscard]] HistoryItem *LoadedItemForMessageId(
		not_null<Data::Thread*> thread,
		int messageId) {
	if (messageId <= 0) {
		return nullptr;
	}
	auto &data = thread->session().data();
	for (const auto peerId : JumpSearchPeers(thread)) {
		if (const auto item = data.message(FullMsgId(peerId, messageId));
			item && ItemMatchesThread(item, thread)) {
			return item;
		}
	}
	return nullptr;
}

[[nodiscard]] DeletedJumpTarget ResolveJumpTarget(
		not_null<Data::Thread*> thread,
		int messageId) {
	if (messageId <= 0) {
		return {
			.state = DeletedJumpState::LocalOnly,
			.item = nullptr,
			.requestedMessageId = messageId,
			.openedMessageId = 0,
		};
	}
	if (const auto exact = LoadedItemForMessageId(thread, messageId)) {
		return {
			.state = DeletedJumpState::ExactLoaded,
			.item = exact,
			.requestedMessageId = messageId,
			.openedMessageId = exact->id.bare,
		};
	}
	for (auto step = 1; step <= kJumpProbeRadius; ++step) {
		const auto before = messageId - step;
		if (const auto item = LoadedItemForMessageId(thread, before)) {
			return {
				.state = DeletedJumpState::NearestLoaded,
				.item = item,
				.requestedMessageId = messageId,
				.openedMessageId = item->id.bare,
			};
		}
		const auto after = messageId + step;
		if (const auto item = LoadedItemForMessageId(thread, after)) {
			return {
				.state = DeletedJumpState::NearestLoaded,
				.item = item,
				.requestedMessageId = messageId,
				.openedMessageId = item->id.bare,
			};
		}
	}
	return {
		.state = DeletedJumpState::RemoteLookup,
		.item = nullptr,
		.requestedMessageId = messageId,
		.openedMessageId = messageId,
	};
}

[[nodiscard]] QString SenderGroupingKey(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread) {
	if (IsOutgoing(snapshot, thread)) {
		return u"self"_q;
	}
	if (snapshot.senderSerialized) {
		return u"peer:"_q + QString::number(snapshot.senderSerialized);
	}
	if (snapshot.fromId) {
		return u"legacy:"_q + QString::number(snapshot.fromId);
	}
	if (!snapshot.senderName.isEmpty()) {
		return u"name:"_q + snapshot.senderName;
	}
	return u"thread:"_q + thread->peer()->name();
}

[[nodiscard]] bool SameVisualCluster(
		const AyuMessages::MessageSnapshot &a,
		const AyuMessages::MessageSnapshot &b,
		not_null<Data::Thread*> thread) {
	if (IsOutgoing(a, thread) != IsOutgoing(b, thread)) {
		return false;
	}
	if (SenderGroupingKey(a, thread) != SenderGroupingKey(b, thread)) {
		return false;
	}
	const auto left = QDateTime::fromSecsSinceEpoch(
		DisplayTimestamp(a)).toLocalTime().date();
	const auto right = QDateTime::fromSecsSinceEpoch(
		DisplayTimestamp(b)).toLocalTime().date();
	if (left != right) {
		return false;
	}
	return std::abs(DisplayTimestamp(b) - DisplayTimestamp(a)) <= kClusterGapSeconds;
}

[[nodiscard]] QString JumpStateText(DeletedJumpState state) {
	switch (state) {
	case DeletedJumpState::LocalOnly:
		return UiText("local only", "только локально");
	case DeletedJumpState::ExactLoaded:
		return UiText("exact live spot", "точное место");
	case DeletedJumpState::NearestLoaded:
		return UiText("nearest live spot", "ближайшее место");
	case DeletedJumpState::RemoteLookup:
		return UiText("open in chat", "переход в чат");
	}
	Unexpected("Jump state.");
}

[[nodiscard]] QString MetaText(
		const AyuMessages::MessageSnapshot &snapshot,
		DeletedJumpState jumpState) {
	const auto original = TimeLabel(DisplayTimestamp(snapshot));
	auto result = original;
	if (snapshot.editDate > 0
		&& (std::abs(snapshot.editDate - DisplayTimestamp(snapshot)) >= 60)) {
		result = UiText("%1 · deleted %2", "%1 · удалено %2").arg(
			original,
			TimeLabel(snapshot.editDate));
	}
	return result + u" · "_q + JumpStateText(jumpState);
}

[[nodiscard]] QString JumpTooltipText(
		not_null<Data::Thread*> thread,
		DeletedJumpState jumpState) {
	switch (jumpState) {
	case DeletedJumpState::LocalOnly:
		return LocalCopyHint();
	case DeletedJumpState::ExactLoaded:
		return OpenHint();
	case DeletedJumpState::NearestLoaded:
		return thread->asTopic()
			? UiText(
				"Open the nearest loaded place in this topic.",
				"Открыть ближайшее загруженное место в этой теме.")
			: UiText(
				"Open the nearest loaded place in this chat.",
				"Открыть ближайшее загруженное место в этом чате.");
	case DeletedJumpState::RemoteLookup:
		return OpenAttemptHint(thread);
	}
	Unexpected("Jump state.");
}

[[nodiscard]] QString OpenTooltip(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread,
		DeletedJumpState jumpState) {
	const auto deleted = DateTimeLabel(snapshot.editDate);
	const auto base = JumpTooltipText(thread, jumpState);
	if (deleted.isEmpty()) {
		return base;
	}
	return base + u"\n"_q + UiText(
		"Deleted at %1",
		"Удалено %1").arg(deleted);
}

[[nodiscard]] QString JumpToastText(
		not_null<Data::Thread*> thread,
		DeletedJumpState jumpState) {
	switch (jumpState) {
	case DeletedJumpState::LocalOnly:
		return LocalCopyHint();
	case DeletedJumpState::ExactLoaded:
		return QString();
	case DeletedJumpState::NearestLoaded:
		return thread->asTopic()
			? UiText(
				"Opened the nearest loaded place in the topic. The deleted message itself remains only in the local archive.",
				"Открыто ближайшее загруженное место в теме. Само удалённое сообщение осталось только в локальном архиве.")
			: UiText(
				"Opened the nearest loaded place in the chat. The deleted message itself remains only in the local archive.",
				"Открыто ближайшее загруженное место в чате. Само удалённое сообщение осталось только в локальном архиве.");
	case DeletedJumpState::RemoteLookup:
		return OpenAttemptHint(thread);
	}
	Unexpected("Jump state.");
}

[[nodiscard]] DeletedMessagePresentation BuildPresentation(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread) {
	const auto jump = ResolveJumpTarget(thread, snapshot.messageId);
	return {
		.state = DeletedStateLabel(snapshot),
		.text = FallbackBodyText(snapshot),
		.meta = MetaText(snapshot, jump.state),
		.tooltip = OpenTooltip(snapshot, thread, jump.state),
		.kind = SnapshotKind(snapshot),
		.jumpState = jump.state,
	};
}

[[nodiscard]] QString ResolveSenderName(
		const AyuMessages::MessageSnapshot &snapshot,
		not_null<Data::Thread*> thread) {
	if (IsOutgoing(snapshot, thread)) {
		return UiText("You", "Вы");
	}
	if (!snapshot.senderName.isEmpty()) {
		return snapshot.senderName;
	}

	auto &data = thread->session().data();
	if (snapshot.senderSerialized) {
		if (const auto peer = data.peerLoaded(DeserializePeerId(snapshot.senderSerialized))) {
			return peer->name();
		}
	}
	if (snapshot.fromId > 0) {
		if (const auto user = data.userLoaded(UserId(snapshot.fromId))) {
			return user->name();
		}
		if (const auto chat = data.chatLoaded(ChatId(snapshot.fromId))) {
			return chat->name();
		}
		if (const auto channel = data.channelLoaded(ChannelId(snapshot.fromId))) {
			return channel->name();
		}
	}
	return thread->peer()->name();
}

[[nodiscard]] style::color ChipTextColor(ScopeChipTone tone) {
	switch (tone) {
	case ScopeChipTone::Accent:
		return st::windowActiveTextFg;
	case ScopeChipTone::Info:
		return st::windowFg;
	case ScopeChipTone::Muted:
		return st::windowSubTextFg;
	}
	Unexpected("Chip tone.");
}

[[nodiscard]] QColor ChipFillColor(ScopeChipTone tone) {
	switch (tone) {
	case ScopeChipTone::Accent:
		return anim::with_alpha(st::windowActiveTextFg->c, 0.10);
	case ScopeChipTone::Info:
		return anim::with_alpha(st::windowFg->c, 0.07);
	case ScopeChipTone::Muted:
		return anim::with_alpha(st::windowSubTextFg->c, 0.08);
	}
	Unexpected("Chip tone.");
}

[[nodiscard]] QColor ChipBorderColor(ScopeChipTone tone) {
	switch (tone) {
	case ScopeChipTone::Accent:
		return anim::with_alpha(st::windowActiveTextFg->c, 0.22);
	case ScopeChipTone::Info:
		return anim::with_alpha(st::windowFg->c, 0.14);
	case ScopeChipTone::Muted:
		return anim::with_alpha(st::windowSubTextFg->c, 0.18);
	}
	Unexpected("Chip tone.");
}

void PaintChip(
		Painter &p,
		const QRect &rect,
		const ScopeChipData &chip) {
	if (rect.isNull() || chip.text.isEmpty()) {
		return;
	}
	p.setPen(ChipBorderColor(chip.tone));
	p.setBrush(ChipFillColor(chip.tone));
	p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
	p.setPen(ChipTextColor(chip.tone));
	p.setFont(st::defaultTextStyle.font->f);
	p.drawText(
		rect.left() + kChipHorizontalPadding,
		rect.top() + ((rect.height() - st::defaultTextStyle.font->height) / 2)
			+ st::defaultTextStyle.font->ascent,
		chip.text);
}

void LayoutChips(
		const std::vector<ScopeChipData> &chips,
		std::vector<QRect> *rects,
		int left,
		int top,
		int width) {
	rects->clear();
	rects->resize(chips.size());
	if (width <= 0) {
		return;
	}
	const auto right = left + width;
	const auto chipHeight = st::defaultTextStyle.font->height + (kChipVerticalPadding * 2);
	auto chipLeft = left;
	auto chipTop = top;
	for (auto i = 0, count = int(chips.size()); i != count; ++i) {
		const auto &chip = chips[i];
		if (chip.text.isEmpty()) {
			(*rects)[i] = {};
			continue;
		}
		const auto chipWidth = std::min(
			width,
			st::defaultTextStyle.font->width(chip.text) + (kChipHorizontalPadding * 2));
		if ((chipLeft != left) && ((chipLeft + chipWidth) > right)) {
			chipLeft = left;
			chipTop += chipHeight + kChipSpacing;
		}
		(*rects)[i] = QRect(chipLeft, chipTop, chipWidth, chipHeight);
		chipLeft += chipWidth + kChipSpacing;
	}
}

[[nodiscard]] QPainterPath BubblePath(
		const QRectF &rect,
		float64 topLeft,
		float64 topRight,
		float64 bottomRight,
		float64 bottomLeft) {
	auto path = QPainterPath();
	const auto left = rect.left();
	const auto top = rect.top();
	const auto right = rect.right();
	const auto bottom = rect.bottom();

	path.moveTo(left + topLeft, top);
	path.lineTo(right - topRight, top);
	if (topRight > 0.) {
		path.quadTo(right, top, right, top + topRight);
	}
	path.lineTo(right, bottom - bottomRight);
	if (bottomRight > 0.) {
		path.quadTo(right, bottom, right - bottomRight, bottom);
	}
	path.lineTo(left + bottomLeft, bottom);
	if (bottomLeft > 0.) {
		path.quadTo(left, bottom, left, bottom - bottomLeft);
	}
	path.lineTo(left, top + topLeft);
	if (topLeft > 0.) {
		path.quadTo(left, top, left + topLeft, top);
	}
	path.closeSubpath();
	return path;
}

[[nodiscard]] QPainterPath BubblePath(
		const QRect &rect,
		bool outgoing,
		const DeletedMessageVisualGroup &group) {
	const auto radius = float64(st::boxRadius);
	const auto compactRadius = std::max(4., radius * 0.45);
	auto topLeft = radius;
	auto topRight = radius;
	auto bottomRight = radius;
	auto bottomLeft = radius;
	if (outgoing) {
		if (group.groupedWithPrevious) {
			topRight = compactRadius;
		}
		if (group.groupedWithNext) {
			bottomRight = compactRadius;
		}
	} else {
		if (group.groupedWithPrevious) {
			topLeft = compactRadius;
		}
		if (group.groupedWithNext) {
			bottomLeft = compactRadius;
		}
	}
	return BubblePath(rect, topLeft, topRight, bottomRight, bottomLeft);
}

class DeletedMessagesIntroCard final : public Ui::RpWidget {
public:
	DeletedMessagesIntroCard(
		not_null<Ui::RpWidget*> parent,
		not_null<Data::Thread*> thread,
		int visibleCount,
		int totalCount,
		int oldestTimestamp,
		int newestTimestamp,
		QString searchQuery);

	int resizeGetHeight(int newWidth) override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void layoutChildren(int newWidth);

	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_scope = nullptr;
	Ui::FlatLabel *_details = nullptr;
	Ui::FlatLabel *_filter = nullptr;
	Ui::FlatLabel *_hint = nullptr;
	std::vector<ScopeChipData> _chips;
	std::vector<QRect> _chipRects;
	QRect _cardRect;
};

DeletedMessagesIntroCard::DeletedMessagesIntroCard(
		not_null<Ui::RpWidget*> parent,
		not_null<Data::Thread*> thread,
		int visibleCount,
		int totalCount,
		int oldestTimestamp,
		int newestTimestamp,
		QString searchQuery)
: Ui::RpWidget(parent)
, _chips(IntroScopeChips(thread, searchQuery)) {
	_title = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(ScopeHeadline()),
		st::boxTitle);
	_scope = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(ScopeSubtitle(thread)),
		st::boxLabel);
	_details = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(ScopeDetails(
			visibleCount,
			totalCount,
			oldestTimestamp,
			newestTimestamp)),
		st::sessionDateLabel);
	if (!searchQuery.isEmpty()) {
		_filter = Ui::CreateChild<Ui::FlatLabel>(
			this,
			rpl::single(SearchScopeDetails(searchQuery, visibleCount, totalCount)),
			st::sessionDateLabel);
	}
	_hint = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(ScopeHint(thread)),
		st::sessionDateLabel);

	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_scope->setAttribute(Qt::WA_TransparentForMouseEvents);
	_details->setAttribute(Qt::WA_TransparentForMouseEvents);
	if (_filter) {
		_filter->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	_hint->setAttribute(Qt::WA_TransparentForMouseEvents);
	_scope->setTryMakeSimilarLines(true);
	_details->setTryMakeSimilarLines(true);
	if (_filter) {
		_filter->setTryMakeSimilarLines(true);
	}
	_hint->setTryMakeSimilarLines(true);
	_title->setTextColorOverride(st::windowFgActive->c);
	_scope->setTextColorOverride(st::windowFg->c);
	_details->setTextColorOverride(st::windowSubTextFg->c);
	if (_filter) {
		_filter->setTextColorOverride(st::windowActiveTextFg->c);
	}
	_hint->setTextColorOverride(st::windowSubTextFg->c);
}

int DeletedMessagesIntroCard::resizeGetHeight(int newWidth) {
	layoutChildren(newWidth);
	return _cardRect.isNull() ? 0 : (_cardRect.bottom() + 1);
}

void DeletedMessagesIntroCard::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	layoutChildren(e->size().width());
}

void DeletedMessagesIntroCard::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_cardRect.isNull()) {
		return;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(st::msgServiceBg);
	p.setOpacity(0.9);
	p.setRenderHint(QPainter::Antialiasing);
	p.drawPath(BubblePath(
		_cardRect,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(anim::with_alpha(st::windowSubTextFg->c, 0.18));
	p.drawPath(BubblePath(
		_cardRect.adjusted(0, 0, -1, -1),
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
	for (auto i = 0, count = int(_chips.size()); i != count; ++i) {
		PaintChip(p, _chipRects[i], _chips[i]);
	}
}

void DeletedMessagesIntroCard::layoutChildren(int newWidth) {
	if (newWidth <= 0) {
		_cardRect = {};
		_chipRects.clear();
		return;
	}
	const auto outer = QRect(
		kRowOuterLeft,
		0,
		std::max(0, newWidth - kRowOuterLeft - kRowOuterRight),
		0);
	if (outer.width() <= 0) {
		_cardRect = {};
		_chipRects.clear();
		return;
	}
	const auto innerLeft = outer.left() + kIntroCardPadding.left();
	const auto innerTop = kIntroCardPadding.top();
	const auto innerWidth = std::max(
		1,
		outer.width() - kIntroCardPadding.left() - kIntroCardPadding.right());

	LayoutChips(_chips, &_chipRects, innerLeft, innerTop, innerWidth);
	auto top = innerTop;
	for (const auto &rect : _chipRects) {
		if (!rect.isNull()) {
			top = std::max(top, rect.bottom() + 10);
		}
	}

	_title->resizeToWidth(innerWidth);
	_scope->resizeToWidth(innerWidth);
	_details->resizeToWidth(innerWidth);
	if (_filter) {
		_filter->resizeToWidth(innerWidth);
	}
	_hint->resizeToWidth(innerWidth);

	_title->moveToLeft(innerLeft, top);
	top += _title->height() + 6;
	_scope->moveToLeft(innerLeft, top);
	top += _scope->height() + 4;
	_details->moveToLeft(innerLeft, top);
	top += _details->height();
	if (_filter) {
		top += 4;
		_filter->moveToLeft(innerLeft, top);
		top += _filter->height();
	}
	top += 8;
	_hint->moveToLeft(innerLeft, top);
	top += _hint->height() + kIntroCardPadding.bottom();

	_cardRect = QRect(outer.left(), 0, outer.width(), top);
}

class DeletedMessagesDayBadge final : public Ui::RpWidget {
public:
	DeletedMessagesDayBadge(
		not_null<Ui::RpWidget*> parent,
		int timestamp,
		DeletedMessagesDaySummary summary,
		bool searchMode);

	int resizeGetHeight(int newWidth) override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void layoutChildren(int newWidth);

	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_details = nullptr;
	QRect _bubbleRect;
};

DeletedMessagesDayBadge::DeletedMessagesDayBadge(
		not_null<Ui::RpWidget*> parent,
		int timestamp,
		DeletedMessagesDaySummary summary,
		bool searchMode)
: Ui::RpWidget(parent) {
	_title = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(DayHeadlineText(timestamp)),
		st::boxLabel);
	_details = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(DaySummaryDetails(summary, searchMode)),
		st::sessionDateLabel);
	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_details->setAttribute(Qt::WA_TransparentForMouseEvents);
	_title->setTextColorOverride(st::windowFgActive->c);
	_details->setTextColorOverride(st::windowSubTextFg->c);
}

int DeletedMessagesDayBadge::resizeGetHeight(int newWidth) {
	layoutChildren(newWidth);
	return _bubbleRect.isNull() ? 0 : (_bubbleRect.bottom() + 1);
}

void DeletedMessagesDayBadge::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	layoutChildren(e->size().width());
}

void DeletedMessagesDayBadge::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_bubbleRect.isNull()) {
		return;
	}
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(st::msgServiceBg);
	p.setOpacity(0.92);
	p.drawPath(BubblePath(
		_bubbleRect,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(anim::with_alpha(st::windowSubTextFg->c, 0.16));
	p.drawPath(BubblePath(
		_bubbleRect.adjusted(0, 0, -1, -1),
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
}

void DeletedMessagesDayBadge::layoutChildren(int newWidth) {
	if (newWidth <= 0) {
		_bubbleRect = {};
		return;
	}
	const auto availableWidth = std::max(0, newWidth - (kRowOuterLeft * 2));
	if (availableWidth <= 0) {
		_bubbleRect = {};
		return;
	}
	const auto naturalWidth = std::max(
		_title->naturalWidth(),
		_details->naturalWidth());
	const auto bubbleWidth = std::min(
		availableWidth,
		naturalWidth + kDayBadgePadding.left() + kDayBadgePadding.right());
	const auto bubbleX = kRowOuterLeft + ((availableWidth - bubbleWidth) / 2);
	const auto innerWidth = std::max(
		1,
		bubbleWidth - kDayBadgePadding.left() - kDayBadgePadding.right());
	_title->resizeToWidth(innerWidth);
	_details->resizeToWidth(innerWidth);
	const auto left = bubbleX + kDayBadgePadding.left();
	auto top = kDayBadgePadding.top();
	_title->moveToLeft(left, top);
	top += _title->height() + 2;
	_details->moveToLeft(left, top);
	top += _details->height() + kDayBadgePadding.bottom();
	_bubbleRect = QRect(bubbleX, 0, bubbleWidth, top);
}

class DeletedMessagesScopeBar final : public Ui::RpWidget {
public:
	explicit DeletedMessagesScopeBar(not_null<Ui::RpWidget*> parent);

	int resizeGetHeight(int newWidth) override;
	void setContent(
		QString title,
		QString scope,
		QString details,
		QString status,
		std::vector<ScopeChipData> chips);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void layoutChildren(int newWidth);

	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_scope = nullptr;
	Ui::FlatLabel *_details = nullptr;
	Ui::FlatLabel *_status = nullptr;
	std::vector<ScopeChipData> _chips;
	std::vector<QRect> _chipRects;
	QRect _cardRect;
};

DeletedMessagesScopeBar::DeletedMessagesScopeBar(
		not_null<Ui::RpWidget*> parent)
: Ui::RpWidget(parent) {
	_title = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::boxTitle);
	_scope = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::boxLabel);
	_details = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::sessionDateLabel);
	_status = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::sessionDateLabel);
	for (const auto label : { _title, _scope, _details, _status }) {
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		label->setTryMakeSimilarLines(true);
	}
	_title->setTextColorOverride(st::windowFgActive->c);
	_scope->setTextColorOverride(st::windowFg->c);
	_details->setTextColorOverride(st::windowSubTextFg->c);
	_status->setTextColorOverride(st::windowActiveTextFg->c);
}

int DeletedMessagesScopeBar::resizeGetHeight(int newWidth) {
	layoutChildren(newWidth);
	return _cardRect.isNull() ? 0 : (_cardRect.bottom() + kScopeBarSkip + 1);
}

void DeletedMessagesScopeBar::setContent(
		QString title,
		QString scope,
		QString details,
		QString status,
		std::vector<ScopeChipData> chips) {
	_title->setText(std::move(title));
	_scope->setText(std::move(scope));
	_details->setText(std::move(details));
	_status->setText(std::move(status));
	_chips = std::move(chips);
	layoutChildren(width());
	update();
}

void DeletedMessagesScopeBar::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	layoutChildren(e->size().width());
}

void DeletedMessagesScopeBar::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_cardRect.isNull()) {
		return;
	}
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(st::msgServiceBg);
	p.setOpacity(0.96);
	p.drawPath(BubblePath(
		_cardRect,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(anim::with_alpha(st::windowSubTextFg->c, 0.20));
	p.drawPath(BubblePath(
		_cardRect.adjusted(0, 0, -1, -1),
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
	for (auto i = 0, count = int(_chips.size()); i != count; ++i) {
		PaintChip(p, _chipRects[i], _chips[i]);
	}
}

void DeletedMessagesScopeBar::layoutChildren(int newWidth) {
	if (newWidth <= 0) {
		_cardRect = {};
		_chipRects.clear();
		return;
	}
	const auto outer = QRect(
		kRowOuterLeft,
		0,
		std::max(0, newWidth - kRowOuterLeft - kRowOuterRight),
		0);
	if (outer.width() <= 0) {
		_cardRect = {};
		_chipRects.clear();
		return;
	}
	const auto innerLeft = outer.left() + kScopeBarPadding.left();
	const auto innerTop = kScopeBarPadding.top();
	const auto innerWidth = std::max(
		1,
		outer.width() - kScopeBarPadding.left() - kScopeBarPadding.right());

	LayoutChips(_chips, &_chipRects, innerLeft, innerTop, innerWidth);
	auto top = innerTop;
	for (const auto &rect : _chipRects) {
		if (!rect.isNull()) {
			top = std::max(top, rect.bottom() + 8);
		}
	}
	_title->resizeToWidth(innerWidth);
	_scope->resizeToWidth(innerWidth);
	_details->resizeToWidth(innerWidth);
	_status->resizeToWidth(innerWidth);
	_title->moveToLeft(innerLeft, top);
	top += _title->height() + 4;
	_scope->moveToLeft(innerLeft, top);
	top += _scope->height() + 2;
	_details->moveToLeft(innerLeft, top);
	top += _details->height() + 3;
	_status->moveToLeft(innerLeft, top);
	top += _status->height() + kScopeBarPadding.bottom();
	_cardRect = QRect(outer.left(), 0, outer.width(), top);
}

class DeletedMessagesFooterBar final : public Ui::RpWidget {
public:
	explicit DeletedMessagesFooterBar(not_null<Ui::RpWidget*> parent);

	int resizeGetHeight(int newWidth) override;
	void setContent(QString title, QString details, QString status);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void layoutChildren(int newWidth);

	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_details = nullptr;
	Ui::FlatLabel *_status = nullptr;
	QRect _cardRect;
};

DeletedMessagesFooterBar::DeletedMessagesFooterBar(
		not_null<Ui::RpWidget*> parent)
: Ui::RpWidget(parent) {
	_title = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::boxLabel);
	_details = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::sessionDateLabel);
	_status = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(QString()),
		st::sessionDateLabel);
	for (const auto label : { _title, _details, _status }) {
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		label->setTryMakeSimilarLines(true);
	}
	_title->setTextColorOverride(st::windowFgActive->c);
	_details->setTextColorOverride(st::windowSubTextFg->c);
	_status->setTextColorOverride(st::windowActiveTextFg->c);
}

int DeletedMessagesFooterBar::resizeGetHeight(int newWidth) {
	layoutChildren(newWidth);
	return _cardRect.isNull() ? 0 : (_cardRect.bottom() + kFooterBarSkip + 1);
}

void DeletedMessagesFooterBar::setContent(
		QString title,
		QString details,
		QString status) {
	_title->setText(std::move(title));
	_details->setText(std::move(details));
	_status->setText(std::move(status));
	layoutChildren(width());
	update();
}

void DeletedMessagesFooterBar::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	layoutChildren(e->size().width());
}

void DeletedMessagesFooterBar::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_cardRect.isNull()) {
		return;
	}
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(st::historyReplyBg);
	p.setOpacity(0.98);
	p.drawPath(BubblePath(
		_cardRect,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(anim::with_alpha(st::windowSubTextFg->c, 0.18));
	p.drawPath(BubblePath(
		_cardRect.adjusted(0, 0, -1, -1),
		st::boxRadius,
		st::boxRadius,
		st::boxRadius,
		st::boxRadius));
}

void DeletedMessagesFooterBar::layoutChildren(int newWidth) {
	if (newWidth <= 0) {
		_cardRect = {};
		return;
	}
	const auto outer = QRect(
		kRowOuterLeft,
		0,
		std::max(0, newWidth - kRowOuterLeft - kRowOuterRight),
		0);
	if (outer.width() <= 0) {
		_cardRect = {};
		return;
	}
	const auto innerLeft = outer.left() + kFooterBarPadding.left();
	const auto innerTop = kFooterBarPadding.top();
	const auto innerWidth = std::max(
		1,
		outer.width() - kFooterBarPadding.left() - kFooterBarPadding.right());
	_title->resizeToWidth(innerWidth);
	_details->resizeToWidth(innerWidth);
	_status->resizeToWidth(innerWidth);
	_title->moveToLeft(innerLeft, innerTop);
	auto top = innerTop + _title->height() + 2;
	_details->moveToLeft(innerLeft, top);
	top += _details->height() + 4;
	_status->moveToLeft(innerLeft, top);
	top += _status->height() + kFooterBarPadding.bottom();
	_cardRect = QRect(outer.left(), 0, outer.width(), top);
}
class DeletedMessageRow final : public Ui::RpWidget {
public:
	DeletedMessageRow(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		AyuMessages::MessageSnapshot snapshot,
		QString senderLabel,
		DeletedMessageVisualGroup group);

	int resizeGetHeight(int newWidth) override;
	[[nodiscard]] const AyuMessages::MessageSnapshot &snapshot() const {
		return _snapshot;
	}

protected:
	bool event(QEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void layoutChildren(int newWidth);
	void openOriginal();

	const not_null<Window::SessionController*> _controller;
	const not_null<Data::Thread*> _thread;
	const AyuMessages::MessageSnapshot _snapshot;
	const QString _senderLabel;
	const DeletedMessageVisualGroup _group;
	const bool _outgoing = false;
	const DeletedMessagePresentation _presentation;
	const bool _interactive = false;

	style::owned_color _senderColor;
	style::owned_color _stateColor;
	style::owned_color _textColor;
	style::owned_color _metaColor;
	style::FlatLabel _senderSt = st::sessionDateLabel;
	style::FlatLabel _stateSt = st::sessionDateLabel;
	style::FlatLabel _textSt = st::boxLabel;
	style::FlatLabel _metaSt = st::sessionDateLabel;

	Ui::FlatLabel *_sender = nullptr;
	Ui::FlatLabel *_state = nullptr;
	Ui::FlatLabel *_text = nullptr;
	Ui::FlatLabel *_meta = nullptr;
	QRect _bubbleRect;
	bool _hovered = false;

};

DeletedMessageRow::DeletedMessageRow(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		AyuMessages::MessageSnapshot snapshot,
		QString senderLabel,
		DeletedMessageVisualGroup group)
: Ui::RpWidget(parent)
, _controller(controller)
, _thread(thread)
, _snapshot(std::move(snapshot))
, _senderLabel(std::move(senderLabel))
, _group(group)
, _outgoing(IsOutgoing(_snapshot, _thread))
, _presentation(BuildPresentation(_snapshot, _thread))
, _interactive(_presentation.jumpState != DeletedJumpState::LocalOnly)
, _senderColor(_outgoing ? st::msgOutServiceFg->c : st::msgInServiceFg->c)
, _stateColor(st::windowSubTextFg->c)
, _textColor(st::windowFg->c)
, _metaColor(_outgoing ? st::msgOutDateFg->c : st::msgInDateFg->c) {
	_senderSt.textFg = _senderColor.color();
	_senderSt.palette.linkFg = _senderColor.color();
	_stateSt.textFg = _stateColor.color();
	_stateSt.palette.linkFg = _stateColor.color();
	_textSt.textFg = _textColor.color();
	_textSt.palette.linkFg = _textColor.color();
	_metaSt.textFg = _metaColor.color();
	_metaSt.palette.linkFg = _metaColor.color();

	if (!_senderLabel.isEmpty()) {
		_sender = Ui::CreateChild<Ui::FlatLabel>(
			this,
			rpl::single(_senderLabel),
			_senderSt);
		_sender->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	_state = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(_presentation.state),
		_stateSt);
	_text = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(_presentation.text),
		_textSt);
	_meta = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(_presentation.meta),
		_metaSt);

	_state->setAttribute(Qt::WA_TransparentForMouseEvents);
	_text->setAttribute(Qt::WA_TransparentForMouseEvents);
	_meta->setAttribute(Qt::WA_TransparentForMouseEvents);
	setCursor(_interactive ? style::cur_pointer : style::cur_default);
	setToolTip(_presentation.tooltip);
}

int DeletedMessageRow::resizeGetHeight(int newWidth) {
	layoutChildren(newWidth);
	return _bubbleRect.isNull()
		? (kRowOuterTop + kRowOuterBottom)
		: (_bubbleRect.bottom()
			+ (_group.groupedWithNext ? kGroupedOuterSkip : kRowOuterBottom)
			+ 1);
}

bool DeletedMessageRow::event(QEvent *e) {
	switch (e->type()) {
	case QEvent::Enter:
		if (_interactive && !_hovered) {
			_hovered = true;
			update();
		}
		break;
	case QEvent::Leave:
		if (_hovered) {
			_hovered = false;
			update();
		}
		break;
	default:
		break;
	}
	return Ui::RpWidget::event(e);
}

void DeletedMessageRow::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	layoutChildren(e->size().width());
}

void DeletedMessageRow::mouseReleaseEvent(QMouseEvent *e) {
	Ui::RpWidget::mouseReleaseEvent(e);
	if (e->button() == Qt::LeftButton && rect().contains(e->pos())) {
		openOriginal();
	}
}

void DeletedMessageRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_bubbleRect.isNull()) {
		return;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(_outgoing
		? (_hovered ? st::msgOutBgSelected : st::msgOutBg)
		: (_hovered ? st::msgInBgSelected : st::msgInBg));
	p.setRenderHint(QPainter::Antialiasing);
	p.drawPath(BubblePath(_bubbleRect, _outgoing, _group));

	if (_hovered) {
		p.setBrush(Qt::NoBrush);
		p.setPen(_outgoing ? st::msgOutDateFg : st::msgInDateFg);
		p.drawPath(BubblePath(
			_bubbleRect.adjusted(0, 0, -1, -1),
			_outgoing,
			_group));
	}
}

void DeletedMessageRow::layoutChildren(int newWidth) {
	if (newWidth <= 0) {
		_bubbleRect = {};
		return;
	}

	const auto availableWidth = std::max(0, newWidth - kRowOuterLeft - kRowOuterRight);
	const auto maxBubbleWidth = std::max(
		kBubbleMinWidth,
		std::min(availableWidth, int(newWidth * kBubbleWidthFactor)));
	const auto maxTextWidth = std::max(
		1,
		maxBubbleWidth - kBubblePadding.left() - kBubblePadding.right());

	const auto senderWidth = _sender
		? std::min(_sender->naturalWidth(), maxTextWidth)
		: 0;
	const auto stateWidth = std::min(_state->naturalWidth(), maxTextWidth);
	const auto textNatural = std::min(_text->naturalWidth(), maxTextWidth);
	const auto metaWidth = std::min(_meta->naturalWidth(), maxTextWidth);
	const auto contentWidth = std::max(
		kBubbleMinWidth - kBubblePadding.left() - kBubblePadding.right(),
		std::max(senderWidth, std::max(stateWidth, std::max(textNatural, metaWidth))));

	if (_sender) {
		_sender->resizeToWidth(contentWidth);
	}
	_state->resizeToWidth(contentWidth);
	_text->resizeToWidth(contentWidth);
	_meta->resizeToWidth(metaWidth);

	const auto bubbleWidth = std::min(
		availableWidth,
		contentWidth + kBubblePadding.left() + kBubblePadding.right());
	const auto bubbleX = _outgoing
		? std::max(kRowOuterLeft, newWidth - kRowOuterRight - bubbleWidth)
		: kRowOuterLeft;
	const auto outerTop = _group.groupedWithPrevious
		? kGroupedOuterSkip
		: kRowOuterTop;
	auto top = outerTop + kBubblePadding.top();
	const auto left = bubbleX + kBubblePadding.left();

	if (_sender) {
		_sender->moveToLeft(left, top);
		top += _sender->height() + 2;
	}
	_state->moveToLeft(left, top);
	top += _state->height() + kBubbleHeaderSkip;
	_text->moveToLeft(left, top);
	top += _text->height() + kBubbleInnerSkip;
	_meta->moveToLeft(
		bubbleX + bubbleWidth - kBubblePadding.right() - _meta->width(),
		top);
	top += _meta->height() + kBubblePadding.bottom();

	_bubbleRect = QRect(bubbleX, outerTop, bubbleWidth, top - outerTop);
}

void DeletedMessageRow::openOriginal() {
	const auto controller = _controller;
	const auto jump = ResolveJumpTarget(_thread, _snapshot.messageId);
	if (jump.state == DeletedJumpState::LocalOnly) {
		controller->window().showToast(JumpToastText(_thread, jump.state));
		return;
	}
	const auto params = Window::SectionShow(Window::SectionShow::Way::Forward);
	if (jump.item) {
		controller->showMessage(jump.item, params);
	} else if (jump.openedMessageId > 0) {
		controller->showThread(_thread, jump.openedMessageId, params);
	} else {
		controller->window().showToast(LocalCopyHint());
		return;
	}
	const auto toast = JumpToastText(_thread, jump.state);
	if (!toast.isEmpty()) {
		controller->window().showToast(toast);
	}
}

class DeletedMessagesMemento;

class DeletedMessagesWidget final : public Window::SectionWidget {
public:
	DeletedMessagesWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread);
	~DeletedMessagesWidget() override;

	Dialogs::RowDescriptor activeChat() const override;
	bool hasTopBarShadow() const override {
		return true;
	}
	QPixmap grabForShowAnimation(
		const Window::SectionSlideParams &params) override;
	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	bool sameTypeAs(not_null<Window::SectionMemento*> memento) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;
	void setInternalState(
		const QRect &geometry,
		not_null<DeletedMessagesMemento*> memento);
	bool floatPlayerHandleWheelEvent(QEvent *e) override;
	QRect floatPlayerAvailableRect() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showAnimatedHook(
		const Window::SectionSlideParams &params) override;
	void showFinishedHook() override;
	void doSetInnerFocus() override;

private:
	void updateAdaptiveLayout();
	void recountChatWidth();
	void updateControlsGeometry();
	void rebuildContent();
	void refreshScopeBar();
	void refreshFooterBar();
	void applySearchQuery(QString query);
	void handleStorageChange();
	[[nodiscard]] DeletedMessageRow *currentVisibleRow() const;
	[[nodiscard]] DeletedMessagesSectionState captureState() const;
	void applyState(const DeletedMessagesSectionState &state);
	void saveState(not_null<DeletedMessagesMemento*> memento);
	void restoreState(not_null<DeletedMessagesMemento*> memento);
	[[nodiscard]] bool restoreAnchor(
		int messageId,
		int timestamp,
		int offset);

	const not_null<Data::Thread*> _thread;
	const not_null<History*> _history;
	std::shared_ptr<Ui::ChatTheme> _theme;
	object_ptr<HistoryView::TopBarWidget> _topBar;
	object_ptr<DeletedMessagesScopeBar> _scopeBar;
	object_ptr<DeletedMessagesFooterBar> _footerBar;
	object_ptr<Ui::PlainShadow> _topBarShadow;
	std::unique_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::VerticalLayout> _content;
	std::vector<DeletedMessageRow*> _rows;
	std::vector<AyuMessages::MessageSnapshot> _messages;
	std::map<int, DeletedMessagesDaySummary> _daySummaries;
	QString _searchQuery;
	int _totalMessages = 0;
	int _oldestTimestamp = 0;
	int _newestTimestamp = 0;

};

class DeletedMessagesMemento final : public Window::SectionMemento {
public:
	explicit DeletedMessagesMemento(not_null<Data::Thread*> thread)
	: _thread(thread->migrateToOrMe()) {
		const auto state = RememberedState(_thread);
		_scrollTop = state.scrollTop;
		_searchQuery = state.searchQuery;
		_anchorMessageId = state.anchorMessageId;
		_anchorTimestamp = state.anchorTimestamp;
		_anchorOffset = state.anchorOffset;
	}

	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override {
		if (column == Window::Column::Third) {
			return nullptr;
		}
		auto result = object_ptr<DeletedMessagesWidget>(
			parent,
			controller,
			_thread);
		result->setInternalState(geometry, this);
		return result;
	}

	[[nodiscard]] not_null<Data::Thread*> thread() const {
		return _thread;
	}
	void setScrollTop(int scrollTop) {
		_scrollTop = scrollTop;
	}
	[[nodiscard]] int scrollTop() const {
		return _scrollTop;
	}
	void setSearchQuery(QString searchQuery) {
		_searchQuery = std::move(searchQuery);
	}
	[[nodiscard]] const QString &searchQuery() const {
		return _searchQuery;
	}
	void setAnchorMessageId(int anchorMessageId) {
		_anchorMessageId = anchorMessageId;
	}
	[[nodiscard]] int anchorMessageId() const {
		return _anchorMessageId;
	}
	void setAnchorTimestamp(int anchorTimestamp) {
		_anchorTimestamp = anchorTimestamp;
	}
	[[nodiscard]] int anchorTimestamp() const {
		return _anchorTimestamp;
	}
	void setAnchorOffset(int anchorOffset) {
		_anchorOffset = anchorOffset;
	}
	[[nodiscard]] int anchorOffset() const {
		return _anchorOffset;
	}
	Data::ForumTopic *topicForRemoveRequests() const override {
		return _thread->asTopic();
	}
	Data::SavedSublist *sublistForRemoveRequests() const override {
		return _thread->asSublist();
	}

private:
	const not_null<Data::Thread*> _thread;
	int _scrollTop = -1;
	QString _searchQuery;
	int _anchorMessageId = 0;
	int _anchorTimestamp = 0;
	int _anchorOffset = 0;

};

DeletedMessagesWidget::DeletedMessagesWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread)
: Window::SectionWidget(parent, controller, thread->peer())
, _thread(thread->migrateToOrMe())
, _history(_thread->owningHistory())
, _topBar(this, controller)
, _scopeBar(this)
, _footerBar(this)
, _topBarShadow(this)
, _scroll(std::make_unique<Ui::ScrollArea>(
	this,
	controller->chatStyle()->value(lifetime(), st::historyScroll),
	false)) {
	controller->chatStyle()->paletteChanged(
	) | rpl::on_next([=] {
		_scroll->updateBars();
	}, _scroll->lifetime());

	Window::ChatThemeValueFromPeer(
		controller,
		_thread->peer()
	) | rpl::on_next([=](std::shared_ptr<Ui::ChatTheme> &&theme) {
		_theme = std::move(theme);
		controller->setChatStyleTheme(_theme);
		update();
	}, lifetime());

	_topBar->setActiveChat(
		HistoryView::TopBarWidget::ActiveChat{
			.key = _thread,
			.section = Dialogs::EntryState::Section::History,
		},
		nullptr);
	_topBar->move(0, 0);
	_topBar->show();
	_scopeBar->show();
	_footerBar->show();

	_topBarShadow->raise();
	controller->adaptive().value(
	) | rpl::on_next([=] {
		updateAdaptiveLayout();
	}, lifetime());
	_topBar->searchRequest(
	) | rpl::on_next([=] {
		if (!_topBar->searchMode()) {
			_topBar->toggleSearch(true, anim::type::normal);
		}
		_topBar->searchSetFocus();
	}, lifetime());
	_topBar->searchQuery(
	) | rpl::distinct_until_changed() | rpl::on_next([=](const QString &query) {
		applySearchQuery(query.trimmed());
	}, lifetime());

	_content = _scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(this));
	_scroll->show();
	_scroll->scrolls(
	) | rpl::on_next([=] {
		refreshScopeBar();
	}, lifetime());
	AyuMessages::deletedMessagesChanged(
	) | rpl::start_with_next([=] {
		handleStorageChange();
	}, lifetime());
}

DeletedMessagesWidget::~DeletedMessagesWidget() {
	SaveRememberedState(_thread, captureState());
}

Dialogs::RowDescriptor DeletedMessagesWidget::activeChat() const {
	return {
		_thread,
		FullMsgId(_history->peer->id, ShowAtUnreadMsgId)
	};
}

QPixmap DeletedMessagesWidget::grabForShowAnimation(
		const Window::SectionSlideParams &params) {
	_topBar->updateControlsVisibility();
	if (params.withTopBarShadow) {
		_topBarShadow->hide();
	}
	const auto result = Ui::GrabWidget(this);
	if (params.withTopBarShadow) {
		_topBarShadow->show();
	}
	return result;
}

bool DeletedMessagesWidget::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	if (const auto deleted = dynamic_cast<DeletedMessagesMemento*>(memento.get())) {
		if (deleted->thread() == _thread
			|| deleted->thread()->migrateToOrMe() == _thread) {
			restoreState(deleted);
			return true;
		}
	}
	return false;
}

bool DeletedMessagesWidget::sameTypeAs(not_null<Window::SectionMemento*> memento) {
	return dynamic_cast<DeletedMessagesMemento*>(memento.get()) != nullptr;
}

std::shared_ptr<Window::SectionMemento> DeletedMessagesWidget::createMemento() {
	auto result = std::make_shared<DeletedMessagesMemento>(_thread);
	saveState(result.get());
	return result;
}

void DeletedMessagesWidget::setInternalState(
		const QRect &geometry,
		not_null<DeletedMessagesMemento*> memento) {
	setGeometry(geometry);
	Ui::SendPendingMoveResizeEvents(this);
	restoreState(memento);
}

bool DeletedMessagesWidget::floatPlayerHandleWheelEvent(QEvent *e) {
	return _scroll->viewportEvent(e);
}

QRect DeletedMessagesWidget::floatPlayerAvailableRect() {
	return mapToGlobal(_scroll->geometry());
}

void DeletedMessagesWidget::resizeEvent(QResizeEvent *e) {
	if (!width() || !height()) {
		return;
	}
	recountChatWidth();
	updateControlsGeometry();
	Window::SectionWidget::resizeEvent(e);
}

void DeletedMessagesWidget::paintEvent(QPaintEvent *e) {
	if (animatingShow()) {
		SectionWidget::paintEvent(e);
		return;
	} else if (controller()->contentOverlapped(this, e)) {
		return;
	}
	if (!_theme) {
		QPainter(this).fillRect(e->rect(), st::windowBg);
		return;
	}
	const auto aboveHeight = _topBar->height()
		+ (_scopeBar ? _scopeBar->height() : 0);
	const auto bg = e->rect().intersected(
		QRect(0, aboveHeight, width(), height() - aboveHeight));
	SectionWidget::PaintBackground(controller(), _theme.get(), this, bg);
}

void DeletedMessagesWidget::showAnimatedHook(
		const Window::SectionSlideParams &params) {
	_topBar->setAnimatingMode(true);
	if (params.withTopBarShadow) {
		_topBarShadow->show();
	}
}

void DeletedMessagesWidget::showFinishedHook() {
	_topBar->setAnimatingMode(false);
}

void DeletedMessagesWidget::doSetInnerFocus() {
	_scroll->setFocus();
}

void DeletedMessagesWidget::updateAdaptiveLayout() {
	_topBarShadow->moveToLeft(
		controller()->adaptive().isOneColumn() ? 0 : st::lineWidth,
		_topBar->height() + (_scopeBar ? _scopeBar->height() : 0));
}

void DeletedMessagesWidget::recountChatWidth() {
	const auto layout = (width() < st::adaptiveChatWideWidth)
		? Window::Adaptive::ChatLayout::Normal
		: Window::Adaptive::ChatLayout::Wide;
	controller()->adaptive().setChatLayout(layout);
}

void DeletedMessagesWidget::updateControlsGeometry() {
	const auto contentWidth = width();
	const auto newScrollTop = _scroll->isHidden()
		? std::nullopt
		: base::make_optional(_scroll->scrollTop() + topDelta());

	_topBar->resizeToWidth(contentWidth);
	const auto scopeHeight = _scopeBar
		? _scopeBar->resizeGetHeight(contentWidth)
		: 0;
	if (_scopeBar) {
		_scopeBar->resize(contentWidth, scopeHeight);
		_scopeBar->move(0, _topBar->height());
	}
	const auto footerHeight = _footerBar
		? _footerBar->resizeGetHeight(contentWidth)
		: 0;
	if (_footerBar) {
		_footerBar->resize(contentWidth, footerHeight);
		_footerBar->move(0, std::max(_topBar->height() + scopeHeight, height() - footerHeight));
	}
	_topBarShadow->resize(contentWidth, st::lineWidth);

	const auto top = _topBar->height() + scopeHeight;
	const auto scrollSize = QSize(
		contentWidth,
		std::max(0, height() - top - footerHeight));
	if (_scroll->size() != scrollSize) {
		_scroll->resize(scrollSize);
		if (_content) {
			_content->resizeToWidth(scrollSize.width());
		}
	}
	_scroll->move(0, top);
	if (newScrollTop) {
		_scroll->scrollToY(*newScrollTop);
	}
	updateAdaptiveLayout();
}

[[nodiscard]] DeletedMessageRow *DeletedMessagesWidget::currentVisibleRow() const {
	if (_rows.empty()) {
		return nullptr;
	}
	const auto scrollTop = _scroll ? _scroll->scrollTop() : 0;
	for (const auto row : _rows) {
		if (row && (row->geometry().bottom() >= scrollTop)) {
			return row;
		}
	}
	return _rows.back();
}

void DeletedMessagesWidget::refreshScopeBar() {
	if (_scopeBar == nullptr) {
		refreshFooterBar();
		return;
	}
	const auto current = currentVisibleRow();
	const auto timestamp = current ? DisplayTimestamp(current->snapshot()) : 0;
	const auto summary = [&]() -> const DeletedMessagesDaySummary* {
		const auto i = _daySummaries.find(DayKey(timestamp));
		return (i != _daySummaries.end()) ? &i->second : nullptr;
	}();
	_scopeBar->setContent(
		TitleText(int(_messages.size()), _totalMessages),
		ScopeSubtitle(_thread),
		_searchQuery.isEmpty()
			? ScopeDetails(
				int(_messages.size()),
				_totalMessages,
				_oldestTimestamp,
				_newestTimestamp)
			: SearchScopeDetails(_searchQuery, int(_messages.size()), _totalMessages),
		HistoryStatusText(
			summary,
			timestamp,
			!_searchQuery.isEmpty(),
			_scroll && (_scroll->scrollTop() >= std::max(0, _scroll->scrollTopMax() - 2)),
			!_messages.empty(),
			_searchQuery),
		HeaderScopeChips(_thread, _searchQuery));
	refreshFooterBar();
}

void DeletedMessagesWidget::refreshFooterBar() {
	if (_footerBar == nullptr) {
		return;
	}
	_footerBar->setContent(
		UiText("Local deleted archive", "Локальный архив"),
		_thread->asTopic()
			? UiText(
				"This screen mirrors one topic. Tap a bubble to jump back to the live topic.",
				"Этот экран повторяет одну тему. Нажмите на пузырь, чтобы вернуться в живую тему.")
			: UiText(
				"This screen mirrors one chat. Tap a bubble to jump back to the live chat.",
				"Этот экран повторяет один чат. Нажмите на пузырь, чтобы вернуться в живой чат."),
		_messages.empty()
			? (_searchQuery.isEmpty()
				? UiText("Waiting for saved local copies", "Ждём локально сохранённые копии")
				: UiText("No matches for the current filter", "По текущему фильтру ничего не найдено"))
			: (_searchQuery.isEmpty()
				? UiText("Read-only · local copies", "Только чтение · локальные копии")
				: UiText("Filter active · read-only", "Фильтр включён · только чтение")));
}

void DeletedMessagesWidget::applySearchQuery(QString query) {
	if (_searchQuery == query) {
		return;
	}
	const auto before = captureState();
	_searchQuery = std::move(query);
	rebuildContent();
	if (!restoreAnchor(
			before.anchorMessageId,
			before.anchorTimestamp,
			before.anchorOffset)) {
		_scroll->scrollToY(_searchQuery.isEmpty() ? _scroll->scrollTopMax() : 0);
	}
	SaveRememberedState(_thread, captureState());
}

void DeletedMessagesWidget::handleStorageChange() {
	if (!_scroll || !_content) {
		rebuildContent();
		return;
	}
	const auto before = captureState();
	const auto wasNearBottom = (_scroll->scrollTop()
		>= std::max(0, _scroll->scrollTopMax() - 2));
	rebuildContent();
	if (!restoreAnchor(
			before.anchorMessageId,
			before.anchorTimestamp,
			before.anchorOffset)) {
		const auto target = wasNearBottom
			? _scroll->scrollTopMax()
			: (before.scrollTop >= 0)
			? std::clamp(before.scrollTop, 0, _scroll->scrollTopMax())
			: _scroll->scrollTopMax();
		_scroll->scrollToY(target);
	}
	refreshScopeBar();
	SaveRememberedState(_thread, captureState());
}

void DeletedMessagesWidget::rebuildContent() {
	if (!_content) {
		return;
	}

	_rows.clear();
	_daySummaries.clear();
	_messages = LoadMessages(_thread);
	_totalMessages = int(_messages.size());
	_oldestTimestamp = _messages.empty() ? 0 : DisplayTimestamp(_messages.front());
	_newestTimestamp = _messages.empty() ? 0 : DisplayTimestamp(_messages.back());
	if (!_searchQuery.isEmpty()) {
		auto filtered = std::vector<AyuMessages::MessageSnapshot>();
		filtered.reserve(_messages.size());
		for (const auto &message : _messages) {
			if (MatchesSearchQuery(message, _thread, _searchQuery)) {
				filtered.push_back(message);
			}
		}
		_messages = std::move(filtered);
	}
	_daySummaries = BuildDaySummaries(_messages);
	_topBar->setCustomTitle(TitleText(int(_messages.size()), _totalMessages));
	_content->clear();
	Ui::AddSkip(_content, st::defaultVerticalListSkip);

	_content->add(
		object_ptr<DeletedMessagesIntroCard>(
			_content,
			_thread,
			int(_messages.size()),
			_totalMessages,
			_oldestTimestamp,
			_newestTimestamp,
			_searchQuery));
	Ui::AddSkip(_content, st::defaultVerticalListSkip);

	if (_messages.empty()) {
		const auto empty = _content->add(
			object_ptr<Ui::FlatLabel>(
				_content,
				rpl::single(EmptyText(_searchQuery)),
				st::boxLabel),
			st::boxRowPadding + QMargins(0, st::defaultVerticalListSkip, 0, 0));
		empty->setTryMakeSimilarLines(true);
		Ui::AddSkip(_content, st::defaultVerticalListSkip);
		if (_scroll->width() > 0) {
			_content->resizeToWidth(_scroll->width());
		}
		refreshScopeBar();
		return;
	}

	auto currentDay = QDate();
	auto previousSender = QString();
	auto previousOutgoing = false;
	auto previousDay = QDate();
	auto havePreviousMessage = false;
	const auto groupedChat = !_thread->peer()->isUser() && !_thread->peer()->isSelf();
	for (auto i = 0, count = int(_messages.size()); i != count; ++i) {
		const auto &message = _messages[i];
		const auto stamp = DisplayTimestamp(message);
		const auto day = QDateTime::fromSecsSinceEpoch(stamp).toLocalTime().date();
		if (day != currentDay) {
			currentDay = day;
			Ui::AddSkip(_content, st::defaultVerticalListSkip / 2);
			auto summary = DeletedMessagesDaySummary();
			if (const auto i = _daySummaries.find(DayKey(stamp)); i != _daySummaries.end()) {
				summary = i->second;
			}
			_content->add(
				object_ptr<DeletedMessagesDayBadge>(
					_content,
					stamp,
					summary,
					!_searchQuery.isEmpty()));
			Ui::AddSkip(_content, st::defaultVerticalListSkip / 2);
		}
		const auto sender = ResolveSenderName(message, _thread);
		const auto outgoing = IsOutgoing(message, _thread);
		const auto groupedWithPrevious = (i > 0)
			&& SameVisualCluster(_messages[i - 1], message, _thread);
		const auto groupedWithNext = (i + 1 < count)
			&& SameVisualCluster(message, _messages[i + 1], _thread);
		const auto showSender = groupedChat
			&& !outgoing
			&& (!havePreviousMessage
				|| previousOutgoing
				|| (previousSender != sender)
				|| (previousDay != day)
				|| !groupedWithPrevious);
		const auto row = _content->add(
			object_ptr<DeletedMessageRow>(
				_content,
				controller(),
				_thread,
				message,
				showSender ? sender : QString(),
				DeletedMessageVisualGroup{
					.groupedWithPrevious = groupedWithPrevious,
					.groupedWithNext = groupedWithNext,
				}));
		_rows.push_back(row);
		previousSender = sender;
		previousOutgoing = outgoing;
		previousDay = day;
		havePreviousMessage = true;
	}

	Ui::AddSkip(_content, st::defaultVerticalListSkip);
	if (_scroll->width() > 0) {
		_content->resizeToWidth(_scroll->width());
	}
	refreshScopeBar();
}

DeletedMessagesSectionState DeletedMessagesWidget::captureState() const {
	auto state = DeletedMessagesSectionState();
	state.scrollTop = _scroll ? _scroll->scrollTop() : -1;
	state.searchQuery = _searchQuery;
	if (!_scroll) {
		return state;
	}
	const auto scrollTop = _scroll->scrollTop();
	for (const auto row : _rows) {
		if (!row) {
			continue;
		}
		const auto geometry = row->geometry();
		if (geometry.bottom() >= scrollTop) {
			state.anchorMessageId = row->snapshot().messageId;
			state.anchorTimestamp = DisplayTimestamp(row->snapshot());
			state.anchorOffset = scrollTop - geometry.top();
			return state;
		}
	}
	if (!_rows.empty()) {
		const auto row = _rows.back();
		if (row) {
			state.anchorMessageId = row->snapshot().messageId;
			state.anchorTimestamp = DisplayTimestamp(row->snapshot());
			state.anchorOffset = std::max(0, scrollTop - row->geometry().top());
		}
	}
	return state;
}

void DeletedMessagesWidget::applyState(const DeletedMessagesSectionState &state) {
	_searchQuery = state.searchQuery;
	if (!_searchQuery.isEmpty() && !_topBar->searchMode()) {
		_topBar->toggleSearch(true, anim::type::instant);
	} else if (_searchQuery.isEmpty() && _topBar->searchMode()) {
		_topBar->toggleSearch(false, anim::type::instant);
	}
	if (_topBar->searchQueryCurrent() != _searchQuery) {
		_topBar->searchSetText(_searchQuery);
	}
	rebuildContent();
	if (!restoreAnchor(
			state.anchorMessageId,
			state.anchorTimestamp,
			state.anchorOffset)) {
		const auto target = (state.scrollTop >= 0)
			? std::clamp(state.scrollTop, 0, _scroll->scrollTopMax())
			: _scroll->scrollTopMax();
		_scroll->scrollToY(target);
	}
	refreshScopeBar();
	SaveRememberedState(_thread, captureState());
}

bool DeletedMessagesWidget::restoreAnchor(
		int messageId,
		int timestamp,
		int offset) {
	if (!_scroll || ((messageId <= 0) && (timestamp <= 0))) {
		return false;
	}
	DeletedMessageRow *nearest = nullptr;
	auto nearestDistance = std::numeric_limits<int>::max();
	for (const auto row : _rows) {
		if (!row) {
			continue;
		}
		const auto &snapshot = row->snapshot();
		if ((snapshot.messageId == messageId)
			&& (!timestamp || (DisplayTimestamp(snapshot) == timestamp))) {
			const auto target = std::clamp(
				row->geometry().top() + offset,
				0,
				_scroll->scrollTopMax());
			_scroll->scrollToY(target);
			return true;
		}
		if (timestamp) {
			const auto distance = std::abs(DisplayTimestamp(snapshot) - timestamp);
			if (distance < nearestDistance) {
				nearestDistance = distance;
				nearest = row;
			}
		}
	}
	if (nearest) {
		const auto target = std::clamp(
			nearest->geometry().top() + offset,
			0,
			_scroll->scrollTopMax());
		_scroll->scrollToY(target);
		return true;
	}
	if (timestamp) {
		return restoreAnchor(messageId, 0, offset);
	}
	return false;
}

void DeletedMessagesWidget::saveState(not_null<DeletedMessagesMemento*> memento) {
	const auto state = captureState();
	memento->setScrollTop(state.scrollTop);
	memento->setSearchQuery(state.searchQuery);
	memento->setAnchorMessageId(state.anchorMessageId);
	memento->setAnchorTimestamp(state.anchorTimestamp);
	memento->setAnchorOffset(state.anchorOffset);
	SaveRememberedState(_thread, state);
}

void DeletedMessagesWidget::restoreState(not_null<DeletedMessagesMemento*> memento) {
	auto state = DeletedMessagesSectionState();
	state.scrollTop = memento->scrollTop();
	state.searchQuery = memento->searchQuery();
	state.anchorMessageId = memento->anchorMessageId();
	state.anchorTimestamp = memento->anchorTimestamp();
	state.anchorOffset = memento->anchorOffset();
	applyState(state);
}

} // namespace

[[nodiscard]] std::shared_ptr<Window::SectionMemento> MakeDeletedMessagesSection(
		not_null<Data::Thread*> thread) {
	return std::make_shared<DeletedMessagesMemento>(thread);
}

} // namespace AyuRecall
