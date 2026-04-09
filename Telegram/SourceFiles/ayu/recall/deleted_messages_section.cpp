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
	return UiText("Open original message in chat", "Открыть исходное сообщение в чате");
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

[[nodiscard]] QString NearestCopyHint() {
	return UiText(
		"Opened the nearest available place in chat. The deleted message itself is only saved locally.",
		"Открыто ближайшее доступное место в чате. Само удалённое сообщение сохранено только локально.");
}

[[nodiscard]] QString DeletedMessagePlaceholder() {
	return UiText("Saved deleted message", "Сохранённое удалённое сообщение");
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

[[nodiscard]] QString MetaText(const AyuMessages::MessageSnapshot &snapshot) {
	const auto original = TimeLabel(DisplayTimestamp(snapshot));
	if (snapshot.editDate <= 0
		|| (std::abs(snapshot.editDate - DisplayTimestamp(snapshot)) < 60)) {
		return original;
	}
	return UiText("%1 · deleted %2", "%1 · удалено %2").arg(
		original,
		TimeLabel(snapshot.editDate));
}

[[nodiscard]] QString OpenTooltip(const AyuMessages::MessageSnapshot &snapshot) {
	const auto deleted = DateTimeLabel(snapshot.editDate);
	if (deleted.isEmpty()) {
		return OpenHint();
	}
	return OpenHint() + u"\n"_q + UiText(
		"Deleted at %1",
		"Удалено %1").arg(deleted);
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

[[nodiscard]] int ResolveJumpTargetMessageId(
		not_null<Data::Thread*> thread,
		int messageId) {
	if (messageId <= 0) {
		return 0;
	}
	auto &data = thread->session().data();
	const auto peerId = thread->peer()->id;
	const auto hasLoaded = [&](int candidate) {
		return candidate > 0
			&& data.message(FullMsgId(peerId, candidate)) != nullptr;
	};
	if (hasLoaded(messageId)) {
		return messageId;
	}
	constexpr auto kProbeRadius = 256;
	for (auto step = 1; step <= kProbeRadius; ++step) {
		const auto before = messageId - step;
		if (hasLoaded(before)) {
			return before;
		}
		const auto after = messageId + step;
		if (hasLoaded(after)) {
			return after;
		}
	}
	return 0;
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
		int newestTimestamp);

	int resizeGetHeight(int newWidth) override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void layoutChildren(int newWidth);

	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_scope = nullptr;
	Ui::FlatLabel *_details = nullptr;
	Ui::FlatLabel *_hint = nullptr;
	QRect _cardRect;
};

DeletedMessagesIntroCard::DeletedMessagesIntroCard(
		not_null<Ui::RpWidget*> parent,
		not_null<Data::Thread*> thread,
		int visibleCount,
		int totalCount,
		int oldestTimestamp,
		int newestTimestamp)
: Ui::RpWidget(parent) {
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
	_hint = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(ScopeHint(thread)),
		st::sessionDateLabel);

	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_scope->setAttribute(Qt::WA_TransparentForMouseEvents);
	_details->setAttribute(Qt::WA_TransparentForMouseEvents);
	_hint->setAttribute(Qt::WA_TransparentForMouseEvents);
	_scope->setTryMakeSimilarLines(true);
	_details->setTryMakeSimilarLines(true);
	_hint->setTryMakeSimilarLines(true);
	_title->setTextColorOverride(st::windowFgActive->c);
	_scope->setTextColorOverride(st::windowFg->c);
	_details->setTextColorOverride(st::windowSubTextFg->c);
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
	p.drawPath(BubblePath(_cardRect, st::boxRadius, st::boxRadius, st::boxRadius, st::boxRadius));
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(anim::with_alpha(st::windowSubTextFg->c, 0.18));
	p.drawPath(BubblePath(_cardRect.adjusted(0, 0, -1, -1), st::boxRadius, st::boxRadius, st::boxRadius, st::boxRadius));
}

void DeletedMessagesIntroCard::layoutChildren(int newWidth) {
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
	const auto innerWidth = std::max(
		1,
		outer.width() - kIntroCardPadding.left() - kIntroCardPadding.right());

	_title->resizeToWidth(innerWidth);
	_scope->resizeToWidth(innerWidth);
	_details->resizeToWidth(innerWidth);
	_hint->resizeToWidth(innerWidth);

	auto top = kIntroCardPadding.top();
	const auto left = outer.left() + kIntroCardPadding.left();
	_title->moveToLeft(left, top);
	top += _title->height() + 6;
	_scope->moveToLeft(left, top);
	top += _scope->height() + 4;
	_details->moveToLeft(left, top);
	top += _details->height() + 8;
	_hint->moveToLeft(left, top);
	top += _hint->height() + kIntroCardPadding.bottom();

	_cardRect = QRect(
		outer.left(),
		0,
		outer.width(),
		top);
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

	style::owned_color _senderColor;
	style::owned_color _textColor;
	style::owned_color _metaColor;
	style::FlatLabel _senderSt = st::sessionDateLabel;
	style::FlatLabel _textSt = st::boxLabel;
	style::FlatLabel _metaSt = st::sessionDateLabel;

	Ui::FlatLabel *_sender = nullptr;
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
, _senderColor(_outgoing ? st::msgOutServiceFg->c : st::msgInServiceFg->c)
, _textColor(st::windowFg->c)
, _metaColor(_outgoing ? st::msgOutDateFg->c : st::msgInDateFg->c) {
	_senderSt.textFg = _senderColor.color();
	_senderSt.palette.linkFg = _senderColor.color();
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
	_text = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(_snapshot.text.isEmpty()
			? DeletedMessagePlaceholder()
			: _snapshot.text),
		_textSt);
	_meta = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(MetaText(_snapshot)),
		_metaSt);

	_text->setAttribute(Qt::WA_TransparentForMouseEvents);
	_meta->setAttribute(Qt::WA_TransparentForMouseEvents);
	setCursor(style::cur_pointer);
	setToolTip(OpenTooltip(_snapshot));
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
		if (!_hovered) {
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
	const auto textNatural = std::min(_text->naturalWidth(), maxTextWidth);
	const auto metaWidth = std::min(_meta->naturalWidth(), maxTextWidth);
	const auto contentWidth = std::max(
		kBubbleMinWidth - kBubblePadding.left() - kBubblePadding.right(),
		std::max(senderWidth, std::max(textNatural, metaWidth)));

	if (_sender) {
		_sender->resizeToWidth(contentWidth);
	}
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
		top += _sender->height() + kBubbleInnerSkip;
	}
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
	if (_snapshot.messageId <= 0) {
		controller->window().showToast(LocalCopyHint());
		return;
	}
	const auto jumpTarget = ResolveJumpTargetMessageId(_thread, _snapshot.messageId);
	const auto params = Window::SectionShow(Window::SectionShow::Way::Forward);
	controller->showThread(
		_thread,
		jumpTarget ? jumpTarget : _snapshot.messageId,
		params);
	if (jumpTarget == 0) {
		controller->window().showToast(OpenAttemptHint(_thread));
	} else if (jumpTarget != _snapshot.messageId) {
		controller->window().showToast(NearestCopyHint());
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
	void applySearchQuery(QString query);
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
	object_ptr<Ui::PlainShadow> _topBarShadow;
	std::unique_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::VerticalLayout> _content;
	std::vector<DeletedMessageRow*> _rows;
	std::vector<AyuMessages::MessageSnapshot> _messages;
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

	rebuildContent();
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
	const auto aboveHeight = _topBar->height();
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
		_topBar->height());
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
	_topBarShadow->resize(contentWidth, st::lineWidth);

	const auto top = _topBar->height();
	const auto scrollSize = QSize(contentWidth, height() - top);
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

void DeletedMessagesWidget::rebuildContent() {
	if (!_content) {
		return;
	}

	_rows.clear();
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
			_newestTimestamp));
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
			Ui::AddDividerText(_content, rpl::single(DayLabel(stamp)));
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
	SaveRememberedState(_thread, captureState());
}

bool DeletedMessagesWidget::restoreAnchor(
		int messageId,
		int timestamp,
		int offset) {
	if (!_scroll || (messageId <= 0)) {
		return false;
	}
	for (const auto row : _rows) {
		if (!row) {
			continue;
		}
		const auto &snapshot = row->snapshot();
		if (snapshot.messageId != messageId) {
			continue;
		}
		if (timestamp && (DisplayTimestamp(snapshot) != timestamp)) {
			continue;
		}
		const auto target = std::clamp(
			row->geometry().top() + offset,
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
