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
#include <QPointer>

#include <algorithm>

namespace AyuRecall {
namespace {

constexpr auto kRowOuterLeft = 16;
constexpr auto kRowOuterRight = 16;
constexpr auto kRowOuterTop = 6;
constexpr auto kRowOuterBottom = 6;
constexpr auto kBubblePadding = QMargins(14, 10, 14, 10);
constexpr auto kBubbleInnerSkip = 4;
constexpr auto kBubbleMinWidth = 120;
constexpr auto kBubbleWidthFactor = 0.72;

[[nodiscard]] bool RussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString UiText(const char *en, const char *ru) {
	return RussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString TitleText(int count) {
	const auto base = UiText("Deleted Messages", "Удалённые сообщения");
	return count > 0 ? (base + u" · "_q + QString::number(count)) : base;
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

[[nodiscard]] QString SectionHint() {
	return UiText(
		"Click a saved message to open its place in the chat.",
		"Нажмите на сохранённое сообщение, чтобы открыть его место в чате.");
}

[[nodiscard]] QString DeletedMessagePlaceholder() {
	return UiText("Saved deleted message", "Сохранённое удалённое сообщение");
}

[[nodiscard]] QString ScopeLabel(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return UiText("Topic: %1", "Тема: %1").arg(topic->title());
	}
	return QString();
}

[[nodiscard]] int TopicIdFor(not_null<Data::Thread*> thread) {
	const auto topic = thread->asTopic();
	return topic ? topic->rootId().bare : 0;
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
	return snapshot.text.contains(query, Qt::CaseInsensitive)
		|| snapshot.senderName.contains(query, Qt::CaseInsensitive)
		|| sender.contains(query, Qt::CaseInsensitive);
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

class DeletedMessageRow final : public Ui::RpWidget {
public:
	DeletedMessageRow(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		AyuMessages::MessageSnapshot snapshot);

	int resizeGetHeight(int newWidth) override;

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
		AyuMessages::MessageSnapshot snapshot)
: Ui::RpWidget(parent)
, _controller(controller)
, _thread(thread)
, _snapshot(std::move(snapshot))
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

	_sender = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(ResolveSenderName(_snapshot, _thread)),
		_senderSt);
	_text = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(_snapshot.text.isEmpty()
			? DeletedMessagePlaceholder()
			: _snapshot.text),
		_textSt);
	_meta = Ui::CreateChild<Ui::FlatLabel>(
		this,
		rpl::single(TimeLabel(DisplayTimestamp(_snapshot))),
		_metaSt);

	_sender->setAttribute(Qt::WA_TransparentForMouseEvents);
	_text->setAttribute(Qt::WA_TransparentForMouseEvents);
	_meta->setAttribute(Qt::WA_TransparentForMouseEvents);
	setCursor(style::cur_pointer);
	setToolTip(OpenHint());
}

int DeletedMessageRow::resizeGetHeight(int newWidth) {
	layoutChildren(newWidth);
	return _bubbleRect.isNull()
		? (kRowOuterTop + kRowOuterBottom)
		: (_bubbleRect.bottom() + kRowOuterBottom + 1);
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
	p.drawRoundedRect(_bubbleRect, st::boxRadius, st::boxRadius);

	if (_hovered) {
		p.setBrush(Qt::NoBrush);
		p.setPen(_outgoing ? st::msgOutDateFg : st::msgInDateFg);
		p.drawRoundedRect(
			_bubbleRect.adjusted(0, 0, -1, -1),
			st::boxRadius,
			st::boxRadius);
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

	const auto senderWidth = std::min(_sender->naturalWidth(), maxTextWidth);
	const auto textNatural = std::min(_text->naturalWidth(), maxTextWidth);
	const auto metaWidth = std::min(_meta->naturalWidth(), maxTextWidth);
	const auto contentWidth = std::max(
		kBubbleMinWidth - kBubblePadding.left() - kBubblePadding.right(),
		std::max(senderWidth, std::max(textNatural, metaWidth)));

	_sender->resizeToWidth(contentWidth);
	_text->resizeToWidth(contentWidth);
	_meta->resizeToWidth(metaWidth);

	const auto bubbleWidth = std::min(
		availableWidth,
		contentWidth + kBubblePadding.left() + kBubblePadding.right());
	const auto bubbleX = _outgoing
		? std::max(kRowOuterLeft, newWidth - kRowOuterRight - bubbleWidth)
		: kRowOuterLeft;
	auto top = kRowOuterTop + kBubblePadding.top();
	const auto left = bubbleX + kBubblePadding.left();

	_sender->moveToLeft(left, top);
	top += _sender->height() + kBubbleInnerSkip;
	_text->moveToLeft(left, top);
	top += _text->height() + kBubbleInnerSkip;
	_meta->moveToLeft(
		bubbleX + bubbleWidth - kBubblePadding.right() - _meta->width(),
		top);
	top += _meta->height() + kBubblePadding.bottom();

	_bubbleRect = QRect(bubbleX, kRowOuterTop, bubbleWidth, top - kRowOuterTop);
}

void DeletedMessageRow::openOriginal() {
	const auto controller = _controller;
	const auto missing = !(_thread->session().data().message(
		FullMsgId(_thread->peer()->id, _snapshot.messageId)));
	const auto params = Window::SectionShow(Window::SectionShow::Way::Forward);
	controller->showThread(_thread, _snapshot.messageId, params);
	if (missing) {
		controller->window().showToast(UiText(
			"Only the locally saved copy is available.",
			"Доступна только локально сохранённая копия."));
	}
}

class DeletedMessagesMemento;

class DeletedMessagesWidget final : public Window::SectionWidget {
public:
	DeletedMessagesWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread);
	~DeletedMessagesWidget() override = default;

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
	void saveState(not_null<DeletedMessagesMemento*> memento);
	void restoreState(not_null<DeletedMessagesMemento*> memento);

	const not_null<Data::Thread*> _thread;
	const not_null<History*> _history;
	std::shared_ptr<Ui::ChatTheme> _theme;
	object_ptr<HistoryView::TopBarWidget> _topBar;
	object_ptr<Ui::PlainShadow> _topBarShadow;
	std::unique_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::VerticalLayout> _content;
	std::vector<AyuMessages::MessageSnapshot> _messages;
	QString _searchQuery;

};

class DeletedMessagesMemento final : public Window::SectionMemento {
public:
	explicit DeletedMessagesMemento(not_null<Data::Thread*> thread)
	: _thread(thread->migrateToOrMe()) {
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
	_searchQuery = std::move(query);
	rebuildContent();
	_scroll->scrollToY(_scroll->scrollTopMax());
}

void DeletedMessagesWidget::rebuildContent() {
	if (!_content) {
		return;
	}

	_messages = LoadMessages(_thread);
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
	_topBar->setCustomTitle(TitleText(int(_messages.size())));
	_content->clear();
	Ui::AddSkip(_content, st::defaultVerticalListSkip);

	const auto scope = ScopeLabel(_thread);
	if (!scope.isEmpty()) {
		Ui::AddDividerText(_content, rpl::single(scope));
		Ui::AddSkip(_content, st::defaultVerticalListSkip / 2);
	}
	const auto hint = _content->add(
		object_ptr<Ui::FlatLabel>(
			_content,
			rpl::single(SectionHint()),
			st::sessionDateLabel),
		st::boxRowPadding);
	hint->setTryMakeSimilarLines(true);
	Ui::AddSkip(_content, st::defaultVerticalListSkip / 2);

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
	for (const auto &message : _messages) {
		const auto stamp = DisplayTimestamp(message);
		const auto day = QDateTime::fromSecsSinceEpoch(stamp).toLocalTime().date();
		if (day != currentDay) {
			currentDay = day;
			Ui::AddSkip(_content, st::defaultVerticalListSkip / 2);
			Ui::AddDividerText(_content, rpl::single(DayLabel(stamp)));
			Ui::AddSkip(_content, st::defaultVerticalListSkip / 2);
		}
		_content->add(
			object_ptr<DeletedMessageRow>(
				_content,
				controller(),
				_thread,
				message));
	}

	Ui::AddSkip(_content, st::defaultVerticalListSkip);
	if (_scroll->width() > 0) {
		_content->resizeToWidth(_scroll->width());
	}
}

void DeletedMessagesWidget::saveState(not_null<DeletedMessagesMemento*> memento) {
	memento->setScrollTop(_scroll->scrollTop());
	memento->setSearchQuery(_searchQuery);
}

void DeletedMessagesWidget::restoreState(not_null<DeletedMessagesMemento*> memento) {
	_searchQuery = memento->searchQuery();
	if (!_searchQuery.isEmpty() && !_topBar->searchMode()) {
		_topBar->toggleSearch(true, anim::type::instant);
	} else if (_searchQuery.isEmpty() && _topBar->searchMode()) {
		_topBar->toggleSearch(false, anim::type::instant);
	}
	if (_topBar->searchQueryCurrent() != _searchQuery) {
		_topBar->searchSetText(_searchQuery);
	}
	rebuildContent();
	const auto target = (memento->scrollTop() >= 0)
		? memento->scrollTop()
		: _scroll->scrollTopMax();
	_scroll->scrollToY(target);
}

} // namespace

[[nodiscard]] std::shared_ptr<Window::SectionMemento> MakeDeletedMessagesSection(
		not_null<Data::Thread*> thread) {
	return std::make_shared<DeletedMessagesMemento>(thread);
}

} // namespace AyuRecall
