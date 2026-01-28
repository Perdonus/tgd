/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <cstdint>
#include <functional>

namespace Api {
struct SendOptions;
} // namespace Api

namespace Main {
class Session;
} // namespace Main

namespace Window {
class Controller;
} // namespace Window

class History;
class HistoryItem;

namespace Plugins {

constexpr int kApiVersion = 2;

struct PluginInfo {
	QString id;
	QString name;
	QString version;
	QString author;
	QString description;
	QString website;
};

struct CommandDescriptor {
	QString command;
	QString description;
	QString usage;
};

struct CommandContext {
	Main::Session *session = nullptr;
	History *history = nullptr;
	QString text;
	QString command;
	QString args;
	const Api::SendOptions *options = nullptr;
};

struct CommandResult {
	enum class Action {
		Continue,
		Cancel,
		Handled,
		ReplaceText,
	};
	Action action = Action::Continue;
	QString replacementText;
};

using CommandHandler = std::function<CommandResult(const CommandContext &)>;
using CommandId = uint64_t;

using ActionHandler = std::function<void()>;
using ActionId = uint64_t;

struct ActionContext {
	Window::Controller *window = nullptr;
	Main::Session *session = nullptr;
};

using ActionWithContextHandler = std::function<void(const ActionContext &)>;

struct OutgoingTextContext {
	Main::Session *session = nullptr;
	History *history = nullptr;
	QString text;
	const Api::SendOptions *options = nullptr;
};

using OutgoingTextHandler = std::function<CommandResult(
	const OutgoingTextContext &)>;
using OutgoingInterceptorId = uint64_t;

struct MessageObserverOptions {
	bool newMessages = true;
	bool editedMessages = false;
	bool deletedMessages = false;
	bool incoming = true;
	bool outgoing = true;
};

enum class MessageEvent {
	New,
	Edited,
	Deleted,
};

struct MessageEventContext {
	Main::Session *session = nullptr;
	History *history = nullptr;
	HistoryItem *item = nullptr;
	MessageEvent event = MessageEvent::New;
};

using MessageEventHandler = std::function<void(const MessageEventContext &)>;
using MessageObserverId = uint64_t;

struct PanelDescriptor {
	QString title;
	QString description;
};

using PanelHandler = std::function<void(Window::Controller*)>;
using PanelId = uint64_t;

class Host {
public:
	virtual ~Host() = default;

	virtual int apiVersion() const = 0;
	virtual QString pluginsPath() const = 0;

	virtual CommandId registerCommand(
		const QString &pluginId,
		CommandDescriptor descriptor,
		CommandHandler handler) = 0;
	virtual void unregisterCommand(CommandId id) = 0;

	virtual ActionId registerAction(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionHandler handler) = 0;
	virtual void unregisterAction(ActionId id) = 0;

	virtual ActionId registerActionWithContext(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionWithContextHandler handler) = 0;

	virtual OutgoingInterceptorId registerOutgoingTextInterceptor(
		const QString &pluginId,
		OutgoingTextHandler handler,
		int priority) = 0;
	virtual void unregisterOutgoingTextInterceptor(
		OutgoingInterceptorId id) = 0;

	virtual MessageObserverId registerMessageObserver(
		const QString &pluginId,
		MessageObserverOptions options,
		MessageEventHandler handler) = 0;
	virtual void unregisterMessageObserver(MessageObserverId id) = 0;

	virtual PanelId registerPanel(
		const QString &pluginId,
		PanelDescriptor descriptor,
		PanelHandler handler) = 0;
	virtual void unregisterPanel(PanelId id) = 0;

	virtual void showToast(const QString &text) = 0;
	virtual void forEachWindow(
		std::function<void(Window::Controller*)> visitor) = 0;
	virtual void onWindowCreated(
		std::function<void(Window::Controller*)> handler) = 0;

	virtual Window::Controller *activeWindow() const = 0;
	virtual Main::Session *activeSession() const = 0;
	virtual void forEachSession(
		std::function<void(Main::Session*)> visitor) = 0;
	virtual void onSessionActivated(
		std::function<void(Main::Session*)> handler) = 0;
};

class Plugin {
public:
	virtual ~Plugin() = default;
	virtual PluginInfo info() const = 0;
	virtual void onLoad() {
	}
	virtual void onUnload() {
	}
};

using EntryFn = Plugin* (*)(Host *host, int apiVersion);

} // namespace Plugins

#if defined(_WIN32)
#define TGD_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define TGD_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define TGD_PLUGIN_EXPORT
#endif

#define TGD_PLUGIN_ENTRY \
	extern "C" TGD_PLUGIN_EXPORT Plugins::Plugin *TgdPluginEntry( \
		Plugins::Host *host, \
		int apiVersion)
