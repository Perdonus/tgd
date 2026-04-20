# Common Classes

These are the most important host-visible classes and pointers you will encounter in plugin callbacks.

## `Window::Controller`

Available from:

- `ActionContext.window`
- `Host::activeWindow()`
- `Host::forEachWindow()`
- `Host::onWindowCreated()`

Use it when you need a real Astrogram window controller instead of a raw widget.

## `QWidget`

Available from:

- `Host::activeWindowWidget()`
- `Host::forEachWindowWidget()`
- `Host::onWindowWidgetCreated()`

Use it for visual manipulation such as opacity or palette changes.

## `Main::Session`

Available from:

- `CommandContext.session`
- `ActionContext.session`
- `Host::activeSession()`
- `Host::forEachSession()`
- `Host::onSessionActivated()`

Use it for account/session-aware logic.

## `History` and `HistoryItem`

Available from:

- `CommandContext.history`
- `OutgoingTextContext.history`
- `MessageEventContext.history`
- `MessageEventContext.item`

Use them for chat/message observers, command routing, or tooling around edits/deletes.

## `Api::SendOptions`

Available as a pointer inside:

- `CommandContext.options`
- `OutgoingTextContext.options`

Useful when you need to respect the context of how a message would be sent.
