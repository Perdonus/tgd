/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "plugins/plugins_api.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QVector>

#include <rpl/lifetime.h>

#include <memory>
#include <vector>

class QLibrary;

struct TextWithTags;

namespace Api {
struct SendOptions;
} // namespace Api

namespace Window {
class Controller;
} // namespace Window

namespace Plugins {

struct PluginState {
	PluginInfo info;
	QString path;
	bool enabled = false;
	bool loaded = false;
	QString error;
};

struct ActionState {
	ActionId id = 0;
	QString title;
	QString description;
};

struct PanelState {
	PanelId id = 0;
	QString title;
	QString description;
};

class Manager final : public QObject, public Host {
public:
	explicit Manager(QObject *parent = nullptr);
	~Manager() override;

	void start();
	void reload();

	std::vector<PluginState> plugins() const;
	std::vector<CommandDescriptor> commandsFor(
		const QString &pluginId) const;
	std::vector<ActionState> actionsFor(
		const QString &pluginId) const;
	std::vector<PanelState> panelsFor(
		const QString &pluginId) const;
	bool triggerAction(ActionId id);
	bool openPanel(PanelId id);

	bool setEnabled(const QString &pluginId, bool enabled);

	CommandResult interceptOutgoingText(
		Main::Session *session,
		History *history,
		const TextWithTags &text,
		const Api::SendOptions &options);

	void notifyWindowCreated(Window::Controller *window);

	int apiVersion() const override;
	QString pluginsPath() const override;

	CommandId registerCommand(
		const QString &pluginId,
		CommandDescriptor descriptor,
		CommandHandler handler) override;
	void unregisterCommand(CommandId id) override;

	ActionId registerAction(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionHandler handler) override;
	void unregisterAction(ActionId id) override;

	ActionId registerActionWithContext(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionWithContextHandler handler) override;

	OutgoingInterceptorId registerOutgoingTextInterceptor(
		const QString &pluginId,
		OutgoingTextHandler handler,
		int priority) override;
	void unregisterOutgoingTextInterceptor(
		OutgoingInterceptorId id) override;

	MessageObserverId registerMessageObserver(
		const QString &pluginId,
		MessageObserverOptions options,
		MessageEventHandler handler) override;
	void unregisterMessageObserver(MessageObserverId id) override;

	PanelId registerPanel(
		const QString &pluginId,
		PanelDescriptor descriptor,
		PanelHandler handler) override;
	void unregisterPanel(PanelId id) override;

	void showToast(const QString &text) override;
	void forEachWindow(
		std::function<void(Window::Controller*)> visitor) override;
	void onWindowCreated(
		std::function<void(Window::Controller*)> handler) override;
	Window::Controller *activeWindow() const override;
	Main::Session *activeSession() const override;
	void forEachSession(
		std::function<void(Main::Session*)> visitor) override;
	void onSessionActivated(
		std::function<void(Main::Session*)> handler) override;

	private:
		struct WindowHandlerEntry {
			QString pluginId;
			std::function<void(Window::Controller*)> handler;
		};
		struct SessionHandlerEntry {
			QString pluginId;
			std::function<void(Main::Session*)> handler;
		};
		struct CommandEntry {
			CommandId id = 0;
			QString pluginId;
			CommandDescriptor descriptor;
			CommandHandler handler;
	};
	struct ActionEntry {
		ActionId id = 0;
		QString pluginId;
		QString title;
		QString description;
		ActionHandler handler;
		ActionWithContextHandler handlerWithContext;
	};
	struct PanelEntry {
		PanelId id = 0;
		QString pluginId;
		PanelDescriptor descriptor;
		PanelHandler handler;
	};
	struct OutgoingInterceptorEntry {
		OutgoingInterceptorId id = 0;
		QString pluginId;
		int priority = 0;
		OutgoingTextHandler handler;
	};
	struct MessageObserverEntry {
		MessageObserverId id = 0;
		QString pluginId;
		MessageObserverOptions options;
		MessageEventHandler handler;
	};
	struct PluginRecord {
		PluginState state;
		std::unique_ptr<QLibrary> library;
		std::unique_ptr<Plugin> instance;
		QVector<CommandId> commandIds;
		QVector<ActionId> actionIds;
		QVector<PanelId> panelIds;
		QVector<OutgoingInterceptorId> outgoingInterceptorIds;
		QVector<MessageObserverId> messageObserverIds;
	};

	void loadConfig();
	void saveConfig() const;
	void scanPlugins();
	void loadPlugin(const QString &path);
	void unloadAll();
	PluginRecord *findRecord(const QString &pluginId);
	const PluginRecord *findRecord(const QString &pluginId) const;
	void unregisterPluginCommands(const QString &pluginId);
		void unregisterPluginActions(const QString &pluginId);
		void unregisterPluginPanels(const QString &pluginId);
		void unregisterPluginOutgoingInterceptors(const QString &pluginId);
		void unregisterPluginMessageObservers(const QString &pluginId);
		void unregisterPluginWindowHandlers(const QString &pluginId);
		void unregisterPluginSessionHandlers(const QString &pluginId);
		QString commandKey(const QString &command) const;
		bool hasPlugin(const QString &pluginId) const;
		void disablePlugin(const QString &pluginId, const QString &reason);
		void updateMessageObserverSubscriptions();
		void handleActiveSessionChanged(Main::Session *session);
		void dispatchMessageEvent(
			Main::Session *session,
			const MessageEventContext &context,
		const MessageObserverOptions &options,
		const MessageObserverEntry &entry);

	QString _pluginsPath;
	QString _configPath;

	std::vector<PluginRecord> _plugins;
	QHash<QString, int> _pluginIndexById;
	QSet<QString> _disabled;

	QHash<QString, CommandId> _commandIdByName;
	QHash<CommandId, CommandEntry> _commands;
	QHash<QString, QVector<CommandId>> _commandsByPlugin;
	CommandId _nextCommandId = 1;

	QHash<ActionId, ActionEntry> _actions;
	QHash<QString, QVector<ActionId>> _actionsByPlugin;
	ActionId _nextActionId = 1;

	QHash<PanelId, PanelEntry> _panels;
	QHash<QString, QVector<PanelId>> _panelsByPlugin;
	PanelId _nextPanelId = 1;

	QHash<OutgoingInterceptorId, OutgoingInterceptorEntry>
		_outgoingInterceptors;
	QHash<QString, QVector<OutgoingInterceptorId>>
		_outgoingInterceptorsByPlugin;
	OutgoingInterceptorId _nextOutgoingInterceptorId = 1;

	QHash<MessageObserverId, MessageObserverEntry> _messageObservers;
	QHash<QString, QVector<MessageObserverId>> _messageObserversByPlugin;
	MessageObserverId _nextMessageObserverId = 1;

		std::vector<WindowHandlerEntry> _windowHandlers;
		std::vector<SessionHandlerEntry> _sessionHandlers;
		QString _registeringPluginId;
		rpl::lifetime _sessionLifetime;
		rpl::lifetime _messageObserverLifetime;
		Main::Session *_activeSession = nullptr;
	};

} // namespace Plugins
