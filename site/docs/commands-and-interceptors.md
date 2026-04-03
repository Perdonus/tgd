# Commands & Interceptors

## Slash command registration

Use `registerCommand()` when you want a named plugin command:

```cpp
_commandId = _host->registerCommand(
	"example.my_plugin",
	{
		.command = "/ping",
		.description = "Ping command",
		.usage = "/ping",
	},
	[=](const Plugins::CommandContext &ctx) {
		Q_UNUSED(ctx);
		_host->showToast("pong");
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Cancel,
		};
	});
```

## `CommandResult`

```cpp
enum class Action {
	Continue,
	Cancel,
	Handled,
	ReplaceText,
};
```

Use:

- `Continue` to let Astrogram continue normal processing
- `Cancel` to stop the original action
- `Handled` to say the plugin took ownership
- `ReplaceText` to rewrite outgoing text

## Outgoing text interception

```cpp
_outgoingId = _host->registerOutgoingTextInterceptor(
	"example.my_plugin",
	[=](const Plugins::OutgoingTextContext &ctx) {
		if (ctx.text.startsWith("/shout ")) {
			return Plugins::CommandResult{
				.action = Plugins::CommandResult::Action::ReplaceText,
				.replacementText = ctx.text.mid(7).toUpper(),
			};
		}
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Continue,
		};
	},
	100);
```

This is what powers plugins like `AI Chat`, where `/ai` can be intercepted before the command is sent into the chat.

## Actions in Settings > Plugins

```cpp
_actionId = _host->registerAction(
	"example.my_plugin",
	"Open popup",
	"Opens a toast",
	[=] {
		_host->showToast("Action called");
	});
```

## Action with context

```cpp
_actionCtxId = _host->registerActionWithContext(
	"example.my_plugin",
	"Context action",
	"Uses active window/session",
	[=](const Plugins::ActionContext &ctx) {
		if (!ctx.window) return;
		_host->showToast("Window is available");
	});
```

## Message observers

```cpp
Plugins::MessageObserverOptions opts;
opts.newMessages = true;
opts.editedMessages = true;
opts.deletedMessages = true;

_observerId = _host->registerMessageObserver(
	"example.my_plugin",
	opts,
	[=](const Plugins::MessageEventContext &ctx) {
		switch (ctx.event) {
		case Plugins::MessageEvent::New: _host->showToast("New"); break;
		case Plugins::MessageEvent::Edited: _host->showToast("Edited"); break;
		case Plugins::MessageEvent::Deleted: _host->showToast("Deleted"); break;
		}
	});
```
