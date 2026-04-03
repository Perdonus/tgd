# Window & Session API

Astrogram exposes both controller-level and widget-level window hooks.

## Window controllers

```cpp
_host->forEachWindow([&](Window::Controller *window) {
	Q_UNUSED(window);
});

_host->onWindowCreated([=](Window::Controller *window) {
	Q_UNUSED(window);
	_host->showToast("Window created");
});
```

Use controller-level callbacks when you need a session-aware window object.

## Window widgets

```cpp
_host->forEachWindowWidget([=](QWidget *widget) {
	if (widget && widget->isWindow()) {
		widget->setWindowOpacity(0.9);
	}
});

_host->onWindowWidgetCreated([=](QWidget *widget) {
	if (widget && widget->isWindow()) {
		widget->setWindowOpacity(0.9);
	}
});
```

This is the preferred path for appearance plugins like `transparent_telegram`, because it works with real window widgets instead of guessing random Qt top-levels.

## Example: applying opacity safely

```cpp
void TransparentPlugin::applyToWidget(QWidget *widget) {
	if (!widget || !widget->isWindow()) {
		return;
	}
	if (widget->windowType() != Qt::Window) {
		return;
	}
	widget->setWindowOpacity(_interfaceOpacity / 100.0);
}

void TransparentPlugin::bindWindows() {
	_host->forEachWindowWidget([=](QWidget *widget) {
		applyToWidget(widget);
	});

	_windowWidgetId = _host->onWindowWidgetCreated([=](QWidget *widget) {
		applyToWidget(widget);
	});
}
```

## Active window and session

```cpp
if (auto *window = _host->activeWindow()) {
	// Use Window::Controller
}

if (auto *widget = _host->activeWindowWidget()) {
	// Use QWidget
}

if (auto *session = _host->activeSession()) {
	// Session-bound logic
}
```

## Session activation

```cpp
_host->onSessionActivated([=](Main::Session *session) {
	Q_UNUSED(session);
	_host->showToast("Session activated");
});
```

## Example: session-aware behavior

```cpp
_sessionActivatedId = _host->onSessionActivated([=](Main::Session *session) {
	if (!session) {
		return;
	}
	refreshForSession(session);
});

if (auto *session = _host->activeSession()) {
	refreshForSession(session);
}
```

## Practical advice

- Prefer `forEachWindowWidget()` for visual effects.
- Prefer `activeSession()` or `forEachSession()` for chat/session-aware logic.
- Be conservative with widget mutation. A plugin still runs in-process.
- Treat callbacks as runtime hooks, not as a place to do heavy blocking work.
