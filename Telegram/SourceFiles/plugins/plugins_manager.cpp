/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_manager.h"

#include "core/application.h"
#include "core/launcher.h"
#include "data/data_changes.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "window/window_controller.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"

#include <QtCore/QDir>
#include <QtCore/QDateTime>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QLibrary>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSysInfo>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <exception>

namespace Plugins {
namespace {

constexpr auto kPluginsFolder = "tdata/plugins";
constexpr auto kConfigFile = "tdata/plugins.json";
constexpr auto kLogFile = "tdata/plugins.log";
constexpr auto kSafeModeFile = "tdata/plugins.safe-mode";
constexpr auto kBinaryInfoName = "TgdPluginBinaryInfo";
constexpr auto kPreviewInfoName = "TgdPluginPreviewInfo";
constexpr auto kEntryName = "TgdPluginEntry";
constexpr auto kNoPluginsArgument = "-noplugins";

QString ProcessUserName() {
#if defined(Q_OS_WIN)
	return qEnvironmentVariable("USERNAME");
#else
	return qEnvironmentVariable("USER");
#endif
}

int PhysicalCpuCoresFallback() {
	const auto logical = QThread::idealThreadCount();
	return logical > 0 ? logical : 1;
}

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

bool IsValidCommandKey(const QString &key) {
	if (key.isEmpty()) {
		return false;
	}
	for (const auto ch : key) {
		if (ch.isSpace() || ch == '/' || ch == '\\' || ch == '@') {
			return false;
		}
	}
	return true;
}

QString PluginBaseName(const QString &path) {
	const auto info = QFileInfo(path);
	const auto base = info.completeBaseName();
	return base.isEmpty() ? info.fileName() : base;
}

QString PreviewText(const char *value) {
	return value
		? QString::fromUtf8(value).trimmed()
		: QString();
}

PluginInfo PluginInfoFromPreview(const Plugins::PreviewInfo &preview) {
	return {
		.id = PreviewText(preview.id),
		.name = PreviewText(preview.name),
		.version = PreviewText(preview.version),
		.author = PreviewText(preview.author),
		.description = PreviewText(preview.description),
		.website = PreviewText(preview.website),
	};
}

void MergePluginInfo(PluginInfo &target, const PluginInfo &preview) {
	if (!preview.id.isEmpty()) {
		target.id = preview.id;
	}
	if (!preview.name.isEmpty()) {
		target.name = preview.name;
	}
	if (!preview.version.isEmpty()) {
		target.version = preview.version;
	}
	if (!preview.author.isEmpty()) {
		target.author = preview.author;
	}
	if (!preview.description.isEmpty()) {
		target.description = preview.description;
	}
	if (!preview.website.isEmpty()) {
		target.website = preview.website;
	}
}

QString PreviewIconFromInfo(const Plugins::PreviewInfo &preview) {
	return PreviewText(preview.icon);
}

QString ReadManifestField(const QByteArray &data, int *offset, bool *ok) {
	if (!offset || !ok || *offset < 0 || *offset >= data.size()) {
		if (ok) {
			*ok = false;
		}
		return QString();
	}
	const auto end = data.indexOf('\0', *offset);
	if (end < 0) {
		*ok = false;
		return QString();
	}
	const auto value = QString::fromUtf8(
		data.constData() + *offset,
		end - *offset).trimmed();
	*offset = end + 1;
	*ok = true;
	return value;
}

bool ReadPreviewManifest(
		const QString &path,
		PluginInfo *info,
		QString *icon) {
	if (!info || !icon) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto data = file.readAll();
	const auto marker = QByteArray(TGD_PLUGIN_MANIFEST_MAGIC) + '\0';
	const auto start = data.indexOf(marker);
	if (start < 0) {
		return false;
	}
	auto offset = start + marker.size();
	auto ok = false;
	const auto id = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto name = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto version = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto author = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto description = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto website = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto parsedIcon = ReadManifestField(data, &offset, &ok);
	if (!ok) {
		return false;
	}
	const auto endMarker = ReadManifestField(data, &offset, &ok);
	if (!ok || endMarker != QString::fromLatin1(TGD_PLUGIN_MANIFEST_END)) {
		return false;
	}
	info->id = id;
	info->name = name;
	info->version = version;
	info->author = author;
	info->description = description;
	info->website = website;
	*icon = parsedIcon;
	return true;
}

QString SanitizedPluginFileStem(QString value) {
	value = value.trimmed();
	for (auto &ch : value) {
		if (ch.isLetterOrNumber() || ch == '.' || ch == '_' || ch == '-') {
			continue;
		}
		ch = '-';
	}
	while (value.contains(u"--"_q)) {
		value.replace(u"--"_q, u"-"_q);
	}
	value = value.trimmed();
	value.remove(QChar('/'));
	value.remove(QChar('\\'));
	value.remove(QChar(':'));
	value.remove(QChar('*'));
	value.remove(QChar('?'));
	value.remove(QChar('"'));
	value.remove(QChar('<'));
	value.remove(QChar('>'));
	value.remove(QChar('|'));
	return value.isEmpty() ? u"plugin"_q : value;
}

bool CommandLineSafeModeRequested() {
	const auto &arguments = Core::Launcher::Instance().arguments();
	return arguments.contains(QString::fromLatin1(kNoPluginsArgument));
}

QString CurrentExceptionText() {
	try {
		throw;
	} catch (const std::exception &e) {
		const auto message = QString::fromUtf8(e.what()).trimmed();
		return message.isEmpty()
			? u"std::exception"_q
			: message;
	} catch (...) {
		return u"Unknown exception."_q;
	}
}

QString DescribeBinaryInfoMismatch(const Plugins::BinaryInfo &info) {
	const auto pluginCompiler = QString::fromLatin1(info.compiler ? info.compiler : "unknown");
	const auto hostCompiler = QString::fromLatin1(Plugins::kCompilerId);
	const auto pluginPlatform = QString::fromLatin1(info.platform ? info.platform : "unknown");
	const auto hostPlatform = QString::fromLatin1(Plugins::kPlatformId);

	if (info.structVersion != Plugins::kBinaryInfoVersion) {
		return u"Incompatible plugin metadata version."_q;
	}
	if (info.apiVersion != Plugins::kApiVersion) {
		return u"Incompatible plugin API version."_q;
	}
	if (info.pointerSize != int(sizeof(void*))) {
		return u"Incompatible plugin architecture."_q;
	}
	if (pluginPlatform != hostPlatform) {
		return u"Incompatible plugin platform."_q;
	}
	if (pluginCompiler != hostCompiler
		|| info.compilerVersion != Plugins::kCompilerVersion) {
		return u"Incompatible plugin compiler ABI."_q;
	}
	if (info.qtMajor != QT_VERSION_MAJOR || info.qtMinor != QT_VERSION_MINOR) {
		return u"Incompatible Qt major/minor version."_q;
	}
	return QString();
}

QString DescribeBinaryInfo(const Plugins::BinaryInfo &info) {
	return QString::fromLatin1(
		"api=%1 ptr=%2 qt=%3.%4 compiler=%5/%6 platform=%7")
		.arg(info.apiVersion)
		.arg(info.pointerSize)
		.arg(info.qtMajor)
		.arg(info.qtMinor)
		.arg(QString::fromLatin1(info.compiler ? info.compiler : "unknown"))
		.arg(info.compilerVersion)
		.arg(QString::fromLatin1(info.platform ? info.platform : "unknown"));
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
	_logPath = cWorkingDir() + QString::fromLatin1(kLogFile);
	_safeModePath = cWorkingDir() + QString::fromLatin1(kSafeModeFile);
	loadConfig();
	if (safeModeEnabled()) {
		appendLogLine(u"[safe-mode] Plugins skipped at startup."_q);
	} else {
		scanPlugins();
	}
	_sessionLifetime.destroy();
	Core::App().domain().activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		handleActiveSessionChanged(session);
	}, _sessionLifetime);
}

void Manager::reload() {
	unloadAll();
	loadConfig();
	if (safeModeEnabled()) {
		appendLogLine(u"[safe-mode] Plugins skipped on reload."_q);
		return;
	}
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

bool Manager::safeModeEnabled() const {
	if (CommandLineSafeModeRequested()) {
		return true;
	}
	return !_safeModePath.isEmpty() && QFileInfo::exists(_safeModePath);
}

bool Manager::setSafeModeEnabled(bool enabled) {
	if (_safeModePath.isEmpty()) {
		return false;
	}
	QDir().mkpath(QFileInfo(_safeModePath).absolutePath());
	if (enabled) {
		QFile file(_safeModePath);
		if (!file.exists()) {
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				return false;
			}
			file.close();
		}
	} else if (QFileInfo::exists(_safeModePath) && !QFile::remove(_safeModePath)) {
		return false;
	}
	reload();
	return true;
}

PackagePreviewState Manager::inspectPackage(const QString &path) const {
	auto result = PackagePreviewState();
	result.sourcePath = path;
	result.info.id = PluginBaseName(path);
	result.info.name = result.info.id;
	result.compatible = false;

	const auto fileInfo = QFileInfo(path);
	if (!fileInfo.exists() || !fileInfo.isFile()) {
		result.error = u"Plugin package file was not found."_q;
		return result;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		result.error = u"Could not read the plugin package."_q;
		return result;
	}
	if (file.size() <= 0) {
		result.error = u"Plugin package is empty."_q;
		return result;
	}
	file.close();
	auto previewInfo = PluginInfo();
	auto previewIcon = QString();
	if (ReadPreviewManifest(path, &previewInfo, &previewIcon)) {
		result.previewAvailable = true;
		MergePluginInfo(result.info, previewInfo);
		result.icon = previewIcon;
	}
	result.compatible = true;

	if (result.info.id.isEmpty()) {
		result.info.id = PluginBaseName(path);
	}
	if (result.info.name.isEmpty()) {
		result.info.name = result.info.id;
	}
	if (!result.info.id.isEmpty()) {
		if (const auto installed = findRecord(result.info.id)) {
			result.installed = true;
			result.update = true;
			result.installedVersion = installed->state.info.version;
			result.installedPath = installed->state.path;
		}
	}
	return result;
}

bool Manager::installPackage(const QString &sourcePath, QString *error) {
	const auto preview = inspectPackage(sourcePath);
	if (!preview.compatible) {
		if (error) {
			*error = preview.error.isEmpty()
				? u"Plugin package is incompatible."_q
				: preview.error;
		}
		return false;
	}

	QDir().mkpath(_pluginsPath);
	const auto sourceInfo = QFileInfo(sourcePath);
	const auto fileStem = !preview.info.id.isEmpty()
		? SanitizedPluginFileStem(preview.info.id)
		: SanitizedPluginFileStem(sourceInfo.completeBaseName());
	const auto targetPath = QDir(_pluginsPath).absoluteFilePath(
		fileStem + u".tgd"_q);
	const auto tempPath = targetPath + u".tmp"_q;
	const auto rescanNow = [this] {
		if (safeModeEnabled()) {
			appendLogLine(u"[safe-mode] Plugins skipped after install attempt."_q);
		} else {
			scanPlugins();
		}
	};

	unloadAll();
	loadConfig();

	QFile::remove(tempPath);

	if (sourceInfo.absoluteFilePath() != targetPath) {
		if (!QFile::copy(sourcePath, tempPath)) {
			rescanNow();
			if (error) {
				*error = u"Could not copy the plugin package into tdata/plugins."_q;
			}
			return false;
		}
		if (!preview.installedPath.isEmpty()
			&& preview.installedPath != targetPath
			&& QFileInfo::exists(preview.installedPath)
			&& !QFile::remove(preview.installedPath)) {
			QFile::remove(tempPath);
			rescanNow();
			if (error) {
				*error = u"Could not replace the previous plugin file."_q;
			}
			return false;
		}
		if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath)) {
			QFile::remove(tempPath);
			rescanNow();
			if (error) {
				*error = u"Could not overwrite the target plugin file."_q;
			}
			return false;
		}
		if (!QFile::rename(tempPath, targetPath)) {
			QFile::remove(tempPath);
			rescanNow();
			if (error) {
				*error = u"Could not finalize the installed plugin file."_q;
			}
			return false;
		}
	}

	if (safeModeEnabled()) {
		appendLogLine(
			u"[install] "_q
			+ QFileInfo(targetPath).fileName()
			+ u" copied while plugin safe mode is enabled."_q);
	} else {
		scanPlugins();
		if (!preview.info.id.isEmpty()) {
			if (const auto record = findRecord(preview.info.id);
				record && !record->state.error.trimmed().isEmpty()) {
				if (error) {
					*error = record->state.error.trimmed();
				}
				return false;
			}
		}
	}
	if (error) {
		error->clear();
	}
	return true;
}

std::vector<CommandDescriptor> Manager::commandsFor(
		const QString &pluginId) const {
	auto result = std::vector<CommandDescriptor>();
	const auto it = _commandsByPlugin.find(pluginId);
	if (it == _commandsByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _commands.find(id);
		if (entry != _commands.end()) {
			result.push_back(entry->descriptor);
		}
	}
	return result;
}

std::vector<ActionState> Manager::actionsFor(const QString &pluginId) const {
	auto result = std::vector<ActionState>();
	const auto it = _actionsByPlugin.find(pluginId);
	if (it == _actionsByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _actions.find(id);
		if (entry != _actions.end()) {
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
	if (it == _panelsByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _panels.find(id);
		if (entry != _panels.end()) {
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
	if (it == _actions.end()) {
		return false;
	}
	try {
		if (it->handlerWithContext) {
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = it->pluginId;
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
			_registeringPluginId = previousPluginId;
			return true;
		}
		if (it->handler) {
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = it->pluginId;
			it->handler();
			_registeringPluginId = previousPluginId;
			return true;
		}
	} catch (...) {
		_registeringPluginId.clear();
		disablePlugin(it->pluginId, u"Action failed: "_q + CurrentExceptionText());
		showToast(u"Plugin action failed and was disabled."_q);
	}
	return false;
}

bool Manager::openPanel(PanelId id) {
	const auto it = _panels.find(id);
	if (it == _panels.end()) {
		return false;
	}
	const auto window = activeWindow()
		? activeWindow()
		: Core::App().activePrimaryWindow();
	if (!window) {
		showToast(u"No active window to show panel."_q);
		return false;
	}
	if (!it->handler) {
		return false;
	}
	try {
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = it->pluginId;
		it->handler(window);
		_registeringPluginId = previousPluginId;
		return true;
	} catch (...) {
		_registeringPluginId.clear();
		disablePlugin(it->pluginId, u"Panel failed: "_q + CurrentExceptionText());
		showToast(u"Plugin panel failed and was disabled."_q);
		return false;
	}
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
		auto ordered = std::vector<OutgoingInterceptorEntry>();
		ordered.reserve(_outgoingInterceptors.size());
		for (auto it = _outgoingInterceptors.cbegin();
			it != _outgoingInterceptors.cend();
			++it) {
			ordered.push_back(it.value());
		}
		std::sort(ordered.begin(), ordered.end(), [](
				const OutgoingInterceptorEntry &a,
				const OutgoingInterceptorEntry &b) {
			if (a.priority != b.priority) {
				return a.priority < b.priority;
			}
			return a.id < b.id;
		});
		const auto context = OutgoingTextContext{
			.session = session,
			.history = history,
			.text = text.text,
			.options = &options,
		};
		for (const auto &entry : ordered) {
			if (!entry.handler) {
				continue;
			}
			try {
				const auto previousPluginId = _registeringPluginId;
				_registeringPluginId = entry.pluginId;
				const auto handled = entry.handler(context);
				_registeringPluginId = previousPluginId;
				if (handled.action != CommandResult::Action::Continue) {
					return handled;
				}
			} catch (...) {
				_registeringPluginId.clear();
				disablePlugin(
					entry.pluginId,
					u"Outgoing interceptor failed: "_q + CurrentExceptionText());
				showToast(u"Plugin interceptor failed and was disabled."_q);
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
	if (key.isEmpty() || !IsValidCommandKey(key)) {
		return result;
	}
	const auto idIt = _commandIdByName.find(key);
	if (idIt == _commandIdByName.end()) {
		return result;
	}
	const auto entryIt = _commands.find(idIt.value());
	if (entryIt == _commands.end()) {
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
		try {
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entryIt->pluginId;
			const auto handled = entryIt->handler(context);
			_registeringPluginId = previousPluginId;
			return handled;
		} catch (...) {
			_registeringPluginId.clear();
			disablePlugin(
				entryIt->pluginId,
				u"Command handler failed: "_q + CurrentExceptionText());
			showToast(u"Plugin command failed and was disabled."_q);
			return {
				.action = CommandResult::Action::Cancel,
			};
		}
	}
	return result;
}

void Manager::notifyWindowCreated(Window::Controller *window) {
	const auto handlers = _windowHandlers;
	for (const auto &entry : handlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			entry.handler(window);
			_registeringPluginId = previousPluginId;
		} catch (...) {
			_registeringPluginId.clear();
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Window callback failed: "_q + CurrentExceptionText());
			}
			showToast(u"Plugin window callback failed."_q);
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
	if (!hasPlugin(pluginId) || !handler) {
		return 0;
	}
	const auto key = commandKey(descriptor.command);
	if (!IsValidCommandKey(key) || _commandIdByName.contains(key)) {
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
	if (it == _commands.end()) {
		return;
	}
	const auto key = commandKey(it->descriptor.command);
	_commandIdByName.remove(key);
	const auto pluginId = it->pluginId;
	_commands.remove(id);
	const auto listIt = _commandsByPlugin.find(pluginId);
	if (listIt != _commandsByPlugin.end()) {
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
	if (!hasPlugin(pluginId) || !handler || title.trimmed().isEmpty()) {
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
	if (it == _actions.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_actions.remove(id);
	const auto listIt = _actionsByPlugin.find(pluginId);
	if (listIt != _actionsByPlugin.end()) {
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
	if (!hasPlugin(pluginId) || !handler || title.trimmed().isEmpty()) {
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
	if (!hasPlugin(pluginId) || !handler) {
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
	if (it == _outgoingInterceptors.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_outgoingInterceptors.remove(id);
	const auto listIt = _outgoingInterceptorsByPlugin.find(pluginId);
	if (listIt != _outgoingInterceptorsByPlugin.end()) {
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
	if (!hasPlugin(pluginId) || !handler) {
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
	if (it == _messageObservers.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_messageObservers.remove(id);
	const auto listIt = _messageObserversByPlugin.find(pluginId);
	if (listIt != _messageObserversByPlugin.end()) {
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
	if (!hasPlugin(pluginId) || !handler || descriptor.title.trimmed().isEmpty()) {
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
	if (it == _panels.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_panels.remove(id);
	const auto listIt = _panelsByPlugin.find(pluginId);
	if (listIt != _panelsByPlugin.end()) {
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
		_windowHandlers.push_back({
			.pluginId = _registeringPluginId,
			.handler = std::move(handler),
		});
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
		_sessionHandlers.push_back({
			.pluginId = _registeringPluginId,
			.handler = std::move(handler),
		});
	}
}

HostInfo Manager::hostInfo() const {
	auto info = HostInfo();
	info.compiler = QString::fromLatin1(kCompilerId);
	info.platform = QString::fromLatin1(kPlatformId);
	info.workingPath = cWorkingDir();
	info.pluginsPath = _pluginsPath;
	info.safeModeEnabled = safeModeEnabled();
	info.runtimeApiEnabled = false;
	info.runtimeApiPort = 0;
	return info;
}

SystemInfo Manager::systemInfo() const {
	auto info = SystemInfo();
	const auto locale = QLocale::system();
	info.processId = quint64(QCoreApplication::applicationPid());
	info.logicalCpuCores = std::max(QThread::idealThreadCount(), 1);
	info.physicalCpuCores = PhysicalCpuCoresFallback();
	info.productType = QSysInfo::productType();
	info.productVersion = QSysInfo::productVersion();
	info.prettyProductName = QSysInfo::prettyProductName();
	info.kernelType = QSysInfo::kernelType();
	info.kernelVersion = QSysInfo::kernelVersion();
	info.architecture = QSysInfo::currentCpuArchitecture();
	info.buildAbi = QSysInfo::buildAbi();
	info.hostName = QSysInfo::machineHostName();
	info.userName = ProcessUserName();
	info.locale = locale.name();
	const auto uiLanguages = locale.uiLanguages();
	info.uiLanguage = uiLanguages.isEmpty() ? QString() : uiLanguages.value(0);
	info.timeZone = QString::fromLatin1(QTimeZone::systemTimeZoneId());
	return info;
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
	auto list = _disabled.values();
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

void Manager::appendLogLine(const QString &line) const {
	if (_logPath.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(_logPath).absolutePath());
	QFile file(_logPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		return;
	}
	const auto stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
	file.write((stamp + u" "_q + line.trimmed() + u"\n"_q).toUtf8());
}

void Manager::logLoadFailure(const QString &path, const QString &reason) const {
	appendLogLine(
		u"[load-failed] "_q
		+ QFileInfo(path).fileName()
		+ u" ("_q
		+ path
		+ u"): "_q
		+ reason);
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
	record.state.info.id = PluginBaseName(path);
	record.state.info.name = record.state.info.id;

	auto library = std::make_unique<QLibrary>(path);
	if (!library->load()) {
		record.state.error = library->errorString();
		logLoadFailure(path, record.state.error);
		_plugins.push_back(std::move(record));
		return;
	}
	const auto previewInfo = reinterpret_cast<PreviewInfoFn>(
		library->resolve(kPreviewInfoName));
	if (previewInfo) {
		if (const auto preview = previewInfo();
			preview
			&& preview->structVersion == kPreviewInfoVersion
			&& preview->apiVersion == kApiVersion) {
			MergePluginInfo(record.state.info, PluginInfoFromPreview(*preview));
			if (record.state.info.name.isEmpty()) {
				record.state.info.name = record.state.info.id;
			}
		}
	}
	const auto entry = reinterpret_cast<EntryFn>(
		library->resolve(kEntryName));
	const auto binaryInfo = reinterpret_cast<BinaryInfoFn>(
		library->resolve(kBinaryInfoName));
	if (!binaryInfo) {
		record.state.error = u"Missing TgdPluginBinaryInfo export."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}
	const auto info = binaryInfo();
	if (!info) {
		record.state.error = u"Plugin binary metadata is null."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}
	if (const auto mismatch = DescribeBinaryInfoMismatch(*info);
		!mismatch.isEmpty()) {
		record.state.error = mismatch;
		logLoadFailure(path, mismatch + u" ["_q + DescribeBinaryInfo(*info) + u"]"_q);
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}
	if (!entry) {
		record.state.error = u"Missing TgdPluginEntry export."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}

	auto instance = std::unique_ptr<Plugin>();
	try {
		instance.reset(entry(this, kApiVersion));
	} catch (...) {
		record.state.error = u"Plugin entry failed: "_q + CurrentExceptionText();
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}
	if (!instance) {
		record.state.error = u"Plugin entry returned null."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}

	try {
		record.state.info = instance->info();
	} catch (...) {
		record.state.error = u"Plugin info() failed: "_q + CurrentExceptionText();
		logLoadFailure(path, record.state.error);
		instance.reset();
		library->unload();
		_plugins.push_back(std::move(record));
		return;
	}
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
		logLoadFailure(path, record.state.error);
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
		_registeringPluginId = _plugins.back().state.info.id;
		try {
			_plugins.back().instance->onLoad();
		} catch (...) {
			const auto pluginId = _plugins.back().state.info.id;
			_registeringPluginId.clear();
			disablePlugin(
				pluginId,
				u"onLoad failed: "_q + CurrentExceptionText());
			showToast(u"Plugin failed to load and was disabled."_q);
			return;
		}
		_registeringPluginId.clear();
	} else {
		_plugins.back().instance.reset();
		_plugins.back().library.reset();
	}
}

void Manager::unloadAll() {
	for (auto &plugin : _plugins) {
		if (plugin.state.loaded && plugin.instance) {
			_registeringPluginId = plugin.state.info.id;
			try {
				plugin.instance->onUnload();
			} catch (...) {
				plugin.state.error = u"onUnload failed: "_q + CurrentExceptionText();
			}
			_registeringPluginId.clear();
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

Manager::PluginRecord *Manager::findRecord(const QString &pluginId) {
	const auto it = _pluginIndexById.find(pluginId);
	if (it == _pluginIndexById.end()) {
		return nullptr;
	}
	const auto index = it.value();
	if (index < 0 || index >= int(_plugins.size())) {
		return nullptr;
	}
	return &_plugins[index];
}

const Manager::PluginRecord *Manager::findRecord(
		const QString &pluginId) const {
	const auto it = _pluginIndexById.find(pluginId);
	if (it == _pluginIndexById.end()) {
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

void Manager::unregisterPluginWindowHandlers(const QString &pluginId) {
	_windowHandlers.erase(
		std::remove_if(
			_windowHandlers.begin(),
			_windowHandlers.end(),
			[&](const WindowHandlerEntry &entry) {
				return entry.pluginId == pluginId;
			}),
		_windowHandlers.end());
}

void Manager::unregisterPluginSessionHandlers(const QString &pluginId) {
	_sessionHandlers.erase(
		std::remove_if(
			_sessionHandlers.begin(),
			_sessionHandlers.end(),
			[&](const SessionHandlerEntry &entry) {
				return entry.pluginId == pluginId;
			}),
		_sessionHandlers.end());
}

QString Manager::commandKey(const QString &command) const {
	return NormalizeCommand(command);
}

bool Manager::hasPlugin(const QString &pluginId) const {
	if (pluginId.trimmed().isEmpty()) {
		return false;
	}
	return _pluginIndexById.contains(pluginId);
}

void Manager::disablePlugin(const QString &pluginId, const QString &reason) {
	auto *record = findRecord(pluginId);
	if (!record) {
		return;
	}
	appendLogLine(
		u"[disabled] "_q
		+ pluginId
		+ (reason.trimmed().isEmpty()
			? QString()
			: (u": "_q + reason.trimmed())));
	record->state.enabled = false;
	record->state.loaded = false;
	if (!reason.trimmed().isEmpty()) {
		record->state.error = reason.trimmed();
	}
	unregisterPluginCommands(pluginId);
	unregisterPluginActions(pluginId);
	unregisterPluginPanels(pluginId);
	unregisterPluginOutgoingInterceptors(pluginId);
	unregisterPluginMessageObservers(pluginId);
	unregisterPluginWindowHandlers(pluginId);
	unregisterPluginSessionHandlers(pluginId);
	if (record->instance) {
		_registeringPluginId = pluginId;
		try {
			record->instance->onUnload();
		} catch (...) {
			// Keep the original failure reason.
		}
		_registeringPluginId.clear();
		record->instance.reset();
	}
	if (record->library) {
		record->library->unload();
		record->library.reset();
	}
	_disabled.insert(pluginId);
	saveConfig();
}

void Manager::updateMessageObserverSubscriptions() {
	_messageObserverLifetime.destroy();
	if (!_activeSession || _messageObservers.isEmpty()) {
		return;
	}
	const auto observerSnapshot = [=] {
		auto result = std::vector<MessageObserverEntry>();
		result.reserve(_messageObservers.size());
		for (auto it = _messageObservers.cbegin();
			it != _messageObservers.cend();
			++it) {
			result.push_back(it.value());
		}
		return result;
	};
	auto wantsNew = false;
	auto wantsEdited = false;
	auto wantsDeleted = false;
	for (auto it = _messageObservers.cbegin();
		it != _messageObservers.cend();
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
			const auto snapshot = observerSnapshot();
			for (const auto &entry : snapshot) {
				dispatchMessageEvent(
					_activeSession,
					context,
					entry.options,
					entry);
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
			const auto snapshot = observerSnapshot();
			for (const auto &entry : snapshot) {
				dispatchMessageEvent(
					_activeSession,
					context,
					entry.options,
					entry);
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
			const auto snapshot = observerSnapshot();
			for (const auto &entry : snapshot) {
				dispatchMessageEvent(
					_activeSession,
					context,
					entry.options,
					entry);
			}
		}, _messageObserverLifetime);
	}
}

void Manager::handleActiveSessionChanged(Main::Session *session) {
	_activeSession = session;
	const auto handlers = _sessionHandlers;
	for (const auto &entry : handlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			entry.handler(session);
			_registeringPluginId = previousPluginId;
		} catch (...) {
			_registeringPluginId.clear();
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Session callback failed: "_q + CurrentExceptionText());
			}
			showToast(u"Plugin session callback failed."_q);
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
	try {
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = entry.pluginId;
		entry.handler(callContext);
		_registeringPluginId = previousPluginId;
	} catch (...) {
		_registeringPluginId.clear();
		disablePlugin(
			entry.pluginId,
			u"Message observer failed: "_q + CurrentExceptionText());
		showToast(u"Plugin observer failed and was disabled."_q);
	}
}

} // namespace Plugins
