/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_context_icon_strip.h"

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "ui/widgets/popup_menu.h"
#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "styles/style_chat.h"
#include "styles/style_boxes.h"

#include <QtWidgets/QAction>
#include <QtCore/QPointer>

#include <algorithm>
#include <memory>

namespace HistoryView {
namespace {

constexpr auto kStripHorizontalPadding = 10;
constexpr auto kStripVerticalPadding = 8;
constexpr auto kStripButtonSize = 34;
constexpr auto kStripButtonSpacing = 8;
constexpr auto kStripMaxButtons = 4;
constexpr auto kStripRadius = 11;
constexpr auto kStripGap = 0;
constexpr auto kContextMenuActionIdProperty[] = "_astro_context_action_id";

struct ContextIconStripButton {
	QString id;
	QString text;
	const style::icon *icon = nullptr;
	std::shared_ptr<Fn<void()>> trigger;
};

class ContextIconStrip final : public Ui::RpWidget {
public:
	ContextIconStrip(
		not_null<QWidget*> parent,
		not_null<Ui::PopupMenu*> menu,
		std::vector<ContextIconStripButton> buttons)
	: Ui::RpWidget(parent)
	, _menu(menu)
	, _buttons(std::move(buttons)) {
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_NoSystemBackground);
		setMouseTracking(true);
		resize(minimumWidthForButtons(), heightForButtons());
	}

	[[nodiscard]] int minimumWidthForButtons() const {
		const auto count = int(_buttons.size());
		if (!count) {
			return 0;
		}
		return (kStripHorizontalPadding * 2)
			+ (kStripButtonSize * count)
			+ (kStripButtonSpacing * (count - 1));
	}

	[[nodiscard]] int heightForButtons() const {
		return (kStripVerticalPadding * 2)
			+ kStripButtonSize;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto panel = panelRect().adjusted(0, 0, -1, -1);
		p.setPen(QPen(st::boxDividerBg, 1.0));
		p.setBrush(st::windowBg);
		p.drawRoundedRect(
			panel,
			kStripRadius + 1,
			kStripRadius + 1);
		for (auto i = 0, count = int(_buttons.size()); i != count; ++i) {
			const auto rect = buttonRect(i);
			const auto hovered = (i == _hovered);
			const auto pressed = (i == _pressed);
			if (hovered || pressed) {
				p.setPen(Qt::NoPen);
				p.setBrush(pressed ? st::windowBgRipple : st::windowBgOver);
				p.drawRoundedRect(rect, kStripRadius, kStripRadius);
			}
			if (const auto icon = _buttons[i].icon) {
				icon->paintInCenter(p, rect);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		updateHovered(e->pos());
	}

	void leaveEventHook(QEvent *e) override {
		updateState(-1, _pressed);
		Ui::RpWidget::leaveEventHook(e);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			e->ignore();
			return;
		}
		const auto index = lookupButton(e->pos());
		updateState(index, index);
		if (index >= 0) {
			e->accept();
		} else {
			e->ignore();
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			e->ignore();
			return;
		}
		const auto index = lookupButton(e->pos());
		const auto trigger = (index >= 0 && index == _pressed)
			? index
			: -1;
		updateState(index, -1);
		if (trigger < 0) {
			e->ignore();
			return;
		}
		e->accept();
		const auto callback = _buttons[trigger].trigger;
		if (_menu) {
			_menu->hideMenu();
		}
		if (callback) {
			(*callback)();
		}
	}

private:
	[[nodiscard]] int buttonsWidth() const {
		return minimumWidthForButtons() - (kStripHorizontalPadding * 2);
	}

	[[nodiscard]] QRect panelRect() const {
		const auto panelWidth = std::min(width(), minimumWidthForButtons());
		return QRect(
			std::max(0, (width() - panelWidth) / 2),
			0,
			panelWidth,
			heightForButtons());
	}

	[[nodiscard]] QRect buttonRect(int index) const {
		const auto panel = panelRect();
		return QRect(
			panel.x()
				+ kStripHorizontalPadding
				+ (index * (kStripButtonSize + kStripButtonSpacing)),
			kStripVerticalPadding,
			kStripButtonSize,
			kStripButtonSize);
	}

	[[nodiscard]] int lookupButton(QPoint position) const {
		for (auto i = 0, count = int(_buttons.size()); i != count; ++i) {
			if (buttonRect(i).contains(position)) {
				return i;
			}
		}
		return -1;
	}

	void updateHovered(QPoint position) {
		updateState(lookupButton(position), _pressed);
	}

	void updateState(int hovered, int pressed) {
		if (_hovered == hovered && _pressed == pressed) {
			return;
		}
		_hovered = hovered;
		_pressed = pressed;
		update();
	}

	const QPointer<Ui::PopupMenu> _menu;
	const std::vector<ContextIconStripButton> _buttons;
	int _hovered = -1;
	int _pressed = -1;
};

[[nodiscard]] QString ContextMenuActionId(QAction *action) {
	return action
		? action->property(kContextMenuActionIdProperty).toString().trimmed()
		: QString();
}

[[nodiscard]] std::vector<ContextIconStripButton> ResolveButtons(
		const ContextMenuSurfaceLayout &layout,
		const ContextMenuResolvedLayout &resolved) {
	auto available = base::flat_map<QString, const ContextMenuResolvedAction*>();
	for (const auto &action : resolved.actions) {
		if (action.id.isEmpty()
			|| !action.icon
			|| !action.trigger) {
			continue;
		}
		available.emplace(action.id, &action);
	}

	auto result = std::vector<ContextIconStripButton>();
	result.reserve(kStripMaxButtons);
	auto seen = base::flat_set<QString>();
	const auto appendEntry = [&](const ContextMenuLayoutEntry &entry) {
		if (!entry.visible || entry.id.isEmpty() || seen.contains(entry.id)) {
			return false;
		}
		const auto it = available.find(entry.id);
		if (it == available.end()) {
			return false;
		}
		seen.emplace(entry.id);
		result.push_back({
			.id = it->second->id,
			.text = it->second->text,
			.icon = it->second->icon,
			.trigger = it->second->trigger,
		});
		return int(result.size()) >= kStripMaxButtons;
	};
	for (const auto &entry : layout.strip) {
		if (appendEntry(entry)) {
			return result;
		}
	}
	if (!result.empty()) {
		return result;
	}
	for (const auto &entry : layout.menu) {
		if (appendEntry(entry)) {
			return result;
		}
	}
	return result;
}

void HideStripActionsInMenu(
		not_null<Ui::PopupMenu*> menu,
		const std::vector<ContextIconStripButton> &buttons) {
	auto stripIds = base::flat_set<QString>();
	for (const auto &button : buttons) {
		if (!button.id.isEmpty()) {
			stripIds.emplace(button.id);
		}
	}
	if (stripIds.empty()) {
		return;
	}
	for (const auto action : menu->actions()) {
		if (!action || !action->isVisible()) {
			continue;
		}
		const auto id = ContextMenuActionId(action);
		if (!id.isEmpty() && stripIds.contains(id)) {
			action->setVisible(false);
		}
	}
	auto seenVisibleAction = false;
	QAction *pendingSeparator = nullptr;
	for (const auto action : menu->actions()) {
		if (!action || !action->isVisible()) {
			continue;
		}
		if (action->isSeparator()) {
			if (!seenVisibleAction || pendingSeparator) {
				action->setVisible(false);
			} else {
				pendingSeparator = action;
			}
			continue;
		}
		pendingSeparator = nullptr;
		seenVisibleAction = true;
	}
	if (pendingSeparator) {
		pendingSeparator->setVisible(false);
	}
}

} // namespace

void MarkContextMenuAction(QAction *action, const QString &id) {
	if (!action || id.isEmpty()) {
		return;
	}
	action->setProperty(kContextMenuActionIdProperty, id);
}

std::vector<QString> ResolveContextIconStripActionIds(
		const ContextMenuSurfaceLayout &layout,
		const ContextMenuResolvedLayout &resolved) {
	const auto buttons = ResolveButtons(layout, resolved);
	auto result = std::vector<QString>();
	result.reserve(buttons.size());
	for (const auto &button : buttons) {
		if (!button.id.isEmpty()) {
			result.push_back(button.id);
		}
	}
	return result;
}

AttachContextIconStripResult AttachContextIconStripToMenu(
		not_null<Ui::PopupMenu*> menu,
		QPoint desiredPosition,
		const ContextMenuSurfaceLayout &layout,
		const ContextMenuResolvedLayout &resolved) {
	auto buttons = ResolveButtons(layout, resolved);
	if (buttons.empty()) {
		return AttachContextIconStripResult::Skipped;
	}
	if (!menu->prepareGeometryFor(desiredPosition)) {
		return AttachContextIconStripResult::Failed;
	}
	HideStripActionsInMenu(menu, buttons);
	menu->prepareGeometryFor(desiredPosition);

	const auto strip = Ui::CreateChild<ContextIconStrip>(
		menu.get(),
		menu,
		std::move(buttons));
	const auto addedHeight = strip->heightForButtons();
	const auto origin = menu->preparedOrigin();
	const auto expandDown = (origin == Ui::PanelAnimation::Origin::TopLeft)
		|| (origin == Ui::PanelAnimation::Origin::TopRight);
	const auto applyGeometry = [=] {
		const auto inner = menu->menu()->geometry();
		const auto desiredHeight = inner.y() + inner.height() + kStripGap + addedHeight;
		if (menu->height() < desiredHeight) {
			const auto add = desiredHeight - menu->height();
			const auto updated = menu->geometry().marginsAdded({
				0,
				expandDown ? 0 : add,
				0,
				expandDown ? add : 0,
			});
			menu->setFixedSize(updated.size());
			menu->setGeometry(updated);
		}
		strip->setGeometry(
			inner.x(),
			inner.y() + inner.height() + kStripGap,
			inner.width(),
			strip->heightForButtons());
	};
	applyGeometry();
	strip->show();

	menu->showStateValue(
	) | rpl::on_next([=](Ui::PopupMenu::ShowState) {
		applyGeometry();
	}, strip->lifetime());

	menu->animatePhaseValue(
	) | rpl::on_next([strip](Ui::PopupMenu::AnimatePhase phase) {
		if (phase == Ui::PopupMenu::AnimatePhase::StartHide) {
			strip->hide();
		}
	}, strip->lifetime());

	return AttachContextIconStripResult::Attached;
}

} // namespace HistoryView
