/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_manager.h"

#include "core/application.h"
#include "data/data_changes.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "settings.h"
#include "window/window_controller.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLibrary>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace Plugins {
namespace {

constexpr auto kPluginsFolder = "tdata/plugins";
constexpr auto kConfigFile = "tdata/plugins.json";
constexpr auto kEntryName = "TgdPluginEntry";

QString TrimmedCommand(QString command) {
	command = command.trimmed();
	if (command.startsWith('/')) {
		command = command.mid(1);
	}
	return command.trimmed();
}

QString NormalizeCommand(QString command) {
	return TrimmedCommand(std::move(command)).toLower();
}

QString PluginBaseName(const QString &path) {
	const auto info = QFileInfo(path);
	const auto base = info.completeBaseName();
	return base.isEmpty() ? info.fileName() : base;
}

} // namespace

Manager::Manager(QObject *parent) : QObject(parent) {
}

Manager::~Manager() {
	unloadAll();
}

void Manager::start() {
	_pluginsPath = cWorkingDir() + QString::fromLatin1(kPluginsFolder);
	_configPath = cWorkingDir() + QString::fromLatin1(kConfigFile);
	loadConfig();
	scanPlugins();
	_sessionLifetime.destroy();
	Core::App().domain().activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		handleActiveSessionChanged(session);
	}, _sessionLifetime);
}

void Manager::reload() {
	unloadAll();
	loadConfig();
	scanPlugins();
}

std::vector<PluginState> Manager::plugins() const {
	auto result = std::vector<PluginState>();
	result.reserve(_plugins.size());
	for (const auto &plugin : _plugins) {
		result.push_back(plugin.state);
	}
	return result;
}

std::vector<CommandDescriptor> Manager::commandsFor(
		const QString &pluginId) const {
	auto result = std::vector<CommandDescriptor>();
	const auto it = _commandsByPlugin.find(pluginId);
	if (it == end(_commandsByPlugin)) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _commands.find(id);
		if (entry != end(_commands)) {
			result.push_back(entry->descriptor);
		}
	}
	return result;
}

std::vector<ActionState> Manager::actionsFor(const QString &pluginId) const {
	auto result = std::vector<ActionState>();
	const auto it = _actionsByPlugin.find(pluginId);
	if (it == end(_actionsByPlugin)) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _actions.find(id);
		if (entry != end(_actions)) {
			result.push_back({
				.id = entry->id,
				.title = entry->title,
				.description = entry->description,
			});
		}
	}
	return result;
}

std::vector<PanelState> Manager::panelsFor(const QString &pluginId) const {
	auto result = std::vector<PanelState>();
	const auto it = _panelsByPlugin.find(pluginId);
	if (it == end(_panelsByPlugin)) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _panels.find(id);
		if (entry != end(_panels)) {
			result.push_back({
				.id = entry->id,
				.title = entry->descriptor.title,
				.description = entry->descriptor.description,
			});
		}
	}
	return result;
}

bool Manager::triggerAction(ActionId id) {
	const auto it = _actions.find(id);
	if (it == end(_actions)) {
		return false;
	}
	if (it->handlerWithContext) {
		auto context = ActionContext();
		context.window = activeWindow();
		if (!context.window) {
			context.window = Core::App().activePrimaryWindow();
		}
		if (context.window) {
			context.session = context.window->maybeSession();
		}
		if (!context.session) {
			context.session = activeSession();
		}
		it->handlerWithContext(context);
		return true;
	}
	if (it->handler) {
		it->handler();
		return true;
	}
	return false;
}

bool Manager::openPanel(PanelId id) {
	const auto it = _panels.find(id);
	if (it == end(_panels)) {
		return false;
	}
	const auto window = activeWindow()
		? activeWindow()
		: Core::App().activePrimaryWindow();
	if (!window) {
		showToast(u"No active window to show panel."_q);
		return false;
	}
	if (it->handler) {
		it->handler(window);
		return true;
	}
	return false;
}

bool Manager::setEnabled(const QString &pluginId, bool enabled) {
	if (!_pluginIndexById.contains(pluginId)) {
		return false;
	}
	if (enabled) {
		_disabled.remove(pluginId);
	} else {
		_disabled.insert(pluginId);
	}
	saveConfig();
	reload();
	return true;
}

CommandResult Manager::interceptOutgoingText(
		Main::Session *session,
		History *history,
		const TextWithTags &text,
		const Api::SendOptions &options) {
	auto result = CommandResult();
	if (!_outgoingInterceptors.empty()) {
		auto ordered = std::vector<const OutgoingInterceptorEntry*>();
		ordered.reserve(_outgoingInterceptors.size());
		for (auto it = _outgoingInterceptors.cbegin();
			it != end(_outgoingInterceptors);
			++it) {
			ordered.push_back(&it.value());
		}
		std::sort(ordered.begin(), ordered.end(), [](
				const OutgoingInterceptorEntry *a,
				const OutgoingInterceptorEntry *b) {
			if (a->priority != b->priority) {
				return a->priority < b->priority;
			}
			return a->id < b->id;
		});
		const auto context = OutgoingTextContext{
			.session = session,
			.history = history,
			.text = text.text,
			.options = &options,
		};
		for (const auto entry : ordered) {
			if (!entry || !entry->handler) {
				continue;
			}
			const auto handled = entry->handler(context);
			if (handled.action != CommandResult::Action::Continue) {
				return handled;
			}
		}
	}
	const auto full = text.text.trimmed();
	if (full.isEmpty() || !full.startsWith('/')) {
		return result;
	}
	auto end = full.size();
	for (auto i = 1; i < full.size(); ++i) {
		if (full.at(i).isSpace()) {
			end = i;
			break;
		}
	}
	const auto token = full.mid(1, end - 1);
	if (token.isEmpty() || token.contains('@')) {
		return result;
	}
	const auto key = commandKey(token);
	if (key.isEmpty()) {
		return result;
	}
	const auto idIt = _commandIdByName.find(key);
	if (idIt == end(_commandIdByName)) {
		return result;
	}
	const auto entryIt = _commands.find(idIt.value());
	if (entryIt == end(_commands)) {
		return result;
	}
	const auto args = full.mid(end).trimmed();
	const auto command = '/' + key;
	const auto context = CommandContext{
		.session = session,
		.history = history,
		.text = text.text,
		.command = command,
		.args = args,
		.options = &options,
	};
	if (entryIt->handler) {
		return entryIt->handler(context);
	}
	return result;
}

void Manager::notifyWindowCreated(Window::Controller *window) {
	for (const auto &handler : _windowHandlers) {
		if (handler) {
			handler(window);
		}
	}
}

int Manager::apiVersion() const {
	return kApiVersion;
}

QString Manager::pluginsPath() const {
	return _pluginsPath;
}

CommandId Manager::registerCommand(
		const QString &pluginId,
	CommandDescriptor descriptor,
	CommandHandler handler) {
	const auto key = commandKey(descriptor.command);
	if (key.isEmpty() || key.contains('@') || _commandIdByName.contains(key)) {
		return 0;
	}
	descriptor.command = '/' + key;
	const auto id = _nextCommandId++;
	_commands.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.descriptor = std::move(descriptor),
		.handler = std::move(handler),
	});
	_commandIdByName.insert(key, id);
	_commandsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->commandIds.push_back(id);
	}
	return id;
}

void Manager::unregisterCommand(CommandId id) {
	const auto it = _commands.find(id);
	if (it == end(_commands)) {
		return;
	}
	const auto key = commandKey(it->descriptor.command);
	_commandIdByName.remove(key);
	const auto pluginId = it->pluginId;
	_commands.remove(id);
	const auto listIt = _commandsByPlugin.find(pluginId);
	if (listIt != end(_commandsByPlugin)) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_commandsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->commandIds.removeAll(id);
	}
}

ActionId Manager::registerAction(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionHandler handler) {
	if (title.trimmed().isEmpty()) {
		return 0;
	}
	const auto id = _nextActionId++;
	_actions.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.title = title,
		.description = description,
		.handler = std::move(handler),
	});
	_actionsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->actionIds.push_back(id);
	}
	return id;
}

void Manager::unregisterAction(ActionId id) {
	const auto it = _actions.find(id);
	if (it == end(_actions)) {
		return;
	}
	const auto pluginId = it->pluginId;
	_actions.remove(id);
	const auto listIt = _actionsByPlugin.find(pluginId);
	if (listIt != end(_actionsByPlugin)) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_actionsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->actionIds.removeAll(id);
	}
}

ActionId Manager::registerActionWithContext(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionWithContextHandler handler) {
	if (title.trimmed().isEmpty()) {
		return 0;
	}
	const auto id = _nextActionId++;
	_actions.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.title = title,
		.description = description,
		.handler = nullptr,
		.handlerWithContext = std::move(handler),
	});
	_actionsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->actionIds.push_back(id);
	}
	return id;
}

OutgoingInterceptorId Manager::registerOutgoingTextInterceptor(
		const QString &pluginId,
		OutgoingTextHandler handler,
		int priority) {
	if (!handler) {
		return 0;
	}
	const auto id = _nextOutgoingInterceptorId++;
	_outgoingInterceptors.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.priority = priority,
		.handler = std::move(handler),
	});
	_outgoingInterceptorsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->outgoingInterceptorIds.push_back(id);
	}
	return id;
}

void Manager::unregisterOutgoingTextInterceptor(OutgoingInterceptorId id) {
	const auto it = _outgoingInterceptors.find(id);
	if (it == end(_outgoingInterceptors)) {
		return;
	}
	const auto pluginId = it->pluginId;
	_outgoingInterceptors.remove(id);
	const auto listIt = _outgoingInterceptorsByPlugin.find(pluginId);
	if (listIt != end(_outgoingInterceptorsByPlugin)) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_outgoingInterceptorsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->outgoingInterceptorIds.removeAll(id);
	}
}

MessageObserverId Manager::registerMessageObserver(
		const QString &pluginId,
		MessageObserverOptions options,
		MessageEventHandler handler) {
	if (!handler) {
		return 0;
	}
	const auto id = _nextMessageObserverId++;
	_messageObservers.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.options = options,
		.handler = std::move(handler),
	});
	_messageObserversByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->messageObserverIds.push_back(id);
	}
	updateMessageObserverSubscriptions();
	return id;
}

void Manager::unregisterMessageObserver(MessageObserverId id) {
	const auto it = _messageObservers.find(id);
	if (it == end(_messageObservers)) {
		return;
	}
	const auto pluginId = it->pluginId;
	_messageObservers.remove(id);
	const auto listIt = _messageObserversByPlugin.find(pluginId);
	if (listIt != end(_messageObserversByPlugin)) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_messageObserversByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->messageObserverIds.removeAll(id);
	}
	updateMessageObserverSubscriptions();
}

PanelId Manager::registerPanel(
		const QString &pluginId,
		PanelDescriptor descriptor,
		PanelHandler handler) {
	if (descriptor.title.trimmed().isEmpty()) {
		return 0;
	}
	const auto id = _nextPanelId++;
	_panels.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.descriptor = std::move(descriptor),
		.handler = std::move(handler),
	});
	_panelsByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->panelIds.push_back(id);
	}
	return id;
}

void Manager::unregisterPanel(PanelId id) {
	const auto it = _panels.find(id);
	if (it == end(_panels)) {
		return;
	}
	const auto pluginId = it->pluginId;
	_panels.remove(id);
	const auto listIt = _panelsByPlugin.find(pluginId);
	if (listIt != end(_panelsByPlugin)) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_panelsByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->panelIds.removeAll(id);
	}
}

void Manager::showToast(const QString &text) {
	if (const auto window = Core::App().activeWindow()) {
		window->showToast(text);
		return;
	}
	if (const auto primary = Core::App().activePrimaryWindow()) {
		primary->showToast(text);
		return;
	}
	Ui::Toast::Show(text);
}

void Manager::forEachWindow(
		std::function<void(Window::Controller*)> visitor) {
	if (!visitor) {
		return;
	}
	Core::App().forEachWindow([&](not_null<Window::Controller*> window) {
		visitor(window.get());
	});
}

void Manager::onWindowCreated(
		std::function<void(Window::Controller*)> handler) {
	if (handler) {
		_windowHandlers.push_back(std::move(handler));
	}
}

Window::Controller *Manager::activeWindow() const {
	if (const auto window = Core::App().activeWindow()) {
		return window;
	}
	return Core::App().activePrimaryWindow();
}

Main::Session *Manager::activeSession() const {
	if (const auto window = activeWindow()) {
		if (const auto session = window->maybeSession()) {
			return session;
		}
	}
	if (_activeSession) {
		return _activeSession;
	}
	if (Core::App().domain().started()) {
		const auto &account = Core::App().domain().active();
		if (account.sessionExists()) {
			return &account.session();
		}
	}
	return nullptr;
}

void Manager::forEachSession(
		std::function<void(Main::Session*)> visitor) {
	if (!visitor || !Core::App().domain().started()) {
		return;
	}
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (const auto session = account->maybeSession()) {
			visitor(session);
		}
	}
}

void Manager::onSessionActivated(
		std::function<void(Main::Session*)> handler) {
	if (handler) {
		_sessionHandlers.push_back(std::move(handler));
	}
}

void Manager::loadConfig() {
	_disabled.clear();
	QFile file(_configPath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return;
	}
	const auto array = document.object().value(u"disabled"_q).toArray();
	for (const auto &value : array) {
		if (value.isString()) {
			_disabled.insert(value.toString());
		}
	}
}

void Manager::saveConfig() const {
	QDir().mkpath(QFileInfo(_configPath).absolutePath());
	auto list = QStringList(begin(_disabled), end(_disabled));
	list.sort(Qt::CaseInsensitive);
	auto array = QJsonArray();
	for (const auto &id : list) {
		array.push_back(id);
	}
	const auto object = QJsonObject{
		{ u"disabled"_q, array },
	};
	const auto document = QJsonDocument(object);
	QFile file(_configPath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(document.toJson(QJsonDocument::Indented));
	}
}

void Manager::scanPlugins() {
	QDir().mkpath(_pluginsPath);
	auto dir = QDir(_pluginsPath);
	const auto files = dir.entryInfoList(
		{ u"*.tgd"_q },
		QDir::Files,
		QDir::Name | QDir::IgnoreCase);
	for (const auto &info : files) {
		loadPlugin(info.absoluteFilePath());
	}
}

void Manager::loadPlugin(const QString &path) {
	auto record = PluginRecord();
	record.state.path = path;
	record.state.enabled = false;
	record.state.loaded = false;

	auto library = std::make_unique<QLibrary>(path);
	if (!library->load()) {
		record.state.error = library->errorString();
		record.state.info.id = PluginBaseName(path);
		record.state.info.name = record.state.info.id;
		_plugins.push_back(std::move(record));
		return;
	}
	const auto entry = reinterpret_cast<EntryFn>(
		library->resolve(kEntryName));
	if (!entry) {
		record.state.error = u"Missing TgdPluginEntry export."_q;
		record.state.info.id = PluginBaseName(path);
		record.state.info.name = record.state.info.id;
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}

	auto instance = std::unique_ptr<Plugin>(entry(this, kApiVersion));
	if (!instance) {
		record.state.error = u"Plugin entry returned null."_q;
		record.state.info.id = PluginBaseName(path);
		record.state.info.name = record.state.info.id;
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}

	record.state.info = instance->info();
	record.state.info.id = record.state.info.id.trimmed();
	if (record.state.info.name.isEmpty()) {
		record.state.info.name = record.state.info.id;
	}
	if (record.state.info.id.isEmpty()) {
		record.state.error = u"Plugin id is empty."_q;
	}
	if (_pluginIndexById.contains(record.state.info.id)) {
		record.state.error = u"Duplicate plugin id."_q;
	}

	record.state.enabled = !_disabled.contains(record.state.info.id);

	if (!record.state.error.isEmpty()) {
		record.state.enabled = false;
		instance.reset();
		library->unload();
		record.state.loaded = false;
		_plugins.push_back(std::move(record));
		return;
	}

	if (record.state.enabled) {
		record.library = std::move(library);
		record.instance = std::move(instance);
		record.state.loaded = true;
	} else {
		instance.reset();
		library->unload();
	}

	const auto index = int(_plugins.size());
	_plugins.push_back(std::move(record));
	_pluginIndexById.insert(_plugins.back().state.info.id, index);

	if (_plugins.back().state.enabled) {
		_plugins.back().instance->onLoad();
	} else {
		_plugins.back().instance.reset();
		_plugins.back().library.reset();
	}
}

void Manager::unloadAll() {
	for (auto &plugin : _plugins) {
		if (plugin.state.loaded && plugin.instance) {
			plugin.instance->onUnload();
		}
	}
	_commands.clear();
	_commandIdByName.clear();
	_commandsByPlugin.clear();
	_nextCommandId = 1;
	_actions.clear();
	_actionsByPlugin.clear();
	_nextActionId = 1;
	_panels.clear();
	_panelsByPlugin.clear();
	_nextPanelId = 1;
	_outgoingInterceptors.clear();
	_outgoingInterceptorsByPlugin.clear();
	_nextOutgoingInterceptorId = 1;
	_messageObservers.clear();
	_messageObserversByPlugin.clear();
	_nextMessageObserverId = 1;
	_messageObserverLifetime.destroy();
	_windowHandlers.clear();
	_sessionHandlers.clear();

	for (auto &plugin : _plugins) {
		plugin.commandIds.clear();
		plugin.actionIds.clear();
		plugin.panelIds.clear();
		plugin.outgoingInterceptorIds.clear();
		plugin.messageObserverIds.clear();
		plugin.state.loaded = false;
		plugin.instance.reset();
		if (plugin.library) {
			plugin.library->unload();
			plugin.library.reset();
		}
	}
	_plugins.clear();
	_pluginIndexById.clear();
}

PluginRecord *Manager::findRecord(const QString &pluginId) {
	const auto it = _pluginIndexById.find(pluginId);
	if (it == end(_pluginIndexById)) {
		return nullptr;
	}
	const auto index = it.value();
	if (index < 0 || index >= int(_plugins.size())) {
		return nullptr;
	}
	return &_plugins[index];
}

const PluginRecord *Manager::findRecord(const QString &pluginId) const {
	const auto it = _pluginIndexById.find(pluginId);
	if (it == end(_pluginIndexById)) {
		return nullptr;
	}
	const auto index = it.value();
	if (index < 0 || index >= int(_plugins.size())) {
		return nullptr;
	}
	return &_plugins[index];
}

void Manager::unregisterPluginCommands(const QString &pluginId) {
	const auto ids = _commandsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterCommand(id);
	}
}

void Manager::unregisterPluginActions(const QString &pluginId) {
	const auto ids = _actionsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterAction(id);
	}
}

void Manager::unregisterPluginPanels(const QString &pluginId) {
	const auto ids = _panelsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterPanel(id);
	}
}

void Manager::unregisterPluginOutgoingInterceptors(const QString &pluginId) {
	const auto ids = _outgoingInterceptorsByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterOutgoingTextInterceptor(id);
	}
}

void Manager::unregisterPluginMessageObservers(const QString &pluginId) {
	const auto ids = _messageObserversByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterMessageObserver(id);
	}
}

QString Manager::commandKey(const QString &command) const {
	return NormalizeCommand(command);
}

void Manager::updateMessageObserverSubscriptions() {
	_messageObserverLifetime.destroy();
	if (!_activeSession || _messageObservers.isEmpty()) {
		return;
	}
	auto wantsNew = false;
	auto wantsEdited = false;
	auto wantsDeleted = false;
	for (auto it = _messageObservers.cbegin();
		it != end(_messageObservers);
		++it) {
		const auto &options = it.value().options;
		wantsNew |= options.newMessages;
		wantsEdited |= options.editedMessages;
		wantsDeleted |= options.deletedMessages;
		if (wantsNew && wantsEdited && wantsDeleted) {
			break;
		}
	}
	if (wantsNew) {
		_activeSession->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			auto context = MessageEventContext{
				.session = _activeSession,
				.history = update.item->history().get(),
				.item = update.item.get(),
				.event = MessageEvent::New,
			};
			for (auto it = _messageObservers.cbegin();
				it != end(_messageObservers);
				++it) {
				dispatchMessageEvent(
					_activeSession,
					context,
					it.value().options,
					it.value());
			}
		}, _messageObserverLifetime);
	}
	if (wantsEdited) {
		_activeSession->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::Edited
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			auto context = MessageEventContext{
				.session = _activeSession,
				.history = update.item->history().get(),
				.item = update.item.get(),
				.event = MessageEvent::Edited,
			};
			for (auto it = _messageObservers.cbegin();
				it != end(_messageObservers);
				++it) {
				dispatchMessageEvent(
					_activeSession,
					context,
					it.value().options,
					it.value());
			}
		}, _messageObserverLifetime);
	}
	if (wantsDeleted) {
		_activeSession->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::Destroyed
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			auto context = MessageEventContext{
				.session = _activeSession,
				.history = update.item->history().get(),
				.item = update.item.get(),
				.event = MessageEvent::Deleted,
			};
			for (auto it = _messageObservers.cbegin();
				it != end(_messageObservers);
				++it) {
				dispatchMessageEvent(
					_activeSession,
					context,
					it.value().options,
					it.value());
			}
		}, _messageObserverLifetime);
	}
}

void Manager::handleActiveSessionChanged(Main::Session *session) {
	_activeSession = session;
	for (const auto &handler : _sessionHandlers) {
		if (handler) {
			handler(session);
		}
	}
	updateMessageObserverSubscriptions();
}

void Manager::dispatchMessageEvent(
		Main::Session *session,
		const MessageEventContext &context,
		const MessageObserverOptions &options,
		const MessageObserverEntry &entry) {
	if (!entry.handler) {
		return;
	}
	switch (context.event) {
	case MessageEvent::New:
		if (!options.newMessages) {
			return;
		}
		break;
	case MessageEvent::Edited:
		if (!options.editedMessages) {
			return;
		}
		break;
	case MessageEvent::Deleted:
		if (!options.deletedMessages) {
			return;
		}
		break;
	}
	const auto item = context.item;
	if (!item) {
		return;
	}
	const auto outgoing = item->out();
	if (outgoing && !options.outgoing) {
		return;
	}
	if (!outgoing && !options.incoming) {
		return;
	}
	auto callContext = context;
	callContext.session = session;
	entry.handler(callContext);
}

} // namespace Plugins
