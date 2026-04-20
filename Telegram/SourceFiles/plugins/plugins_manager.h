/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "plugins/plugins_api.h"

#include <QtCore/QHash>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QStringList>
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

class QTcpServer;
class QTcpSocket;

namespace Plugins {

struct PluginState {
	PluginInfo info;
	QString path;
	QString sha256;
	bool enabled = false;
	bool loaded = false;
	QString error;
	bool disabledByRecovery = false;
	bool recoverySuspected = false;
	QString recoveryReason;
	bool sourceVerified = false;
	QString sourceTrustText;
	QString sourceTrustDetails;
	QString sourceTrustReason;
	int64 sourceChannelId = 0;
	int64 sourceMessageId = 0;
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

struct SettingsPageState {
	SettingsPageId id = 0;
	QString title;
	QString description;
	QVector<SettingsSectionDescriptor> sections;
};

struct PackagePreviewState {
	PluginInfo info;
	QString sourcePath;
	QString icon;
	QString error;
	QString installedVersion;
	QString installedPath;
	bool compatible = false;
	bool previewAvailable = false;
	bool installed = false;
	bool update = false;
};

struct RecoveryOperationState {
	bool active = false;
	QString kind;
	QStringList pluginIds;
	QString details;
	QString startedAt;
};

struct TraceOperationState {
	quint64 id = 0;
	QString kind;
	QStringList pluginIds;
	QString details;
	QDateTime startedAt;
};

class Manager final : public QObject, public Host {
public:
	explicit Manager(QObject *parent = nullptr);
	~Manager() override;

	void start();
	void reload();

	std::vector<PluginState> plugins() const;
	bool safeModeEnabled() const;
	bool setSafeModeEnabled(bool enabled);
	bool runtimeApiEnabled() const;
	int runtimeApiPort() const;
	bool setRuntimeApiEnabled(bool enabled);
	PackagePreviewState inspectPackage(const QString &path) const;
	bool installPackage(const QString &sourcePath, QString *error = nullptr);
	bool removePlugin(const QString &pluginId, QString *error = nullptr);

	std::vector<CommandDescriptor> commandsFor(
		const QString &pluginId) const;
	std::vector<ActionState> actionsFor(
		const QString &pluginId) const;
	std::vector<PanelState> panelsFor(
		const QString &pluginId) const;
	std::vector<SettingsPageState> settingsPagesFor(
		const QString &pluginId) const;
	bool triggerAction(ActionId id);
	bool openPanel(PanelId id);
	bool updateSetting(SettingsPageId id, SettingDescriptor setting);

	bool setEnabled(const QString &pluginId, bool enabled);

	CommandResult interceptOutgoingText(
		Main::Session *session,
		History *history,
		const TextWithTags &text,
		const Api::SendOptions &options);

	void notifyWindowCreated(Window::Controller *window);

	int apiVersion() const override;
	QString pluginsPath() const override;
	QJsonValue storedSettingValue(
		const QString &pluginId,
		const QString &settingId) const override;

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

	SettingsPageId registerSettingsPage(
		const QString &pluginId,
		SettingsPageDescriptor descriptor,
		SettingsChangedHandler handler) override;
	void unregisterSettingsPage(SettingsPageId id) override;

	void showToast(const QString &text) override;
	void forEachWindow(
		std::function<void(Window::Controller*)> visitor) override;
	void onWindowCreated(
		std::function<void(Window::Controller*)> handler) override;
	void forEachWindowWidget(
		std::function<void(QWidget*)> visitor) override;
	void onWindowWidgetCreated(
		std::function<void(QWidget*)> handler) override;
	Window::Controller *activeWindow() const override;
	QWidget *activeWindowWidget() const override;
	bool settingBoolValue(
		const QString &pluginId,
		const QString &settingId,
		bool fallback) const override;
	int settingIntValue(
		const QString &pluginId,
		const QString &settingId,
		int fallback) const override;
	QString settingStringValue(
		const QString &pluginId,
		const QString &settingId,
		const QString &fallback) const override;
	Main::Session *activeSession() const override;
	void forEachSession(
		std::function<void(Main::Session*)> visitor) override;
	void onSessionActivated(
		std::function<void(Main::Session*)> handler) override;
	HostInfo hostInfo() const override;
	SystemInfo systemInfo() const override;

private:
	struct WindowHandlerEntry {
		QString pluginId;
		std::function<void(Window::Controller*)> handler;
	};
	struct WindowWidgetHandlerEntry {
		QString pluginId;
		std::function<void(QWidget*)> handler;
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
	struct SettingsPageEntry {
		SettingsPageId id = 0;
		QString pluginId;
		SettingsPageDescriptor descriptor;
		SettingsChangedHandler handler;
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
		QVector<SettingsPageId> settingsPageIds;
		QVector<OutgoingInterceptorId> outgoingInterceptorIds;
		QVector<MessageObserverId> messageObserverIds;
	};

	void loadConfig();
	void saveConfig() const;
	void appendLogLine(const QString &line) const;
	void appendTraceLine(const QByteArray &line) const;
	void writeLogRecord(
		const QString &path,
		const QByteArray &record,
		bool jsonLog) const;
	bool rotateLogFileIfNeeded(
		const QString &path,
		qsizetype recordSize,
		bool jsonLog) const;
	QJsonObject makeLogEvent(
		QString phase,
		QString event,
		QJsonObject details = {}) const;
	QString formatLogEventText(const QJsonObject &event) const;
	void logEvent(
		QString phase,
		QString event,
		QJsonObject details = {}) const;
	void logLoadFailure(const QString &path, const QString &reason) const;
	void logOperationStart(
		const QString &kind,
		const QStringList &pluginIds,
		const QString &details);
	void logOperationFinish(
		const QString &result = QString(),
		const QString &reason = QString());
	quint64 currentOperationId() const;
	QJsonObject pluginInfoToJson(const PluginInfo &info) const;
	QJsonObject pluginStateToJson(const PluginState &state) const;
	QJsonObject commandDescriptorToJson(
		const CommandDescriptor &descriptor) const;
	QJsonObject panelDescriptorToJson(
		const PanelDescriptor &descriptor) const;
	QJsonObject settingDescriptorToJson(
		const SettingDescriptor &descriptor) const;
	QJsonObject settingsPageDescriptorToJson(
		const SettingsPageDescriptor &descriptor) const;
	QJsonObject sendOptionsToJson(
		const Api::SendOptions *options) const;
	QJsonObject binaryInfoToJson(const BinaryInfo &info) const;
	QJsonObject fileInfoToJson(const QFileInfo &info) const;
	QJsonObject messageContextToJson(
		const MessageEventContext &context) const;
	QJsonObject commandResultToJson(const CommandResult &result) const;
	QJsonObject registrationSummaryToJson(
		const PluginRecord &record) const;
	QString fileSha256(const QString &path) const;
	void applyStoredSettings(
		const QString &pluginId,
		SettingsPageDescriptor &descriptor) const;
	void rememberSettingValue(
		const QString &pluginId,
		const SettingDescriptor &descriptor);
	int findRecordIndex(const QString &pluginId) const;
	void rebuildPluginIndex();
	void moveLastPluginRecordToIndex(int index);
	void syncSourceTrustState(PluginState &state) const;
	void scanPlugins(bool metadataOnly = false);
	void loadPluginMetadataOnly(const QString &path);
	void loadPlugin(const QString &path);
	void unloadPluginRecord(
		PluginRecord &record,
		bool preserveLoadError = false);
	void unloadAll();
	PluginRecord *findRecord(const QString &pluginId);
	const PluginRecord *findRecord(const QString &pluginId) const;
	bool removePluginFileReliable(
		const QString &path,
		QString *finalPath,
		QString *error,
		bool *scheduledOnReboot);
	void unregisterPluginCommands(const QString &pluginId);
	void unregisterPluginActions(const QString &pluginId);
	void unregisterPluginPanels(const QString &pluginId);
	void unregisterPluginSettingsPages(const QString &pluginId);
	void unregisterPluginOutgoingInterceptors(const QString &pluginId);
	void unregisterPluginMessageObservers(const QString &pluginId);
	void unregisterPluginWindowHandlers(const QString &pluginId);
	void unregisterPluginWindowWidgetHandlers(const QString &pluginId);
	void unregisterPluginSessionHandlers(const QString &pluginId);
	QString commandKey(const QString &command) const;
	bool hasPlugin(const QString &pluginId) const;
	void disablePlugin(const QString &pluginId, const QString &reason);
	void disablePlugin(
		const QString &pluginId,
		const QString &reason,
		bool disabledByRecovery,
		const QString &recoveryReason);
	void loadRecoveryState();
	void saveRecoveryState() const;
	void recoverFromPendingState();
	void startRecoveryOperation(
		QString kind,
		QStringList pluginIds = {},
		QString details = QString());
	void finishRecoveryOperation();
	void syncRecoveryFlags(PluginState &state) const;
	void clearRecoveryDisabled(const QString &pluginId);
	void queueRecoveryNotice(
		QString kind,
		QStringList pluginIds,
		QString details);
	void showRecoveryNotice(Window::Controller *window);
	QStringList describeRecoveryPlugins(
		const QStringList &pluginIds) const;
	QString composeRecoveryClipboardText() const;
	void updateMessageObserverSubscriptions();
	void handleActiveSessionChanged(Main::Session *session);
	void dispatchMessageEvent(
		Main::Session *session,
		const MessageEventContext &context,
		const MessageObserverOptions &options,
		const MessageObserverEntry &entry);
	void ensureRuntimeApiCliHelper();

	void ensureRuntimeApiServerState();
	bool startRuntimeApiServer();
	void stopRuntimeApiServer();
	void onRuntimeApiNewConnection();
	void onRuntimeApiSocketReadyRead(QTcpSocket *socket);
	QByteArray processRuntimeApiRequest(
		const QString &method,
		const QString &target,
		const QByteArray &body,
		bool &disableRuntimeApiAfterResponse);

	QString _pluginsPath;
	QString _configPath;
	QString _logPath;
	QString _tracePath;
	QString _safeModePath;
	QString _recoveryPath;
	bool _runtimeApiEnabled = false;
	int _runtimeApiPort = 37080;
	std::unique_ptr<QTcpServer> _runtimeApiServer;
	QHash<QTcpSocket*, QByteArray> _runtimeApiBuffers;
	bool _reloadInProgress = false;
	bool _reloadQueued = false;

	std::vector<PluginRecord> _plugins;
	QHash<QString, int> _pluginIndexById;
	QSet<QString> _disabled;
	QHash<QString, QJsonObject> _storedSettings;
	QSet<QString> _disabledByRecovery;
	RecoveryOperationState _recoveryPending;
	RecoveryOperationState _recoveryNotice;
	bool _recoveryNoticeShown = false;
	QVector<TraceOperationState> _traceOperations;
	quint64 _nextTraceOperationId = 1;

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

	QHash<SettingsPageId, SettingsPageEntry> _settingsPages;
	QHash<QString, QVector<SettingsPageId>> _settingsPagesByPlugin;
	SettingsPageId _nextSettingsPageId = 1;

	QHash<OutgoingInterceptorId, OutgoingInterceptorEntry> _outgoingInterceptors;
	QHash<QString, QVector<OutgoingInterceptorId>> _outgoingInterceptorsByPlugin;
	OutgoingInterceptorId _nextOutgoingInterceptorId = 1;

	QHash<MessageObserverId, MessageObserverEntry> _messageObservers;
	QHash<QString, QVector<MessageObserverId>> _messageObserversByPlugin;
	MessageObserverId _nextMessageObserverId = 1;

	std::vector<WindowHandlerEntry> _windowHandlers;
	std::vector<WindowWidgetHandlerEntry> _windowWidgetHandlers;
	std::vector<SessionHandlerEntry> _sessionHandlers;
	QString _registeringPluginId;
	rpl::lifetime _sessionLifetime;
	rpl::lifetime _messageObserverLifetime;
	Main::Session *_activeSession = nullptr;
};

} // namespace Plugins
