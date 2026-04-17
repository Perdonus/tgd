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

#include <QtCore/QPointer>

#include <algorithm>
#include <memory>

namespace HistoryView {
namespace {

constexpr auto kStripHorizontalPadding = 8;
constexpr auto kStripVerticalPadding = 8;
constexpr auto kStripButtonSize = 34;
constexpr auto kStripButtonSpacing = 6;
constexpr auto kStripSeparatorHeight = 1;
constexpr auto kStripMaxButtons = 4;
constexpr auto kStripRadius = 8;

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
		setMouseTracking(true);
		resize(widthForButtons(), heightForButtons());
	}

	[[nodiscard]] int widthForButtons() const {
		const auto count = int(_buttons.size());
		if (!count) {
			return 0;
		}
		return (kStripHorizontalPadding * 2)
			+ (kStripButtonSize * count)
			+ (kStripButtonSpacing * (count - 1));
	}

	[[nodiscard]] int heightForButtons() const {
		return kStripSeparatorHeight
			+ (kStripVerticalPadding * 2)
			+ kStripButtonSize;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.fillRect(e->rect(), st::windowBg);
		p.fillRect(
			QRect(0, 0, width(), kStripSeparatorHeight),
			st::boxDividerBg);
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
	[[nodiscard]] QRect buttonRect(int index) const {
		return QRect(
			kStripHorizontalPadding
				+ (index * (kStripButtonSize + kStripButtonSpacing)),
			kStripSeparatorHeight + kStripVerticalPadding,
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

[[nodiscard]] std::vector<ContextIconStripButton> ResolveButtons(
		const ContextMenuSurfaceLayout &layout,
		const ContextMenuResolvedLayout &resolved) {
	auto available = base::flat_map<QString, const ContextMenuResolvedAction*>();
	for (const auto &action : resolved.actions) {
		if (action.id.isEmpty()
			|| !action.icon
			|| !action.stripEligible
			|| !action.trigger) {
			continue;
		}
		available.emplace(action.id, &action);
	}

	auto result = std::vector<ContextIconStripButton>();
	result.reserve(std::min(kStripMaxButtons, int(layout.strip.size())));
	auto seen = base::flat_set<QString>();
	for (const auto &entry : layout.strip) {
		if (!entry.visible || seen.contains(entry.id)) {
			continue;
		}
		const auto it = available.find(entry.id);
		if (it == available.end()) {
			continue;
		}
		seen.emplace(entry.id);
		result.push_back({
			.id = it->second->id,
			.text = it->second->text,
			.icon = it->second->icon,
			.trigger = it->second->trigger,
		});
		if (int(result.size()) >= kStripMaxButtons) {
			break;
		}
	}
	if (!result.empty()) {
		return result;
	}
	for (const auto &action : resolved.actions) {
		if (action.id.isEmpty()
			|| !action.icon
			|| !action.stripEligible
			|| !action.trigger
			|| seen.contains(action.id)) {
			continue;
		}
		seen.emplace(action.id);
		result.push_back({
			.id = action.id,
			.text = action.text,
			.icon = action.icon,
			.trigger = action.trigger,
		});
		if (int(result.size()) >= kStripMaxButtons) {
			break;
		}
	}
	return result;
}

} // namespace

AttachContextIconStripResult AttachContextIconStripToMenu(
		not_null<Ui::PopupMenu*> menu,
		QPoint desiredPosition,
		const ContextMenuSurfaceLayout &layout,
		const ContextMenuResolvedLayout &resolved) {
	Q_UNUSED(layout);

	const auto customizationLayout = LoadContextMenuCustomizationLayout();
	auto buttons = ResolveButtons(
		LookupContextMenuSurfaceLayout(customizationLayout, resolved.surface),
		resolved);
	if (buttons.empty()) {
		return AttachContextIconStripResult::Skipped;
	}
	if (!menu->prepareGeometryFor(desiredPosition)) {
		return AttachContextIconStripResult::Failed;
	}

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
		const auto desiredHeight = inner.y() + inner.height() + addedHeight;
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
			inner.y() + inner.height(),
			inner.width(),
			addedHeight);
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
