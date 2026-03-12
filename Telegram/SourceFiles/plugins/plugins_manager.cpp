/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_manager.h"

#include "boxes/abstract_box.h"
#include "api/api_common.h"
#include "core/application.h"
#include "core/launcher.h"
#include "core/version.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "history/history_item.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QDateTime>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLibrary>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSysInfo>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <exception>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace Plugins {
namespace {

constexpr auto kPluginsFolder = "tdata/plugins";
constexpr auto kConfigFile = "tdata/plugins.json";
constexpr auto kLogFile = "tdata/plugins.log";
constexpr auto kTraceFile = "tdata/plugins.trace.jsonl";
constexpr auto kSafeModeFile = "tdata/plugins.safe-mode";
constexpr auto kRecoveryFile = "tdata/plugins.recovery.json";
constexpr auto kBinaryInfoName = "TgdPluginBinaryInfo";
constexpr auto kPreviewInfoName = "TgdPluginPreviewInfo";
constexpr auto kEntryName = "TgdPluginEntry";
constexpr auto kNoPluginsArgument = "-noplugins";
constexpr auto kRecoveryDuckSize = 72;
constexpr auto kMaxPluginLogBytes = qsizetype(25 * 1024 * 1024);
constexpr auto kMaxPluginLogBackups = 5;
constexpr auto kRuntimeApiHost = "127.0.0.1";
constexpr auto kRuntimeApiAuthHeader = "x-tgd-token";

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

[[nodiscard]] QString RuntimeApiText(QString en, QString ru) {
	return PluginUiText(std::move(en), std::move(ru));
}

QString ProcessUserName() {
#if defined(Q_OS_WIN)
	return qEnvironmentVariable("USERNAME");
#else
	return qEnvironmentVariable("USER");
#endif
}

std::pair<quint64, quint64> SystemMemoryInfo() {
#if defined(Q_OS_WIN)
	auto memory = MEMORYSTATUSEX();
	memory.dwLength = sizeof(memory);
	if (GlobalMemoryStatusEx(&memory)) {
		return {
			quint64(memory.ullTotalPhys),
			quint64(memory.ullAvailPhys),
		};
	}
#elif defined(Q_OS_LINUX)
	auto info = sysinfo();
	if (sysinfo(&info) == 0) {
		const auto unit = quint64(info.mem_unit ? info.mem_unit : 1);
		return {
			quint64(info.totalram) * unit,
			quint64(info.freeram) * unit,
		};
	}
#endif
	return { 0, 0 };
}

int PhysicalCpuCoresFallback() {
	const auto logical = QThread::idealThreadCount();
	return logical > 0 ? logical : 1;
}

QString HttpStatusText(int statusCode) {
	switch (statusCode) {
	case 200:
		return u"OK"_q;
	case 400:
		return u"Bad Request"_q;
	case 401:
		return u"Unauthorized"_q;
	case 404:
		return u"Not Found"_q;
	case 405:
		return u"Method Not Allowed"_q;
	case 409:
		return u"Conflict"_q;
	case 500:
		return u"Internal Server Error"_q;
	default:
		return u"Error"_q;
	}
}

[[nodiscard]] QString RecoveryOperationText(const QString &kind) {
	if (kind == u"install"_q) {
		return PluginUiText(u"plugin install"_q, u"установки плагина"_q);
	} else if (kind == u"update"_q) {
		return PluginUiText(u"plugin update"_q, u"обновления плагина"_q);
	} else if (kind == u"reload"_q) {
		return PluginUiText(u"plugin reload"_q, u"перезагрузки плагинов"_q);
	} else if (kind == u"enable"_q) {
		return PluginUiText(u"plugin enable"_q, u"включения плагина"_q);
	} else if (kind == u"disable"_q) {
		return PluginUiText(u"plugin disable"_q, u"выключения плагина"_q);
	} else if (kind == u"load"_q) {
		return PluginUiText(u"plugin loading"_q, u"загрузки плагина"_q);
	} else if (kind == u"onload"_q) {
		return PluginUiText(u"plugin startup"_q, u"запуска плагина"_q);
	} else if (kind == u"unload"_q) {
		return PluginUiText(u"plugin unload"_q, u"выгрузки плагина"_q);
	} else if (kind == u"command"_q) {
		return PluginUiText(u"plugin command"_q, u"команды плагина"_q);
	} else if (kind == u"action"_q) {
		return PluginUiText(u"plugin action"_q, u"действия плагина"_q);
	} else if (kind == u"panel"_q) {
		return PluginUiText(u"plugin panel"_q, u"панели плагина"_q);
	} else if (kind == u"window"_q) {
		return PluginUiText(
			u"plugin window callback"_q,
			u"оконного callback плагина"_q);
	} else if (kind == u"session"_q) {
		return PluginUiText(
			u"plugin session callback"_q,
			u"session callback плагина"_q);
	} else if (kind == u"outgoing"_q) {
		return PluginUiText(
			u"outgoing interceptor"_q,
			u"перехватчика исходящего текста"_q);
	} else if (kind == u"observer"_q) {
		return PluginUiText(
			u"message observer"_q,
			u"наблюдателя сообщений"_q);
	}
	return PluginUiText(u"plugin operation"_q, u"операции плагина"_q);
}

class RecoveryDuckIcon final : public Ui::RpWidget {
public:
	explicit RecoveryDuckIcon(QWidget *parent) : Ui::RpWidget(parent) {
		setMinimumSize(kRecoveryDuckSize, kRecoveryDuckSize);
		setMaximumSize(kRecoveryDuckSize, kRecoveryDuckSize);
		paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(this);
			p.setRenderHint(QPainter::Antialiasing);
			const auto rect = QRect(0, 0, width(), height());
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 214, 10));
			p.drawEllipse(rect.adjusted(4, 4, -4, -4));
			auto font = this->font();
			font.setPointSize(font.pointSize() + 18);
			p.setFont(font);
			p.setPen(Qt::black);
			p.drawText(
				rect,
				Qt::AlignCenter,
				QString::fromUtf8("\xF0\x9F\xA6\x86"));
		}, lifetime());
	}
};

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

QString DescribeBinaryInfoMismatchField(const Plugins::BinaryInfo &info) {
	const auto pluginCompiler = QString::fromLatin1(
		info.compiler ? info.compiler : "unknown");
	const auto hostCompiler = QString::fromLatin1(Plugins::kCompilerId);
	const auto pluginPlatform = QString::fromLatin1(
		info.platform ? info.platform : "unknown");
	const auto hostPlatform = QString::fromLatin1(Plugins::kPlatformId);

	if (info.structVersion != Plugins::kBinaryInfoVersion) {
		return u"structVersion"_q;
	}
	if (info.apiVersion != Plugins::kApiVersion) {
		return u"apiVersion"_q;
	}
	if (info.pointerSize != int(sizeof(void*))) {
		return u"pointerSize"_q;
	}
	if (pluginPlatform != hostPlatform) {
		return u"platform"_q;
	}
	if (pluginCompiler != hostCompiler) {
		return u"compiler"_q;
	}
	if (info.compilerVersion != Plugins::kCompilerVersion) {
		return u"compilerVersion"_q;
	}
	if (info.qtMajor != QT_VERSION_MAJOR) {
		return u"qtMajor"_q;
	}
	if (info.qtMinor != QT_VERSION_MINOR) {
		return u"qtMinor"_q;
	}
	return QString();
}

QString JsonValueToText(const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) {
		return u"null"_q;
	}
	if (value.isBool()) {
		return value.toBool() ? u"true"_q : u"false"_q;
	}
	if (value.isDouble()) {
		return QString::number(value.toDouble(), 'g', 16);
	}
	if (value.isString()) {
		auto text = value.toString();
		text.replace(u'\\', u"\\\\"_q);
		text.replace(u'\r', u"\\r"_q);
		text.replace(u'\n', u"\\n"_q);
		text.replace(u'\t', u"\\t"_q);
		text.replace(u'"', u"\\\""_q);
		return u"\""_q + text + u"\""_q;
	}
	return QString::fromUtf8(
		QJsonDocument(value.isObject()
			? QJsonDocument(value.toObject())
			: QJsonDocument(value.toArray()))
			.toJson(QJsonDocument::Compact));
}

void MergeJsonObject(QJsonObject &target, const QJsonObject &extra) {
	for (auto i = extra.begin(); i != extra.end(); ++i) {
		target.insert(i.key(), i.value());
	}
}

QJsonArray JsonArrayFromStrings(const QStringList &list) {
	auto result = QJsonArray();
	for (const auto &value : list) {
		result.push_back(value);
	}
	return result;
}

QStringList SortedJsonKeys(const QJsonObject &object) {
	auto keys = object.keys();
	keys.sort(Qt::CaseInsensitive);
	return keys;
}

QString CommandResultActionName(const Plugins::CommandResult::Action action) {
	using Action = Plugins::CommandResult::Action;
	switch (action) {
	case Action::Continue:
		return u"continue"_q;
	case Action::Cancel:
		return u"cancel"_q;
	case Action::Handled:
		return u"handled"_q;
	case Action::ReplaceText:
		return u"replace_text"_q;
	}
	return u"unknown"_q;
}

QString MessageEventName(const Plugins::MessageEvent event) {
	switch (event) {
	case Plugins::MessageEvent::New:
		return u"new"_q;
	case Plugins::MessageEvent::Edited:
		return u"edited"_q;
	case Plugins::MessageEvent::Deleted:
		return u"deleted"_q;
	}
	return u"unknown"_q;
}

QString DateTimeToIsoUtc(const QDateTime &value) {
	return value.isValid()
		? value.toUTC().toString(Qt::ISODateWithMs)
		: QString();
}

QJsonObject RecoveryOperationToJson(
		const RecoveryOperationState &state) {
	auto object = QJsonObject{
		{ u"kind"_q, state.kind },
		{ u"details"_q, state.details },
		{ u"startedAt"_q, state.startedAt },
	};
	auto pluginIds = QJsonArray();
	for (const auto &id : state.pluginIds) {
		pluginIds.push_back(id);
	}
	object.insert(u"pluginIds"_q, pluginIds);
	return object;
}

RecoveryOperationState RecoveryOperationFromJson(
		const QJsonObject &object) {
	auto state = RecoveryOperationState();
	state.kind = object.value(u"kind"_q).toString();
	state.details = object.value(u"details"_q).toString();
	state.startedAt = object.value(u"startedAt"_q).toString();
	const auto pluginIds = object.value(u"pluginIds"_q).toArray();
	for (const auto &value : pluginIds) {
		if (value.isString()) {
			state.pluginIds.push_back(value.toString());
		}
	}
	state.active = !state.kind.isEmpty();
	return state;
}

} // namespace

Manager::Manager(QObject *parent) : QObject(parent) {
}

Manager::~Manager() {
	stopRuntimeApiServer();
	unloadAll();
}

void Manager::loadRecoveryState() {
	_disabledByRecovery.clear();
	_recoveryPending = RecoveryOperationState();
	_recoveryNotice = RecoveryOperationState();
	_recoveryNoticeShown = false;

	QFile file(_recoveryPath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return;
	}
	const auto object = document.object();
	const auto disabledArray = object.value(u"disabledByRecovery"_q).toArray();
	for (const auto &value : disabledArray) {
		if (value.isString()) {
			_disabledByRecovery.insert(value.toString());
		}
	}
	if (const auto pending = object.value(u"pending"_q); pending.isObject()) {
		_recoveryPending = RecoveryOperationFromJson(pending.toObject());
	}
	if (const auto notice = object.value(u"notice"_q); notice.isObject()) {
		_recoveryNotice = RecoveryOperationFromJson(notice.toObject());
	}
	logEvent(
		u"recovery"_q,
		u"state-loaded"_q,
		QJsonObject{
			{ u"path"_q, _recoveryPath },
			{ u"disabledByRecovery"_q, JsonArrayFromStrings(_disabledByRecovery.values()) },
			{ u"pendingActive"_q, _recoveryPending.active },
			{ u"pendingKind"_q, _recoveryPending.kind },
			{ u"noticeActive"_q, _recoveryNotice.active },
			{ u"noticeKind"_q, _recoveryNotice.kind },
		});
}

void Manager::saveRecoveryState() const {
	if (_recoveryPath.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(_recoveryPath).absolutePath());

	auto disabledByRecovery = _disabledByRecovery.values();
	disabledByRecovery.sort(Qt::CaseInsensitive);

	auto object = QJsonObject();
	auto disabledArray = QJsonArray();
	for (const auto &id : disabledByRecovery) {
		disabledArray.push_back(id);
	}
	object.insert(u"disabledByRecovery"_q, disabledArray);
	if (_recoveryPending.active) {
		object.insert(u"pending"_q, RecoveryOperationToJson(_recoveryPending));
	}
	if (_recoveryNotice.active) {
		object.insert(u"notice"_q, RecoveryOperationToJson(_recoveryNotice));
	}

	QFile file(_recoveryPath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
	logEvent(
		u"recovery"_q,
		u"state-saved"_q,
		QJsonObject{
			{ u"path"_q, _recoveryPath },
			{ u"disabledByRecovery"_q, JsonArrayFromStrings(disabledByRecovery) },
			{ u"pendingActive"_q, _recoveryPending.active },
			{ u"noticeActive"_q, _recoveryNotice.active },
		});
}

void Manager::recoverFromPendingState() {
	if (!_recoveryPending.active) {
		return;
	}
	const auto pluginIds = _recoveryPending.pluginIds;
	const auto details = _recoveryPending.details;
	const auto kind = _recoveryPending.kind;

	for (const auto &pluginId : pluginIds) {
		if (pluginId.isEmpty()) {
			continue;
		}
		_disabled.insert(pluginId);
		_disabledByRecovery.insert(pluginId);
	}
	saveConfig();
	QDir().mkpath(QFileInfo(_safeModePath).absolutePath());
	auto file = QFile(_safeModePath);
	if (!file.exists() && file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.close();
	}
	_recoveryPending = RecoveryOperationState();
	queueRecoveryNotice(kind, pluginIds, details);
	logEvent(
		u"recovery"_q,
		u"safe-mode-enabled-after-crash"_q,
		QJsonObject{
			{ u"kind"_q, kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(pluginIds) },
			{ u"details"_q, details },
			{ u"safeModePath"_q, _safeModePath },
		});
	saveRecoveryState();
}

void Manager::startRecoveryOperation(
		QString kind,
		QStringList pluginIds,
		QString details) {
	_recoveryPending.active = true;
	_recoveryPending.kind = std::move(kind);
	_recoveryPending.pluginIds = std::move(pluginIds);
	_recoveryPending.details = std::move(details);
	_recoveryPending.startedAt = QDateTime::currentDateTimeUtc().toString(
		Qt::ISODateWithMs);
	logOperationStart(
		_recoveryPending.kind,
		_recoveryPending.pluginIds,
		_recoveryPending.details);
	saveRecoveryState();
}

void Manager::finishRecoveryOperation() {
	if (!_recoveryPending.active) {
		return;
	}
	_recoveryPending = RecoveryOperationState();
	saveRecoveryState();
	logOperationFinish();
}

void Manager::syncRecoveryFlags(PluginState &state) const {
	state.disabledByRecovery = _disabledByRecovery.contains(state.info.id);
	state.recoverySuspected = state.disabledByRecovery;
	state.recoveryReason = state.disabledByRecovery
		? PluginUiText(
			u"Disabled automatically after a suspected plugin crash."_q,
			u"Автоматически выключен после подозрения на крэш плагина."_q)
		: QString();
}

void Manager::clearRecoveryDisabled(const QString &pluginId) {
	if (pluginId.isEmpty()) {
		return;
	}
	_disabledByRecovery.remove(pluginId);
	if (_recoveryNotice.active) {
		_recoveryNotice.pluginIds.removeAll(pluginId);
		if (_recoveryNotice.pluginIds.isEmpty()) {
			_recoveryNotice = RecoveryOperationState();
		}
	}
	saveRecoveryState();
}

void Manager::queueRecoveryNotice(
		QString kind,
		QStringList pluginIds,
		QString details) {
	_recoveryNotice.active = true;
	_recoveryNotice.kind = std::move(kind);
	_recoveryNotice.pluginIds = std::move(pluginIds);
	_recoveryNotice.details = std::move(details);
	_recoveryNotice.startedAt = QDateTime::currentDateTimeUtc().toString(
		Qt::ISODateWithMs);
	_recoveryNoticeShown = false;
	logEvent(
		u"recovery"_q,
		u"notice-queued"_q,
		QJsonObject{
			{ u"kind"_q, _recoveryNotice.kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(_recoveryNotice.pluginIds) },
			{ u"details"_q, _recoveryNotice.details },
			{ u"startedAt"_q, _recoveryNotice.startedAt },
		});
}

QStringList Manager::describeRecoveryPlugins(
		const QStringList &pluginIds) const {
	auto result = QStringList();
	for (const auto &pluginId : pluginIds) {
		if (pluginId.isEmpty()) {
			continue;
		}
		if (const auto record = findRecord(pluginId)) {
			const auto name = record->state.info.name.trimmed();
			result.push_back(name.isEmpty() ? pluginId : name);
		} else {
			result.push_back(pluginId);
		}
	}
	result.removeDuplicates();
	return result;
}

QString Manager::composeRecoveryClipboardText() const {
	auto lines = QStringList();
	lines.push_back(PluginUiText(
		u"Telegram Desktop plugin recovery log"_q,
		u"Лог восстановления плагинов Telegram Desktop"_q));
	if (_recoveryNotice.active) {
		const auto plugins = describeRecoveryPlugins(_recoveryNotice.pluginIds);
		lines.push_back(
			PluginUiText(u"Operation: "_q, u"Операция: "_q)
			+ RecoveryOperationText(_recoveryNotice.kind));
		if (!plugins.isEmpty()) {
			lines.push_back(
				PluginUiText(u"Plugins: "_q, u"Плагины: "_q)
				+ plugins.join(u", "_q));
		}
		if (!_recoveryNotice.details.trimmed().isEmpty()) {
			lines.push_back(
				PluginUiText(u"Details: "_q, u"Детали: "_q)
				+ _recoveryNotice.details.trimmed());
		}
	}
	if (!_logPath.isEmpty()) {
		auto file = QFile(_logPath);
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const auto content = QString::fromUtf8(file.readAll()).trimmed();
			if (!content.isEmpty()) {
				lines.push_back(QString());
				lines.push_back(u"plugins.log"_q);
				lines.push_back(content);
			}
		}
	}
	if (!_tracePath.isEmpty()) {
		auto file = QFile(_tracePath);
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const auto content = QString::fromUtf8(file.readAll()).trimmed();
			if (!content.isEmpty()) {
				lines.push_back(QString());
				lines.push_back(u"plugins.trace.jsonl"_q);
				lines.push_back(content);
			}
		}
	}
	return lines.join(u"\n"_q);
}

void Manager::showRecoveryNotice(Window::Controller *window) {
	if (_recoveryNoticeShown || !_recoveryNotice.active || !window) {
		return;
	}
	_recoveryNoticeShown = true;
	const auto clipboardText = composeRecoveryClipboardText();
	if (const auto clipboard = QGuiApplication::clipboard()) {
		clipboard->setText(clipboardText);
	}

	const auto plugins = describeRecoveryPlugins(_recoveryNotice.pluginIds);
	const auto pluginText = plugins.isEmpty()
		? PluginUiText(u"unknown plugin"_q, u"неизвестный плагин"_q)
		: plugins.join(u", "_q);
	const auto title = PluginUiText(u"Safe Mode"_q, u"Безопасный режим"_q);
	const auto body = plugins.size() == 1
		? PluginUiText(
			u"Telegram noticed a crash during "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nSuspected plugin: "_q
				+ pluginText
				+ u"\n\nSafe mode was enabled automatically, the plugin was turned off, and the recovery log was copied to the clipboard."_q,
			u"Telegram заметил крэш во время "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nПодозреваемый плагин: "_q
				+ pluginText
				+ u"\n\nБезопасный режим включён автоматически, плагин выключен, лог восстановления уже скопирован в буфер обмена."_q)
		: PluginUiText(
			u"Telegram noticed a crash during "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nSuspected plugins: "_q
				+ pluginText
				+ u"\n\nSafe mode was enabled automatically, the listed plugins were turned off, and the recovery log was copied to the clipboard."_q,
			u"Telegram заметил крэш во время "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nПодозреваемые плагины: "_q
				+ pluginText
				+ u"\n\nБезопасный режим включён автоматически, указанные плагины выключены, лог восстановления уже скопирован в буфер обмена."_q);

	window->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(title));
		box->addLeftButton(rpl::single(PluginUiText(u"Copy"_q, u"Копировать"_q)), [=] {
			if (const auto clipboard = QGuiApplication::clipboard()) {
				clipboard->setText(clipboardText);
			}
			window->showToast(PluginUiText(
				u"Recovery log copied."_q,
				u"Лог восстановления скопирован."_q));
		});
		box->addRow(
			object_ptr<RecoveryDuckIcon>(box),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				st::boxPadding.bottom() / 2),
			style::al_top);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(body),
			st::boxLabel),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				0),
			style::al_top);
		box->addButton(rpl::single(PluginUiText(u"Continue"_q, u"Продолжить"_q)), [=] {
			box->closeBox();
		});
	}));

	_recoveryNotice = RecoveryOperationState();
	saveRecoveryState();
}

void Manager::start() {
	_pluginsPath = cWorkingDir() + QString::fromLatin1(kPluginsFolder);
	_configPath = cWorkingDir() + QString::fromLatin1(kConfigFile);
	_logPath = cWorkingDir() + QString::fromLatin1(kLogFile);
	_tracePath = cWorkingDir() + QString::fromLatin1(kTraceFile);
	_safeModePath = cWorkingDir() + QString::fromLatin1(kSafeModeFile);
	_recoveryPath = cWorkingDir() + QString::fromLatin1(kRecoveryFile);
	logEvent(
		u"manager"_q,
		u"start"_q,
		QJsonObject{
			{ u"pluginsPath"_q, _pluginsPath },
			{ u"configPath"_q, _configPath },
			{ u"logPath"_q, _logPath },
			{ u"tracePath"_q, _tracePath },
			{ u"safeModePath"_q, _safeModePath },
			{ u"recoveryPath"_q, _recoveryPath },
			{ u"apiVersion"_q, kApiVersion },
		});
	loadConfig();
	if (_runtimeApiEnabled) {
		startRuntimeApiServer();
	}
	loadRecoveryState();
	recoverFromPendingState();
	if (safeModeEnabled()) {
		logEvent(u"safe-mode"_q, u"startup-scan-metadata-only"_q);
		scanPlugins(true);
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
	logEvent(
		u"manager"_q,
		u"reload-requested"_q,
		QJsonObject{
			{ u"safeMode"_q, safeModeEnabled() },
			{ u"knownPlugins"_q, int(_plugins.size()) },
		});
	unloadAll();
	loadConfig();
	loadRecoveryState();
	if (safeModeEnabled()) {
		logEvent(u"safe-mode"_q, u"reload-scan-metadata-only"_q);
		scanPlugins(true);
		return;
	}
	const auto ownsRecovery = !_recoveryPending.active;
	if (ownsRecovery) {
		startRecoveryOperation(u"reload"_q);
	}
	scanPlugins();
	if (ownsRecovery) {
		finishRecoveryOperation();
	}
}

std::vector<PluginState> Manager::plugins() const {
	auto result = std::vector<PluginState>();
	result.reserve(_plugins.size());
	for (const auto &plugin : _plugins) {
		auto state = plugin.state;
		syncRecoveryFlags(state);
		result.push_back(std::move(state));
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
	logEvent(
		u"safe-mode"_q,
		u"set"_q,
		QJsonObject{
			{ u"enabled"_q, enabled },
			{ u"path"_q, _safeModePath },
		});
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
		logEvent(
			u"package"_q,
			u"inspect-failed"_q,
			QJsonObject{
				{ u"reason"_q, result.error },
				{ u"file"_q, fileInfoToJson(fileInfo) },
			});
		return result;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		result.error = u"Could not read the plugin package."_q;
		logEvent(
			u"package"_q,
			u"inspect-failed"_q,
			QJsonObject{
				{ u"reason"_q, result.error },
				{ u"file"_q, fileInfoToJson(fileInfo) },
			});
		return result;
	}
	if (file.size() <= 0) {
		result.error = u"Plugin package is empty."_q;
		logEvent(
			u"package"_q,
			u"inspect-failed"_q,
			QJsonObject{
				{ u"reason"_q, result.error },
				{ u"file"_q, fileInfoToJson(fileInfo) },
			});
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
	logEvent(
		u"package"_q,
		u"inspect"_q,
		QJsonObject{
			{ u"file"_q, fileInfoToJson(fileInfo) },
			{ u"sha256"_q, fileSha256(path) },
			{ u"previewAvailable"_q, result.previewAvailable },
			{ u"compatible"_q, result.compatible },
			{ u"installed"_q, result.installed },
			{ u"update"_q, result.update },
			{ u"installedVersion"_q, result.installedVersion },
			{ u"installedPath"_q, result.installedPath },
			{ u"icon"_q, result.icon },
			{ u"plugin"_q, pluginInfoToJson(result.info) },
			{ u"error"_q, result.error },
		});
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
		logEvent(
			u"package"_q,
			u"install-rejected"_q,
			QJsonObject{
				{ u"sourcePath"_q, sourcePath },
				{ u"reason"_q, error ? *error : preview.error },
				{ u"plugin"_q, pluginInfoToJson(preview.info) },
			});
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
			logEvent(u"safe-mode"_q, u"install-rescan-metadata-only"_q);
			scanPlugins(true);
		} else {
			scanPlugins();
		}
	};
	startRecoveryOperation(
			preview.update ? u"update"_q : u"install"_q,
			preview.info.id.isEmpty() ? QStringList() : QStringList{ preview.info.id },
			preview.info.name.isEmpty() ? targetPath : preview.info.name);
	logEvent(
		u"package"_q,
		u"install-start"_q,
		QJsonObject{
			{ u"sourcePath"_q, sourcePath },
			{ u"targetPath"_q, targetPath },
			{ u"tempPath"_q, tempPath },
			{ u"sourceSha256"_q, fileSha256(sourcePath) },
			{ u"source"_q, fileInfoToJson(sourceInfo) },
			{ u"plugin"_q, pluginInfoToJson(preview.info) },
			{ u"update"_q, preview.update },
			{ u"installedPath"_q, preview.installedPath },
		});

	unloadAll();
	loadConfig();
	loadRecoveryState();

	QFile::remove(tempPath);

	if (sourceInfo.absoluteFilePath() != targetPath) {
		if (!QFile::copy(sourcePath, tempPath)) {
			logEvent(
				u"package"_q,
				u"copy-failed"_q,
				QJsonObject{
					{ u"sourcePath"_q, sourcePath },
					{ u"tempPath"_q, tempPath },
				});
			rescanNow();
			if (error) {
				*error = u"Could not copy the plugin package into tdata/plugins."_q;
			}
			finishRecoveryOperation();
			return false;
		}
		if (!preview.installedPath.isEmpty()
			&& preview.installedPath != targetPath
			&& QFileInfo::exists(preview.installedPath)
			&& !QFile::remove(preview.installedPath)) {
			logEvent(
				u"package"_q,
				u"remove-previous-failed"_q,
				QJsonObject{
					{ u"installedPath"_q, preview.installedPath },
					{ u"targetPath"_q, targetPath },
				});
			QFile::remove(tempPath);
			rescanNow();
			if (error) {
				*error = u"Could not replace the previous plugin file."_q;
			}
			finishRecoveryOperation();
			return false;
		}
		if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath)) {
			logEvent(
				u"package"_q,
				u"overwrite-remove-failed"_q,
				QJsonObject{
					{ u"targetPath"_q, targetPath },
				});
			QFile::remove(tempPath);
			rescanNow();
			if (error) {
				*error = u"Could not overwrite the target plugin file."_q;
			}
			finishRecoveryOperation();
			return false;
		}
		if (!QFile::rename(tempPath, targetPath)) {
			logEvent(
				u"package"_q,
				u"rename-failed"_q,
				QJsonObject{
					{ u"tempPath"_q, tempPath },
					{ u"targetPath"_q, targetPath },
				});
			QFile::remove(tempPath);
			rescanNow();
			if (error) {
				*error = u"Could not finalize the installed plugin file."_q;
			}
			finishRecoveryOperation();
			return false;
		}
	}

	if (safeModeEnabled()) {
		logEvent(
			u"package"_q,
			u"installed-while-safe-mode"_q,
			QJsonObject{
				{ u"targetPath"_q, targetPath },
				{ u"plugin"_q, pluginInfoToJson(preview.info) },
			});
	} else {
		scanPlugins();
		if (!preview.info.id.isEmpty()) {
			if (const auto record = findRecord(preview.info.id);
				record && !record->state.error.trimmed().isEmpty()) {
				logEvent(
					u"package"_q,
					u"install-load-failed"_q,
					QJsonObject{
						{ u"targetPath"_q, targetPath },
						{ u"plugin"_q, pluginInfoToJson(preview.info) },
						{ u"loadError"_q, record->state.error.trimmed() },
					});
				if (error) {
					*error = record->state.error.trimmed();
				}
				finishRecoveryOperation();
				return false;
			}
		}
	}
	finishRecoveryOperation();
	if (error) {
		error->clear();
	}
	logEvent(
		u"package"_q,
		u"install-finished"_q,
		QJsonObject{
			{ u"targetPath"_q, targetPath },
			{ u"targetSha256"_q, fileSha256(targetPath) },
			{ u"plugin"_q, pluginInfoToJson(preview.info) },
			{ u"safeMode"_q, safeModeEnabled() },
		});
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
		logEvent(
			u"action"_q,
			u"missing"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
			});
		return false;
	}
	try {
		if (it->handlerWithContext) {
			startRecoveryOperation(u"action"_q, { it->pluginId }, it->title);
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
			logEvent(
				u"action"_q,
				u"invoke-context"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
					{ u"hasWindow"_q, context.window != nullptr },
					{ u"hasSession"_q, context.session != nullptr },
				});
			it->handlerWithContext(context);
			_registeringPluginId = previousPluginId;
			logEvent(
				u"action"_q,
				u"success"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			finishRecoveryOperation();
			return true;
		}
		if (it->handler) {
			startRecoveryOperation(u"action"_q, { it->pluginId }, it->title);
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = it->pluginId;
			logEvent(
				u"action"_q,
				u"invoke"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			it->handler();
			_registeringPluginId = previousPluginId;
			logEvent(
				u"action"_q,
				u"success"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			finishRecoveryOperation();
			return true;
		}
	} catch (...) {
		_registeringPluginId.clear();
		logEvent(
			u"action"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->title },
				{ u"reason"_q, CurrentExceptionText() },
			});
		disablePlugin(it->pluginId, u"Action failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin action failed and was disabled."_q,
			u"Действие плагина завершилось с ошибкой и было выключено."_q));
		finishRecoveryOperation();
	}
	return false;
}

bool Manager::openPanel(PanelId id) {
	const auto it = _panels.find(id);
	if (it == _panels.end()) {
		logEvent(
			u"panel"_q,
			u"missing"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
			});
		return false;
	}
	const auto window = activeWindow()
		? activeWindow()
		: Core::App().activePrimaryWindow();
	if (!window) {
		logEvent(
			u"panel"_q,
			u"no-active-window"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->descriptor.title },
			});
		showToast(u"No active window to show panel."_q);
		return false;
	}
	if (!it->handler) {
		logEvent(
			u"panel"_q,
			u"missing-handler"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
			});
		return false;
	}
	try {
		startRecoveryOperation(u"panel"_q, { it->pluginId }, it->descriptor.title);
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = it->pluginId;
		logEvent(
			u"panel"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"descriptor"_q, panelDescriptorToJson(it->descriptor) },
			});
		it->handler(window);
		_registeringPluginId = previousPluginId;
		logEvent(
			u"panel"_q,
			u"success"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->descriptor.title },
			});
		finishRecoveryOperation();
		return true;
	} catch (...) {
		_registeringPluginId.clear();
		logEvent(
			u"panel"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"title"_q, it->descriptor.title },
				{ u"reason"_q, CurrentExceptionText() },
			});
		disablePlugin(it->pluginId, u"Panel failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin panel failed and was disabled."_q,
			u"Панель плагина завершилась с ошибкой и была выключена."_q));
		finishRecoveryOperation();
		return false;
	}
}

bool Manager::setEnabled(const QString &pluginId, bool enabled) {
	if (!_pluginIndexById.contains(pluginId)) {
		logEvent(
			u"plugin"_q,
			u"toggle-missing"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"enabled"_q, enabled },
			});
		return false;
	}
	if (enabled) {
		_disabled.remove(pluginId);
		clearRecoveryDisabled(pluginId);
	} else {
		_disabled.insert(pluginId);
	}
	saveConfig();
	logEvent(
		u"plugin"_q,
		u"toggle"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"enabled"_q, enabled },
		});
	startRecoveryOperation(
		enabled ? u"enable"_q : u"disable"_q,
		{ pluginId });
	reload();
	finishRecoveryOperation();
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
				startRecoveryOperation(
					u"outgoing"_q,
					{ entry.pluginId });
				logEvent(
					u"outgoing"_q,
					u"invoke"_q,
					QJsonObject{
						{ u"id"_q, QString::number(entry.id) },
						{ u"pluginId"_q, entry.pluginId },
						{ u"priority"_q, entry.priority },
						{ u"text"_q, context.text },
						{ u"sendOptions"_q, sendOptionsToJson(context.options) },
					});
				const auto previousPluginId = _registeringPluginId;
				_registeringPluginId = entry.pluginId;
				const auto handled = entry.handler(context);
				_registeringPluginId = previousPluginId;
				logEvent(
					u"outgoing"_q,
					u"result"_q,
					QJsonObject{
						{ u"id"_q, QString::number(entry.id) },
						{ u"pluginId"_q, entry.pluginId },
						{ u"result"_q, commandResultToJson(handled) },
					});
				finishRecoveryOperation();
				if (handled.action != CommandResult::Action::Continue) {
					return handled;
				}
			} catch (...) {
				_registeringPluginId.clear();
				logEvent(
					u"outgoing"_q,
					u"failed"_q,
					QJsonObject{
						{ u"id"_q, QString::number(entry.id) },
						{ u"pluginId"_q, entry.pluginId },
						{ u"reason"_q, CurrentExceptionText() },
						{ u"text"_q, context.text },
					});
				disablePlugin(
					entry.pluginId,
					u"Outgoing interceptor failed: "_q + CurrentExceptionText());
				showToast(PluginUiText(
					u"Plugin interceptor failed and was disabled."_q,
					u"Перехватчик плагина завершился с ошибкой и был выключен."_q));
				finishRecoveryOperation();
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
			startRecoveryOperation(
				u"command"_q,
				{ entryIt->pluginId },
				entryIt->descriptor.command);
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entryIt->pluginId;
			logEvent(
				u"command"_q,
				u"invoke"_q,
				QJsonObject{
					{ u"id"_q, QString::number(entryIt->id) },
					{ u"pluginId"_q, entryIt->pluginId },
					{ u"descriptor"_q, commandDescriptorToJson(entryIt->descriptor) },
					{ u"text"_q, context.text },
					{ u"commandText"_q, context.command },
					{ u"args"_q, context.args },
					{ u"sendOptions"_q, sendOptionsToJson(context.options) },
				});
			const auto handled = entryIt->handler(context);
			_registeringPluginId = previousPluginId;
			logEvent(
				u"command"_q,
				u"result"_q,
				QJsonObject{
					{ u"id"_q, QString::number(entryIt->id) },
					{ u"pluginId"_q, entryIt->pluginId },
					{ u"result"_q, commandResultToJson(handled) },
				});
			finishRecoveryOperation();
			return handled;
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"command"_q,
				u"failed"_q,
				QJsonObject{
					{ u"id"_q, QString::number(entryIt->id) },
					{ u"pluginId"_q, entryIt->pluginId },
					{ u"reason"_q, CurrentExceptionText() },
					{ u"text"_q, context.text },
					{ u"commandText"_q, context.command },
					{ u"args"_q, context.args },
				});
			disablePlugin(
				entryIt->pluginId,
				u"Command handler failed: "_q + CurrentExceptionText());
			showToast(PluginUiText(
				u"Plugin command failed and was disabled."_q,
				u"Команда плагина завершилась с ошибкой и была выключена."_q));
			finishRecoveryOperation();
			return {
				.action = CommandResult::Action::Cancel,
			};
		}
	}
	return result;
}

void Manager::notifyWindowCreated(Window::Controller *window) {
	logEvent(
		u"window"_q,
		u"created"_q,
		QJsonObject{
			{ u"hasWindow"_q, window != nullptr },
		});
	if (_recoveryNotice.active && !_recoveryNoticeShown) {
		QTimer::singleShot(0, this, [=] {
			showRecoveryNotice(window);
		});
	}
	const auto handlers = _windowHandlers;
	for (const auto &entry : handlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			startRecoveryOperation(u"window"_q, { entry.pluginId });
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			logEvent(
				u"window"_q,
				u"invoke-callback"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"hasWindow"_q, window != nullptr },
				});
			entry.handler(window);
			_registeringPluginId = previousPluginId;
			logEvent(
				u"window"_q,
				u"callback-success"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
				});
			finishRecoveryOperation();
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"window"_q,
				u"callback-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"reason"_q, CurrentExceptionText() },
				});
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Window callback failed: "_q + CurrentExceptionText());
			}
			showToast(PluginUiText(
				u"Plugin window callback failed."_q,
				u"Оконный callback плагина завершился с ошибкой."_q));
			finishRecoveryOperation();
		}
	}
}

int Manager::apiVersion() const {
	return kApiVersion;
}

QString Manager::pluginsPath() const {
	return _pluginsPath;
}

HostInfo Manager::hostInfo() const {
	auto info = HostInfo();
	info.appVersion = QString::fromLatin1(AppVersionStr);
	info.compiler = QString::fromLatin1(kCompilerId);
	info.platform = QString::fromLatin1(kPlatformId);
	info.workingPath = cWorkingDir();
	info.pluginsPath = _pluginsPath;
	info.safeModeEnabled = safeModeEnabled();
	info.runtimeApiEnabled = _runtimeApiEnabled && (_runtimeApiServer != nullptr);
	info.runtimeApiPort = int(_runtimeApiPort);
	info.runtimeApiBaseUrl = runtimeApiBaseUrl();
	return info;
}

SystemInfo Manager::systemInfo() const {
	auto info = SystemInfo();
	const auto locale = QLocale::system();
	const auto [totalMemoryBytes, availableMemoryBytes] = SystemMemoryInfo();
	info.processId = quint64(QCoreApplication::applicationPid());
	info.totalMemoryBytes = totalMemoryBytes;
	info.availableMemoryBytes = availableMemoryBytes;
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

bool Manager::runtimeApiEnabled() const {
	return _runtimeApiEnabled;
}

QString Manager::runtimeApiBaseUrl() const {
	return u"http://%1:%2"_q.arg(QString::fromLatin1(kRuntimeApiHost)).arg(
		int(_runtimeApiPort));
}

QString Manager::runtimeApiToken() const {
	return _runtimeApiToken;
}

bool Manager::setRuntimeApiEnabled(bool enabled) {
	if (_runtimeApiEnabled == enabled) {
		return true;
	}
	if (enabled) {
		ensureRuntimeApiToken();
		const auto previous = _runtimeApiEnabled;
		_runtimeApiEnabled = true;
		if (!startRuntimeApiServer()) {
			_runtimeApiEnabled = previous;
			return false;
		}
	} else {
		stopRuntimeApiServer();
		_runtimeApiEnabled = false;
	}
	saveConfig();
	logEvent(
		u"runtime-api"_q,
		u"set-enabled"_q,
		QJsonObject{
			{ u"enabled"_q, _runtimeApiEnabled },
			{ u"baseUrl"_q, runtimeApiBaseUrl() },
			{ u"hasToken"_q, !_runtimeApiToken.isEmpty() },
		});
	return true;
}

QString Manager::rotateRuntimeApiToken() {
	ensureRuntimeApiToken();
	_runtimeApiToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
	saveConfig();
	logEvent(
		u"runtime-api"_q,
		u"rotate-token"_q,
		QJsonObject{
			{ u"baseUrl"_q, runtimeApiBaseUrl() },
		});
	return _runtimeApiToken;
}

CommandId Manager::registerCommand(
		const QString &pluginId,
	CommandDescriptor descriptor,
	CommandHandler handler) {
	if (!hasPlugin(pluginId) || !handler) {
		logEvent(
			u"registry"_q,
			u"register-command-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	const auto key = commandKey(descriptor.command);
	if (!IsValidCommandKey(key) || _commandIdByName.contains(key)) {
		logEvent(
			u"registry"_q,
			u"register-command-invalid"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"descriptor"_q, commandDescriptorToJson(descriptor) },
				{ u"normalizedKey"_q, key },
				{ u"duplicate"_q, _commandIdByName.contains(key) },
			});
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
	logEvent(
		u"registry"_q,
		u"register-command"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"descriptor"_q, commandDescriptorToJson(_commands.value(id).descriptor) },
		});
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
	logEvent(
		u"registry"_q,
		u"unregister-command"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

ActionId Manager::registerAction(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionHandler handler) {
	if (!hasPlugin(pluginId) || !handler || title.trimmed().isEmpty()) {
		logEvent(
			u"registry"_q,
			u"register-action-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"title"_q, title },
				{ u"hasHandler"_q, handler != nullptr },
			});
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
	logEvent(
		u"registry"_q,
		u"register-action"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"title"_q, title },
			{ u"description"_q, description },
			{ u"contextAware"_q, false },
		});
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
	logEvent(
		u"registry"_q,
		u"unregister-action"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

ActionId Manager::registerActionWithContext(
		const QString &pluginId,
		const QString &title,
		const QString &description,
		ActionWithContextHandler handler) {
	if (!hasPlugin(pluginId) || !handler || title.trimmed().isEmpty()) {
		logEvent(
			u"registry"_q,
			u"register-action-context-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"title"_q, title },
				{ u"hasHandler"_q, handler != nullptr },
			});
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
	logEvent(
		u"registry"_q,
		u"register-action"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"title"_q, title },
			{ u"description"_q, description },
			{ u"contextAware"_q, true },
		});
	return id;
}

OutgoingInterceptorId Manager::registerOutgoingTextInterceptor(
		const QString &pluginId,
		OutgoingTextHandler handler,
		int priority) {
	if (!hasPlugin(pluginId) || !handler) {
		logEvent(
			u"registry"_q,
			u"register-outgoing-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"priority"_q, priority },
				{ u"hasHandler"_q, handler != nullptr },
			});
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
	logEvent(
		u"registry"_q,
		u"register-outgoing"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"priority"_q, priority },
		});
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
	logEvent(
		u"registry"_q,
		u"unregister-outgoing"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

MessageObserverId Manager::registerMessageObserver(
		const QString &pluginId,
		MessageObserverOptions options,
		MessageEventHandler handler) {
	if (!hasPlugin(pluginId) || !handler) {
		logEvent(
			u"registry"_q,
			u"register-observer-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"hasHandler"_q, handler != nullptr },
			});
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
	logEvent(
		u"registry"_q,
		u"register-observer"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"newMessages"_q, options.newMessages },
			{ u"editedMessages"_q, options.editedMessages },
			{ u"deletedMessages"_q, options.deletedMessages },
			{ u"incoming"_q, options.incoming },
			{ u"outgoing"_q, options.outgoing },
		});
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
	logEvent(
		u"registry"_q,
		u"unregister-observer"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
	updateMessageObserverSubscriptions();
}

PanelId Manager::registerPanel(
		const QString &pluginId,
		PanelDescriptor descriptor,
		PanelHandler handler) {
	if (!hasPlugin(pluginId) || !handler || descriptor.title.trimmed().isEmpty()) {
		logEvent(
			u"registry"_q,
			u"register-panel-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"descriptor"_q, panelDescriptorToJson(descriptor) },
				{ u"hasHandler"_q, handler != nullptr },
			});
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
	logEvent(
		u"registry"_q,
		u"register-panel"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"descriptor"_q, panelDescriptorToJson(_panels.value(id).descriptor) },
		});
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
	logEvent(
		u"registry"_q,
		u"unregister-panel"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
		});
}

void Manager::showToast(const QString &text) {
	logEvent(
		u"ui"_q,
		u"toast"_q,
		QJsonObject{
			{ u"text"_q, text },
		});
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
		logEvent(
			u"registry"_q,
			u"register-window-handler"_q,
			QJsonObject{
				{ u"pluginId"_q, _registeringPluginId },
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
		logEvent(
			u"registry"_q,
			u"register-session-handler"_q,
			QJsonObject{
				{ u"pluginId"_q, _registeringPluginId },
			});
	}
}

void Manager::loadConfig() {
	_disabled.clear();
	QFile file(_configPath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		logEvent(
			u"config"_q,
			u"load-empty"_q,
			QJsonObject{
				{ u"path"_q, _configPath },
			});
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		logEvent(
			u"config"_q,
			u"load-invalid-json"_q,
			QJsonObject{
				{ u"path"_q, _configPath },
			});
		return;
	}
	const auto array = document.object().value(u"disabled"_q).toArray();
	for (const auto &value : array) {
		if (value.isString()) {
			_disabled.insert(value.toString());
		}
	}
	const auto object = document.object();
	_runtimeApiEnabled = object.value(u"runtimeApiEnabled"_q).toBool(false);
	const auto configuredPort = object.value(u"runtimeApiPort"_q).toInt(
		int(_runtimeApiPort));
	if (configuredPort > 0 && configuredPort <= 65535) {
		_runtimeApiPort = quint16(configuredPort);
	}
	_runtimeApiToken = object.value(u"runtimeApiToken"_q).toString().trimmed();
	logEvent(
		u"config"_q,
		u"loaded"_q,
		QJsonObject{
			{ u"path"_q, _configPath },
			{ u"disabled"_q, JsonArrayFromStrings(_disabled.values()) },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, int(_runtimeApiPort) },
			{ u"hasRuntimeApiToken"_q, !_runtimeApiToken.isEmpty() },
		});
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
		{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
		{ u"runtimeApiPort"_q, int(_runtimeApiPort) },
		{ u"runtimeApiToken"_q, _runtimeApiToken },
	};
	const auto document = QJsonDocument(object);
	QFile file(_configPath);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(document.toJson(QJsonDocument::Indented));
	}
	logEvent(
		u"config"_q,
		u"saved"_q,
		QJsonObject{
			{ u"path"_q, _configPath },
			{ u"disabled"_q, JsonArrayFromStrings(list) },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, int(_runtimeApiPort) },
			{ u"hasRuntimeApiToken"_q, !_runtimeApiToken.isEmpty() },
		});
}

void Manager::ensureRuntimeApiToken() {
	if (_runtimeApiToken.trimmed().isEmpty()) {
		_runtimeApiToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
}

bool Manager::startRuntimeApiServer() {
	if (!_runtimeApiEnabled) {
		return true;
	}
	if (_runtimeApiServer) {
		return _runtimeApiServer->isListening();
	}
	ensureRuntimeApiToken();
	auto *server = new QTcpServer(this);
	if (!server->listen(QHostAddress(QString::fromLatin1(kRuntimeApiHost)), _runtimeApiPort)) {
		logEvent(
			u"runtime-api"_q,
			u"listen-failed"_q,
			QJsonObject{
				{ u"baseUrl"_q, runtimeApiBaseUrl() },
				{ u"reason"_q, server->errorString() },
			});
		server->deleteLater();
		return false;
	}
	connect(server, &QTcpServer::newConnection, this, [=] {
		handleRuntimeApiNewConnection();
	});
	_runtimeApiServer = server;
	logEvent(
		u"runtime-api"_q,
		u"started"_q,
		QJsonObject{
			{ u"baseUrl"_q, runtimeApiBaseUrl() },
			{ u"hasToken"_q, !_runtimeApiToken.isEmpty() },
		});
	return true;
}

void Manager::stopRuntimeApiServer() {
	if (!_runtimeApiServer) {
		return;
	}
	logEvent(
		u"runtime-api"_q,
		u"stopped"_q,
		QJsonObject{
			{ u"baseUrl"_q, runtimeApiBaseUrl() },
		});
	const auto sockets = _runtimeApiBuffers.keys();
	for (auto *socket : sockets) {
		closeRuntimeApiSocket(socket);
	}
	_runtimeApiServer->close();
	_runtimeApiServer->deleteLater();
	_runtimeApiServer = nullptr;
}

void Manager::handleRuntimeApiNewConnection() {
	if (!_runtimeApiServer) {
		return;
	}
	while (_runtimeApiServer->hasPendingConnections()) {
		auto *socket = _runtimeApiServer->nextPendingConnection();
		if (!socket) {
			continue;
		}
		_runtimeApiBuffers.insert(socket, QByteArray());
		connect(socket, &QTcpSocket::readyRead, this, [=] {
			handleRuntimeApiReadyRead(socket);
		});
		connect(socket, &QTcpSocket::disconnected, this, [=] {
			closeRuntimeApiSocket(socket);
		});
	}
}

void Manager::handleRuntimeApiReadyRead(QTcpSocket *socket) {
	if (!socket) {
		return;
	}
	auto it = _runtimeApiBuffers.find(socket);
	if (it == _runtimeApiBuffers.end()) {
		_runtimeApiBuffers.insert(socket, QByteArray());
		it = _runtimeApiBuffers.find(socket);
	}
	it.value().append(socket->readAll());
	const auto headerEnd = it.value().indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		return;
	}
	const auto request = it.value();
	auto headerBlock = request.left(headerEnd);
	auto lines = headerBlock.split('\n');
	if (lines.isEmpty()) {
		socket->write(makeRuntimeApiResponse(400, runtimeApiEnvelope(
			false,
			QJsonValue(),
			RuntimeApiText(
				u"Malformed HTTP request."_q,
				u"Некорректный HTTP-запрос."_q))));
		socket->disconnectFromHost();
		return;
	}
	const auto requestLine = QString::fromUtf8(lines.takeFirst()).trimmed();
	const auto requestParts = requestLine.split(u' ', Qt::SkipEmptyParts);
	if (requestParts.size() < 2) {
		socket->write(makeRuntimeApiResponse(400, runtimeApiEnvelope(
			false,
			QJsonValue(),
			RuntimeApiText(
				u"Malformed HTTP request line."_q,
				u"Некорректная строка HTTP-запроса."_q))));
		socket->disconnectFromHost();
		return;
	}
	const auto method = requestParts[0];
	const auto target = requestParts[1];
	auto headers = QHash<QByteArray, QByteArray>();
	for (const auto &line : lines) {
		const auto clean = line.trimmed();
		if (clean.isEmpty()) {
			continue;
		}
		const auto separator = clean.indexOf(':');
		if (separator <= 0) {
			continue;
		}
		headers.insert(
			clean.left(separator).trimmed().toLower(),
			clean.mid(separator + 1).trimmed());
	}
	if (method != u"GET"_q) {
		socket->write(makeRuntimeApiResponse(405, runtimeApiEnvelope(
			false,
			QJsonValue(),
			RuntimeApiText(
				u"Only GET is supported."_q,
				u"Поддерживается только GET."_q))));
		socket->disconnectFromHost();
		return;
	}

	const auto url = QUrl::fromEncoded(target.toUtf8());
	const auto path = url.path().trimmed();
	const auto query = QUrlQuery(url);
	logEvent(
		u"runtime-api"_q,
		u"request"_q,
		QJsonObject{
			{ u"method"_q, method },
			{ u"path"_q, path },
			{ u"peer"_q, socket->peerAddress().toString() },
		});

	if (path == u"/api/ping"_q) {
		socket->write(makeRuntimeApiResponse(
			200,
			runtimeApiEnvelope(true, QJsonObject{
				{ u"status"_q, u"ok"_q },
				{ u"apiVersion"_q, kApiVersion },
			})));
		socket->disconnectFromHost();
		return;
	}

	if (!runtimeApiAuthorized(headers, query)) {
		socket->write(makeRuntimeApiResponse(401, runtimeApiEnvelope(
			false,
			QJsonValue(),
			RuntimeApiText(
				u"Runtime API token is missing or invalid."_q,
				u"Токен runtime API отсутствует или неверен."_q))));
		socket->disconnectFromHost();
		return;
	}

	auto statusCode = 200;
	auto payload = runtimeApiEnvelope(true, QJsonObject());

	if (path == u"/api/runtime/state"_q) {
		payload = runtimeApiEnvelope(true, runtimeStateToJson());
	} else if (path == u"/api/runtime/host"_q) {
		payload = runtimeApiEnvelope(true, hostInfoToJson(hostInfo()));
	} else if (path == u"/api/runtime/system"_q) {
		payload = runtimeApiEnvelope(true, systemInfoToJson(systemInfo()));
	} else if (path == u"/api/runtime/plugins"_q) {
		payload = runtimeApiEnvelope(true, pluginStatesToJson());
	} else if (path == u"/api/runtime/sessions"_q) {
		payload = runtimeApiEnvelope(true, sessionStatesToJson());
	} else if (path == u"/api/runtime/windows"_q) {
		payload = runtimeApiEnvelope(true, windowStatesToJson());
	} else if (path == u"/api/runtime/reload"_q) {
		reload();
		payload = runtimeApiEnvelope(true, runtimeStateToJson());
	} else if (path == u"/api/runtime/plugin/enable"_q) {
		const auto pluginId = query.queryItemValue(u"id"_q).trimmed();
		if (pluginId.isEmpty()) {
			statusCode = 400;
			payload = runtimeApiEnvelope(false, QJsonValue(), RuntimeApiText(
				u"Missing plugin id."_q,
				u"Не указан id плагина."_q));
		} else if (!setEnabled(pluginId, true)) {
			statusCode = 404;
			payload = runtimeApiEnvelope(false, QJsonValue(), RuntimeApiText(
				u"Plugin could not be enabled."_q,
				u"Не удалось включить плагин."_q));
		} else if (const auto record = findRecord(pluginId)) {
			payload = runtimeApiEnvelope(true, pluginStateToJson(record->state));
		} else {
			payload = runtimeApiEnvelope(true, runtimeStateToJson());
		}
	} else if (path == u"/api/runtime/plugin/disable"_q) {
		const auto pluginId = query.queryItemValue(u"id"_q).trimmed();
		if (pluginId.isEmpty()) {
			statusCode = 400;
			payload = runtimeApiEnvelope(false, QJsonValue(), RuntimeApiText(
				u"Missing plugin id."_q,
				u"Не указан id плагина."_q));
		} else if (!setEnabled(pluginId, false)) {
			statusCode = 404;
			payload = runtimeApiEnvelope(false, QJsonValue(), RuntimeApiText(
				u"Plugin could not be disabled."_q,
				u"Не удалось выключить плагин."_q));
		} else if (const auto record = findRecord(pluginId)) {
			payload = runtimeApiEnvelope(true, pluginStateToJson(record->state));
		} else {
			payload = runtimeApiEnvelope(true, runtimeStateToJson());
		}
	} else if (path == u"/api/runtime/plugin/install"_q) {
		const auto sourcePath = query.queryItemValue(u"path"_q).trimmed();
		auto error = QString();
		if (sourcePath.isEmpty()) {
			statusCode = 400;
			payload = runtimeApiEnvelope(false, QJsonValue(), RuntimeApiText(
				u"Missing plugin package path."_q,
				u"Не указан путь к пакету плагина."_q));
		} else if (!installPackage(sourcePath, &error)) {
			statusCode = 409;
			payload = runtimeApiEnvelope(
				false,
				QJsonValue(),
				error.isEmpty()
					? RuntimeApiText(
						u"Plugin install failed."_q,
						u"Установка плагина не удалась."_q)
					: error);
		} else {
			payload = runtimeApiEnvelope(true, runtimeStateToJson());
		}
	} else if (path == u"/api/runtime/logs/recent"_q) {
		const auto requestedBytes = query.queryItemValue(u"bytes"_q).toLongLong();
		const auto maxBytes = qsizetype(std::clamp<qint64>(
			(requestedBytes > 0) ? requestedBytes : 65536,
			1024,
			qint64(kMaxPluginLogBytes)));
		payload = runtimeApiEnvelope(true, QJsonObject{
			{ u"log"_q, QString::fromUtf8(readLogTail(_logPath, maxBytes)) },
			{ u"trace"_q, QString::fromUtf8(readLogTail(_tracePath, maxBytes)) },
		});
	} else {
		statusCode = 404;
		payload = runtimeApiEnvelope(false, QJsonValue(), RuntimeApiText(
			u"Unknown runtime API endpoint."_q,
			u"Неизвестный endpoint runtime API."_q));
	}

	socket->write(makeRuntimeApiResponse(statusCode, payload));
	socket->disconnectFromHost();
}

void Manager::closeRuntimeApiSocket(QTcpSocket *socket) {
	if (!socket) {
		return;
	}
	_runtimeApiBuffers.remove(socket);
	socket->close();
	socket->deleteLater();
}

bool Manager::runtimeApiAuthorized(
		const QHash<QByteArray, QByteArray> &headers,
		const QUrlQuery &query) const {
	if (!_runtimeApiEnabled || _runtimeApiToken.isEmpty()) {
		return false;
	}
	auto token = query.queryItemValue(u"token"_q).trimmed();
	if (token.isEmpty()) {
		token = QString::fromUtf8(headers.value(kRuntimeApiAuthHeader)).trimmed();
	}
	if (token.isEmpty()) {
		const auto authorization = QString::fromUtf8(
			headers.value("authorization")).trimmed();
		if (authorization.startsWith(u"Bearer "_q, Qt::CaseInsensitive)) {
			token = authorization.mid(7).trimmed();
		}
	}
	return token == _runtimeApiToken;
}

void Manager::appendTraceLine(const QByteArray &line) const {
	writeLogRecord(_tracePath, line, true);
}

void Manager::writeLogRecord(
		const QString &path,
		const QByteArray &record,
		bool jsonLog) const {
	if (path.isEmpty() || record.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	const auto rotated = rotateLogFileIfNeeded(path, record.size(), jsonLog);
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		return;
	}
	if (rotated) {
		const auto rotationEvent = makeLogEvent(
			u"log"_q,
			u"rotated"_q,
			QJsonObject{
				{ u"target"_q, QFileInfo(path).fileName() },
				{ u"json"_q, jsonLog },
				{ u"reason"_q, u"max-size"_q },
				{ u"maxBytes"_q, QString::number(kMaxPluginLogBytes) },
			});
		const auto rotationLine = jsonLog
			? (QJsonDocument(rotationEvent).toJson(QJsonDocument::Compact) + '\n')
			: ((rotationEvent.value(u"timestampUtc"_q).toString()
				+ u" "_q
				+ formatLogEventText(rotationEvent)
				+ u"\n"_q).toUtf8());
		file.write(rotationLine);
	}
	file.write(record);
}

bool Manager::rotateLogFileIfNeeded(
		const QString &path,
		qsizetype recordSize,
		bool jsonLog) const {
	Q_UNUSED(jsonLog);
	if (path.isEmpty()) {
		return false;
	}
	const auto info = QFileInfo(path);
	if (!info.exists()) {
		return false;
	}
	if ((info.size() + recordSize) <= kMaxPluginLogBytes) {
		return false;
	}
	const auto rotatedName = [&](int index) {
		return path + u'.' + QString::number(index);
	};
	for (auto index = kMaxPluginLogBackups; index >= 1; --index) {
		const auto source = (index == 1) ? path : rotatedName(index - 1);
		const auto target = rotatedName(index);
		if (!QFileInfo::exists(source)) {
			continue;
		}
		if (QFileInfo::exists(target)) {
			QFile::remove(target);
		}
		QFile::rename(source, target);
	}
	return true;
}

void Manager::appendLogLine(const QString &line) const {
	if (_logPath.isEmpty()) {
		return;
	}
	const auto stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
	writeLogRecord(
		_logPath,
		(stamp + u" "_q + line.trimmed() + u"\n"_q).toUtf8(),
		false);
}

QJsonObject Manager::makeLogEvent(
		QString phase,
		QString event,
		QJsonObject details) const {
	auto result = QJsonObject{
		{ u"timestampUtc"_q, DateTimeToIsoUtc(QDateTime::currentDateTimeUtc()) },
		{ u"pid"_q, int(QCoreApplication::applicationPid()) },
		{ u"phase"_q, std::move(phase) },
		{ u"event"_q, std::move(event) },
		{ u"language"_q, UseRussianPluginUi() ? u"ru"_q : u"en"_q },
	};
	if (_activeSession) {
		result.insert(
			u"sessionUniqueId"_q,
			QString::number(_activeSession->uniqueId()));
	}
	if (!_traceOperations.isEmpty()) {
		const auto &operation = _traceOperations.back();
		result.insert(u"operationId"_q, QString::number(operation.id));
		result.insert(u"operationKind"_q, operation.kind);
		result.insert(u"operationDepth"_q, _traceOperations.size());
		if (!operation.pluginIds.isEmpty()) {
			result.insert(
				u"operationPluginIds"_q,
				JsonArrayFromStrings(operation.pluginIds));
		}
		if (!operation.details.isEmpty()) {
			result.insert(u"operationDetails"_q, operation.details);
		}
	}
	MergeJsonObject(result, details);
	return result;
}

QString Manager::formatLogEventText(const QJsonObject &event) const {
	auto parts = QStringList();
	if (const auto phase = event.value(u"phase"_q).toString(); !phase.isEmpty()) {
		parts.push_back(u"phase="_q + JsonValueToText(phase));
	}
	if (const auto name = event.value(u"event"_q).toString(); !name.isEmpty()) {
		parts.push_back(u"event="_q + JsonValueToText(name));
	}
	for (const auto &key : SortedJsonKeys(event)) {
		if (key == u"timestampUtc"_q || key == u"phase"_q || key == u"event"_q) {
			continue;
		}
		parts.push_back(key + u"="_q + JsonValueToText(event.value(key)));
	}
	return parts.join(u" "_q);
}

void Manager::logEvent(
		QString phase,
		QString event,
		QJsonObject details) const {
	const auto payload = makeLogEvent(
		std::move(phase),
		std::move(event),
		std::move(details));
	appendLogLine(formatLogEventText(payload));
	appendTraceLine(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
}

void Manager::logOperationStart(
		const QString &kind,
		const QStringList &pluginIds,
		const QString &details) {
	auto operation = TraceOperationState();
	operation.id = _nextTraceOperationId++;
	operation.kind = kind;
	operation.pluginIds = pluginIds;
	operation.details = details;
	operation.startedAt = QDateTime::currentDateTimeUtc();
	_traceOperations.push_back(operation);
	logEvent(
		u"operation"_q,
		u"start"_q,
		QJsonObject{
			{ u"kind"_q, kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(pluginIds) },
			{ u"details"_q, details },
			{ u"startedAt"_q, DateTimeToIsoUtc(operation.startedAt) },
		});
}

void Manager::logOperationFinish(
		const QString &result,
		const QString &reason) {
	if (_traceOperations.isEmpty()) {
		return;
	}
	const auto operation = _traceOperations.back();
	_traceOperations.pop_back();
	const auto finishedAt = QDateTime::currentDateTimeUtc();
	logEvent(
		u"operation"_q,
		u"finish"_q,
		QJsonObject{
			{ u"operationId"_q, QString::number(operation.id) },
			{ u"kind"_q, operation.kind },
			{ u"pluginIds"_q, JsonArrayFromStrings(operation.pluginIds) },
			{ u"details"_q, operation.details },
			{ u"startedAt"_q, DateTimeToIsoUtc(operation.startedAt) },
			{ u"finishedAt"_q, DateTimeToIsoUtc(finishedAt) },
			{ u"durationMs"_q, QString::number(operation.startedAt.msecsTo(finishedAt)) },
			{ u"result"_q, result.isEmpty() ? u"ok"_q : result },
			{ u"reason"_q, reason },
		});
}

quint64 Manager::currentOperationId() const {
	return _traceOperations.isEmpty() ? 0 : _traceOperations.back().id;
}

QJsonObject Manager::pluginInfoToJson(const PluginInfo &info) const {
	return QJsonObject{
		{ u"id"_q, info.id },
		{ u"name"_q, info.name },
		{ u"version"_q, info.version },
		{ u"author"_q, info.author },
		{ u"description"_q, info.description },
		{ u"website"_q, info.website },
	};
}

QJsonObject Manager::pluginStateToJson(const PluginState &state) const {
	auto result = pluginInfoToJson(state.info);
	result.insert(u"path"_q, state.path);
	result.insert(u"enabled"_q, state.enabled);
	result.insert(u"loaded"_q, state.loaded);
	result.insert(u"error"_q, state.error);
	result.insert(u"disabledByRecovery"_q, state.disabledByRecovery);
	result.insert(u"recoverySuspected"_q, state.recoverySuspected);
	result.insert(u"recoveryReason"_q, state.recoveryReason);
	return result;
}

QJsonObject Manager::commandDescriptorToJson(
		const CommandDescriptor &descriptor) const {
	return QJsonObject{
		{ u"command"_q, descriptor.command },
		{ u"description"_q, descriptor.description },
		{ u"usage"_q, descriptor.usage },
	};
}

QJsonObject Manager::panelDescriptorToJson(
		const PanelDescriptor &descriptor) const {
	return QJsonObject{
		{ u"title"_q, descriptor.title },
		{ u"description"_q, descriptor.description },
	};
}

QJsonObject Manager::sendOptionsToJson(
		const Api::SendOptions *options) const {
	if (!options) {
		return {};
	}
	return QJsonObject{
		{ u"price"_q, QString::number(options->price) },
		{ u"scheduled"_q, QString::number(options->scheduled) },
		{ u"scheduleRepeatPeriod"_q, QString::number(options->scheduleRepeatPeriod) },
		{ u"shortcutId"_q, QString::number(options->shortcutId) },
		{ u"effectId"_q, QString::number(options->effectId) },
		{ u"stakeNanoTon"_q, QString::number(options->stakeNanoTon) },
		{ u"starsApproved"_q, options->starsApproved },
		{ u"silent"_q, options->silent },
		{ u"handleSupportSwitch"_q, options->handleSupportSwitch },
		{ u"invertCaption"_q, options->invertCaption },
		{ u"hideViaBot"_q, options->hideViaBot },
		{ u"mediaSpoiler"_q, options->mediaSpoiler },
		{ u"ttlSeconds"_q, QString::number(options->ttlSeconds) },
		{ u"hasSendAs"_q, options->sendAs != nullptr },
		{ u"stakeSeedHashHex"_q, QString::fromLatin1(options->stakeSeedHash.toHex()) },
	};
}

QJsonObject Manager::binaryInfoToJson(const BinaryInfo &info) const {
	return QJsonObject{
		{ u"structVersion"_q, info.structVersion },
		{ u"apiVersion"_q, info.apiVersion },
		{ u"pointerSize"_q, info.pointerSize },
		{ u"qtMajor"_q, info.qtMajor },
		{ u"qtMinor"_q, info.qtMinor },
		{ u"compiler"_q, QString::fromLatin1(info.compiler ? info.compiler : "unknown") },
		{ u"compilerVersion"_q, info.compilerVersion },
		{ u"platform"_q, QString::fromLatin1(info.platform ? info.platform : "unknown") },
	};
}

QJsonObject Manager::fileInfoToJson(const QFileInfo &info) const {
	return QJsonObject{
		{ u"fileName"_q, info.fileName() },
		{ u"path"_q, info.absoluteFilePath() },
		{ u"canonicalPath"_q, info.canonicalFilePath() },
		{ u"exists"_q, info.exists() },
		{ u"size"_q, QString::number(info.size()) },
		{ u"lastModifiedUtc"_q, DateTimeToIsoUtc(info.lastModified()) },
	};
}

QJsonObject Manager::messageContextToJson(
		const MessageEventContext &context) const {
	auto result = QJsonObject{
		{ u"event"_q, MessageEventName(context.event) },
		{ u"hasSession"_q, context.session != nullptr },
		{ u"hasHistory"_q, context.history != nullptr },
		{ u"hasItem"_q, context.item != nullptr },
	};
	if (context.item) {
		const auto fullId = context.item->fullId();
		result.insert(u"peerId"_q, QString::number(fullId.peer.value));
		result.insert(u"msgId"_q, QString::number(fullId.msg.bare));
		result.insert(u"outgoing"_q, context.item->out());
		result.insert(u"text"_q, context.item->originalText().text);
	}
	return result;
}

QJsonObject Manager::commandResultToJson(const CommandResult &result) const {
	return QJsonObject{
		{ u"action"_q, CommandResultActionName(result.action) },
		{ u"replacementText"_q, result.replacementText },
	};
}

QJsonObject Manager::registrationSummaryToJson(
		const PluginRecord &record) const {
	auto windowHandlers = 0;
	for (const auto &entry : _windowHandlers) {
		if (entry.pluginId == record.state.info.id) {
			++windowHandlers;
		}
	}
	auto sessionHandlers = 0;
	for (const auto &entry : _sessionHandlers) {
		if (entry.pluginId == record.state.info.id) {
			++sessionHandlers;
		}
	}
	return QJsonObject{
		{ u"commands"_q, record.commandIds.size() },
		{ u"actions"_q, record.actionIds.size() },
		{ u"panels"_q, record.panelIds.size() },
		{ u"outgoingInterceptors"_q, record.outgoingInterceptorIds.size() },
		{ u"messageObservers"_q, record.messageObserverIds.size() },
		{ u"windowHandlers"_q, windowHandlers },
		{ u"sessionHandlers"_q, sessionHandlers },
	};
}

QJsonObject Manager::hostInfoToJson(const HostInfo &info) const {
	return QJsonObject{
		{ u"structVersion"_q, info.structVersion },
		{ u"apiVersion"_q, info.apiVersion },
		{ u"pointerSize"_q, info.pointerSize },
		{ u"qtMajor"_q, info.qtMajor },
		{ u"qtMinor"_q, info.qtMinor },
		{ u"compilerVersion"_q, info.compilerVersion },
		{ u"appVersion"_q, info.appVersion },
		{ u"compiler"_q, info.compiler },
		{ u"platform"_q, info.platform },
		{ u"workingPath"_q, info.workingPath },
		{ u"pluginsPath"_q, info.pluginsPath },
		{ u"safeModeEnabled"_q, info.safeModeEnabled },
		{ u"runtimeApiEnabled"_q, info.runtimeApiEnabled },
		{ u"runtimeApiPort"_q, info.runtimeApiPort },
		{ u"runtimeApiBaseUrl"_q, info.runtimeApiBaseUrl },
	};
}

QJsonObject Manager::systemInfoToJson(const SystemInfo &info) const {
	return QJsonObject{
		{ u"structVersion"_q, info.structVersion },
		{ u"processId"_q, QString::number(info.processId) },
		{ u"totalMemoryBytes"_q, QString::number(info.totalMemoryBytes) },
		{ u"availableMemoryBytes"_q, QString::number(info.availableMemoryBytes) },
		{ u"logicalCpuCores"_q, info.logicalCpuCores },
		{ u"physicalCpuCores"_q, info.physicalCpuCores },
		{ u"productType"_q, info.productType },
		{ u"productVersion"_q, info.productVersion },
		{ u"prettyProductName"_q, info.prettyProductName },
		{ u"kernelType"_q, info.kernelType },
		{ u"kernelVersion"_q, info.kernelVersion },
		{ u"architecture"_q, info.architecture },
		{ u"buildAbi"_q, info.buildAbi },
		{ u"hostName"_q, info.hostName },
		{ u"userName"_q, info.userName },
		{ u"locale"_q, info.locale },
		{ u"uiLanguage"_q, info.uiLanguage },
		{ u"timeZone"_q, info.timeZone },
	};
}

QJsonObject Manager::runtimeStateToJson() const {
	const auto sessions = sessionStatesToJson();
	const auto windows = windowStatesToJson();
	return QJsonObject{
		{ u"enabled"_q, _runtimeApiEnabled },
		{ u"listening"_q, _runtimeApiServer != nullptr },
		{ u"baseUrl"_q, runtimeApiBaseUrl() },
		{ u"safeModeEnabled"_q, safeModeEnabled() },
		{ u"pluginCount"_q, int(_plugins.size()) },
		{ u"sessionCount"_q, sessions.size() },
		{ u"windowCount"_q, windows.size() },
		{ u"host"_q, hostInfoToJson(hostInfo()) },
	};
}

QJsonArray Manager::pluginStatesToJson() const {
	auto result = QJsonArray();
	for (const auto &state : plugins()) {
		result.push_back(pluginStateToJson(state));
	}
	return result;
}

QJsonArray Manager::sessionStatesToJson() const {
	auto result = QJsonArray();
	if (!Core::App().domain().started()) {
		return result;
	}
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		Q_UNUSED(index);
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		const auto user = session->user();
		auto object = QJsonObject();
		object.insert(u"uniqueId"_q, QString::number(session->uniqueId()));
		object.insert(u"userPeerId"_q, QString::number(session->userPeerId().value));
		object.insert(u"name"_q, user->name());
		object.insert(u"username"_q, user->username());
		object.insert(u"active"_q, session == activeSession());
		object.insert(u"testMode"_q, session->isTestMode());
		result.push_back(object);
	}
	return result;
}

QJsonArray Manager::windowStatesToJson() const {
	auto result = QJsonArray();
	Core::App().forEachWindow([&](not_null<Window::Controller*> window) {
		auto object = QJsonObject{
			{ u"id"_q, QString::number(quintptr(window.get()), 16) },
			{ u"active"_q, window.get() == activeWindow() },
			{ u"primary"_q, window->isPrimary() },
			{ u"hasSession"_q, window->maybeSession() != nullptr },
		};
		if (const auto session = window->maybeSession()) {
			const auto user = session->user();
			object.insert(u"sessionUniqueId"_q, QString::number(session->uniqueId()));
			object.insert(u"userName"_q, user->name());
		}
		result.push_back(object);
	});
	return result;
}

QByteArray Manager::readLogTail(const QString &path, qsizetype maxBytes) const {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return QByteArray();
	}
	const auto size = file.size();
	if (maxBytes > 0 && size > maxBytes) {
		file.seek(size - maxBytes);
	}
	return file.readAll();
}

QByteArray Manager::makeRuntimeApiResponse(
		int statusCode,
		const QJsonObject &payload) const {
	const auto body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	auto response = QByteArray();
	response.append("HTTP/1.1 ");
	response.append(QByteArray::number(statusCode));
	response.append(" ");
	response.append(HttpStatusText(statusCode).toUtf8());
	response.append("\r\n");
	response.append("Content-Type: application/json; charset=utf-8\r\n");
	response.append("Cache-Control: no-store\r\n");
	response.append("Connection: close\r\n");
	response.append("Content-Length: ");
	response.append(QByteArray::number(body.size()));
	response.append("\r\n\r\n");
	response.append(body);
	return response;
}

QJsonObject Manager::runtimeApiEnvelope(
		bool ok,
		QJsonValue result,
		QString error) const {
	return QJsonObject{
		{ u"ok"_q, ok },
		{ u"error"_q, error },
		{ u"result"_q, result.isUndefined() ? QJsonValue() : result },
	};
}

QString Manager::fileSha256(const QString &path) const {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	return QString::fromLatin1(
		QCryptographicHash::hash(
			file.readAll(),
			QCryptographicHash::Sha256).toHex());
}

void Manager::logLoadFailure(const QString &path, const QString &reason) const {
	logEvent(
		u"load"_q,
		u"failed"_q,
		QJsonObject{
			{ u"reason"_q, reason },
			{ u"sha256"_q, fileSha256(path) },
			{ u"file"_q, fileInfoToJson(QFileInfo(path)) },
		});
}

void Manager::scanPlugins(bool metadataOnly) {
	QDir().mkpath(_pluginsPath);
	auto dir = QDir(_pluginsPath);
	const auto files = dir.entryInfoList(
		{ u"*.tgd"_q },
		QDir::Files,
		QDir::Name | QDir::IgnoreCase);
	logEvent(
		u"scan"_q,
		u"start"_q,
		QJsonObject{
			{ u"metadataOnly"_q, metadataOnly },
			{ u"pluginsPath"_q, _pluginsPath },
			{ u"count"_q, files.size() },
		});
	for (const auto &info : files) {
		logEvent(
			u"scan"_q,
			u"discovered-file"_q,
			QJsonObject{
				{ u"metadataOnly"_q, metadataOnly },
				{ u"file"_q, fileInfoToJson(info) },
				{ u"sha256"_q, fileSha256(info.absoluteFilePath()) },
			});
		if (metadataOnly) {
			loadPluginMetadataOnly(info.absoluteFilePath());
		} else {
			loadPlugin(info.absoluteFilePath());
		}
	}
	logEvent(
		u"scan"_q,
		u"finish"_q,
		QJsonObject{
			{ u"metadataOnly"_q, metadataOnly },
			{ u"loadedPlugins"_q, int(_plugins.size()) },
		});
}

void Manager::loadPluginMetadataOnly(const QString &path) {
	auto record = PluginRecord();
	record.state.path = path;
	record.state.enabled = true;
	record.state.loaded = false;
	record.state.info.id = PluginBaseName(path);
	record.state.info.name = record.state.info.id;

	auto previewInfo = PluginInfo();
	auto previewIcon = QString();
	if (ReadPreviewManifest(path, &previewInfo, &previewIcon)) {
		MergePluginInfo(record.state.info, previewInfo);
		logEvent(
			u"plugin"_q,
			u"metadata-preview"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"icon"_q, previewIcon },
				{ u"plugin"_q, pluginInfoToJson(previewInfo) },
			});
	} else {
		logEvent(
			u"plugin"_q,
			u"metadata-preview-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
	}
	record.state.info.id = record.state.info.id.trimmed();
	if (record.state.info.name.isEmpty()) {
		record.state.info.name = record.state.info.id;
	}
	record.state.enabled = !_disabled.contains(record.state.info.id);
	syncRecoveryFlags(record.state);
	const auto index = int(_plugins.size());
	_plugins.push_back(std::move(record));
	_pluginIndexById.insert(_plugins.back().state.info.id, index);
	logEvent(
		u"plugin"_q,
		u"metadata-loaded"_q,
		QJsonObject{
			{ u"state"_q, pluginStateToJson(_plugins.back().state) },
		});
}

void Manager::loadPlugin(const QString &path) {
	auto record = PluginRecord();
	record.state.path = path;
	record.state.enabled = false;
	record.state.loaded = false;
	record.state.info.id = PluginBaseName(path);
	record.state.info.name = record.state.info.id;
	logEvent(
		u"load"_q,
		u"begin"_q,
		QJsonObject{
			{ u"path"_q, path },
			{ u"file"_q, fileInfoToJson(QFileInfo(path)) },
			{ u"sha256"_q, fileSha256(path) },
			{ u"baseName"_q, record.state.info.id },
		});

	auto previewManifestInfo = PluginInfo();
	auto previewManifestIcon = QString();
	const auto hasPreviewManifest = ReadPreviewManifest(
		path,
		&previewManifestInfo,
		&previewManifestIcon);
	if (hasPreviewManifest) {
		MergePluginInfo(record.state.info, previewManifestInfo);
		logEvent(
			u"load"_q,
			u"preview-manifest-read"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"icon"_q, previewManifestIcon },
				{ u"plugin"_q, pluginInfoToJson(previewManifestInfo) },
			});
	} else {
		logEvent(
			u"load"_q,
			u"preview-manifest-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
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
	syncRecoveryFlags(record.state);
	if (!record.state.error.isEmpty()) {
		logLoadFailure(path, record.state.error);
		_plugins.push_back(std::move(record));
		return;
	}
	if (!record.state.enabled) {
		logEvent(
			u"load"_q,
			u"skip-disabled-metadata-only"_q,
			QJsonObject{
				{ u"state"_q, pluginStateToJson(record.state) },
			});
		const auto index = int(_plugins.size());
		_plugins.push_back(std::move(record));
		_pluginIndexById.insert(_plugins.back().state.info.id, index);
		return;
	}

	startRecoveryOperation(u"load"_q, { record.state.info.id }, path);

	auto library = std::make_unique<QLibrary>(path);
	if (!library->load()) {
		record.state.error = library->errorString();
		logLoadFailure(path, record.state.error);
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	const auto previewInfo = reinterpret_cast<PreviewInfoFn>(
		library->resolve(kPreviewInfoName));
	if (previewInfo) {
		logEvent(
			u"load"_q,
			u"preview-export-found"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
		if (const auto preview = previewInfo();
			preview
			&& preview->structVersion == kPreviewInfoVersion
			&& preview->apiVersion == kApiVersion) {
			MergePluginInfo(record.state.info, PluginInfoFromPreview(*preview));
			logEvent(
				u"load"_q,
				u"preview-export-read"_q,
				QJsonObject{
					{ u"path"_q, path },
					{ u"plugin"_q, pluginInfoToJson(record.state.info) },
					{ u"icon"_q, PreviewIconFromInfo(*preview) },
				});
			if (record.state.info.name.isEmpty()) {
				record.state.info.name = record.state.info.id;
			}
		} else {
			logEvent(
				u"load"_q,
				u"preview-export-invalid"_q,
				QJsonObject{
					{ u"path"_q, path },
				});
		}
	} else {
		logEvent(
			u"load"_q,
			u"preview-export-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
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
		finishRecoveryOperation();
		return;
	}
	const auto info = binaryInfo();
	if (!info) {
		record.state.error = u"Plugin binary metadata is null."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	logEvent(
		u"load"_q,
		u"binary-info"_q,
		QJsonObject{
			{ u"path"_q, path },
			{ u"binary"_q, binaryInfoToJson(*info) },
			{ u"hostBinary"_q, binaryInfoToJson(kBinaryInfo) },
		});
	if (const auto mismatch = DescribeBinaryInfoMismatch(*info);
		!mismatch.isEmpty()) {
		record.state.error = mismatch;
		logEvent(
			u"load"_q,
			u"abi-mismatch"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"reason"_q, mismatch },
				{ u"field"_q, DescribeBinaryInfoMismatchField(*info) },
				{ u"pluginBinary"_q, binaryInfoToJson(*info) },
				{ u"hostBinary"_q, binaryInfoToJson(kBinaryInfo) },
			});
		logLoadFailure(path, mismatch + u" ["_q + DescribeBinaryInfo(*info) + u"]"_q);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	if (!entry) {
		record.state.error = u"Missing TgdPluginEntry export."_q;
		logEvent(
			u"load"_q,
			u"entry-export-missing"_q,
			QJsonObject{
				{ u"path"_q, path },
			});
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}

	auto instance = std::unique_ptr<Plugin>();
	try {
		logEvent(
			u"load"_q,
			u"entry-call"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"apiVersion"_q, kApiVersion },
			});
		instance.reset(entry(this, kApiVersion));
	} catch (...) {
		record.state.error = u"Plugin entry failed: "_q + CurrentExceptionText();
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
	if (!instance) {
		record.state.error = u"Plugin entry returned null."_q;
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}

	if (hasPreviewManifest) {
		logEvent(
			u"load"_q,
			u"info-call-skipped-preview-manifest"_q,
			QJsonObject{
				{ u"path"_q, path },
				{ u"pluginId"_q, record.state.info.id },
			});
	} else {
		try {
			logEvent(
				u"load"_q,
				u"info-call"_q,
				QJsonObject{
					{ u"path"_q, path },
				});
			record.state.info = instance->info();
		} catch (...) {
			record.state.error = u"Plugin info() failed: "_q + CurrentExceptionText();
			logLoadFailure(path, record.state.error);
			instance.reset();
			library->unload();
			_plugins.push_back(std::move(record));
			finishRecoveryOperation();
			return;
		}
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
	syncRecoveryFlags(record.state);

	if (!record.state.error.isEmpty()) {
		logLoadFailure(path, record.state.error);
		record.state.enabled = false;
		instance.reset();
		library->unload();
		record.state.loaded = false;
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}

	if (record.state.enabled) {
		record.library = std::move(library);
		record.instance = std::move(instance);
		record.state.loaded = true;
		logEvent(
			u"load"_q,
			u"library-loaded"_q,
			QJsonObject{
				{ u"state"_q, pluginStateToJson(record.state) },
			});
	} else {
		instance.reset();
		library->unload();
		logEvent(
			u"load"_q,
			u"library-not-enabled"_q,
			QJsonObject{
				{ u"state"_q, pluginStateToJson(record.state) },
			});
	}

	const auto index = int(_plugins.size());
	_plugins.push_back(std::move(record));
	_pluginIndexById.insert(_plugins.back().state.info.id, index);

	if (_plugins.back().state.enabled) {
		startRecoveryOperation(
			u"onload"_q,
			{ _plugins.back().state.info.id },
			_plugins.back().state.path);
		_registeringPluginId = _plugins.back().state.info.id;
			try {
				logEvent(
					u"load"_q,
					u"onload-call"_q,
					QJsonObject{
						{ u"pluginId"_q, _plugins.back().state.info.id },
						{ u"path"_q, _plugins.back().state.path },
					});
				_plugins.back().instance->onLoad();
		} catch (...) {
			const auto pluginId = _plugins.back().state.info.id;
			_registeringPluginId.clear();
			disablePlugin(
				pluginId,
				u"onLoad failed: "_q + CurrentExceptionText());
			showToast(PluginUiText(
				u"Plugin failed to load and was disabled."_q,
				u"Плагин упал при загрузке и был выключен."_q));
			finishRecoveryOperation();
			return;
			}
			_registeringPluginId.clear();
			logEvent(
				u"load"_q,
				u"success"_q,
				QJsonObject{
					{ u"state"_q, pluginStateToJson(_plugins.back().state) },
					{ u"registrations"_q, registrationSummaryToJson(_plugins.back()) },
				});
			finishRecoveryOperation();
		} else {
			_plugins.back().instance.reset();
			_plugins.back().library.reset();
	}
	finishRecoveryOperation();
}

void Manager::unloadAll() {
	logEvent(
		u"unload"_q,
		u"begin"_q,
		QJsonObject{
			{ u"count"_q, int(_plugins.size()) },
		});
	for (auto &plugin : _plugins) {
		if (plugin.state.loaded && plugin.instance) {
			startRecoveryOperation(
				u"unload"_q,
				{ plugin.state.info.id },
				plugin.state.path);
			_registeringPluginId = plugin.state.info.id;
			try {
				logEvent(
					u"unload"_q,
					u"onunload-call"_q,
					QJsonObject{
						{ u"pluginId"_q, plugin.state.info.id },
						{ u"path"_q, plugin.state.path },
						{ u"registrations"_q, registrationSummaryToJson(plugin) },
					});
				plugin.instance->onUnload();
			} catch (...) {
				plugin.state.error = u"onUnload failed: "_q + CurrentExceptionText();
				logEvent(
					u"unload"_q,
					u"onunload-failed"_q,
					QJsonObject{
						{ u"pluginId"_q, plugin.state.info.id },
						{ u"reason"_q, plugin.state.error },
					});
			}
			_registeringPluginId.clear();
			finishRecoveryOperation();
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
	logEvent(u"unload"_q, u"finish"_q);
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
	disablePlugin(pluginId, reason, false, QString());
}

void Manager::disablePlugin(
		const QString &pluginId,
		const QString &reason,
		bool disabledByRecovery,
		const QString &recoveryReason) {
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
	logEvent(
		u"plugin"_q,
		u"disabled"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"reason"_q, reason.trimmed() },
			{ u"disabledByRecovery"_q, disabledByRecovery },
			{ u"recoveryReason"_q, recoveryReason },
			{ u"stateBefore"_q, pluginStateToJson(record->state) },
			{ u"registrationsBefore"_q, registrationSummaryToJson(*record) },
		});
	record->state.enabled = false;
	record->state.loaded = false;
	if (!reason.trimmed().isEmpty()) {
		record->state.error = reason.trimmed();
	}
	record->state.disabledByRecovery = disabledByRecovery;
	record->state.recoverySuspected = disabledByRecovery;
	record->state.recoveryReason = recoveryReason;
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
	if (disabledByRecovery) {
		_disabledByRecovery.insert(pluginId);
	} else {
		_disabledByRecovery.remove(pluginId);
	}
	saveConfig();
	saveRecoveryState();
	logEvent(
		u"plugin"_q,
		u"disabled-finished"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"stateAfter"_q, pluginStateToJson(record->state) },
		});
}

void Manager::updateMessageObserverSubscriptions() {
	_messageObserverLifetime.destroy();
	if (!_activeSession || _messageObservers.isEmpty()) {
		logEvent(
			u"observer"_q,
			u"subscriptions-cleared"_q,
			QJsonObject{
				{ u"hasActiveSession"_q, _activeSession != nullptr },
				{ u"observerCount"_q, _messageObservers.size() },
			});
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
	logEvent(
		u"observer"_q,
		u"subscriptions-updated"_q,
		QJsonObject{
			{ u"observerCount"_q, _messageObservers.size() },
			{ u"wantsNew"_q, wantsNew },
			{ u"wantsEdited"_q, wantsEdited },
			{ u"wantsDeleted"_q, wantsDeleted },
			{ u"sessionUniqueId"_q, _activeSession ? QString::number(_activeSession->uniqueId()) : QString() },
		});
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
	logEvent(
		u"session"_q,
		u"active-changed"_q,
		QJsonObject{
			{ u"hasSession"_q, session != nullptr },
			{ u"sessionUniqueId"_q, session ? QString::number(session->uniqueId()) : QString() },
			{ u"handlerCount"_q, int(_sessionHandlers.size()) },
		});
	const auto handlers = _sessionHandlers;
	for (const auto &entry : handlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			startRecoveryOperation(u"session"_q, { entry.pluginId });
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			logEvent(
				u"session"_q,
				u"invoke-callback"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"hasSession"_q, session != nullptr },
					{ u"sessionUniqueId"_q, session ? QString::number(session->uniqueId()) : QString() },
				});
			entry.handler(session);
			_registeringPluginId = previousPluginId;
			logEvent(
				u"session"_q,
				u"callback-success"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
				});
			finishRecoveryOperation();
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"session"_q,
				u"callback-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"reason"_q, CurrentExceptionText() },
				});
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Session callback failed: "_q + CurrentExceptionText());
			}
			showToast(PluginUiText(
				u"Plugin session callback failed."_q,
				u"Session callback плагина завершился с ошибкой."_q));
			finishRecoveryOperation();
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
		startRecoveryOperation(u"observer"_q, { entry.pluginId });
		const auto previousPluginId = _registeringPluginId;
		_registeringPluginId = entry.pluginId;
		logEvent(
			u"observer"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(entry.id) },
				{ u"pluginId"_q, entry.pluginId },
				{ u"context"_q, messageContextToJson(callContext) },
			});
		entry.handler(callContext);
		_registeringPluginId = previousPluginId;
		logEvent(
			u"observer"_q,
			u"success"_q,
			QJsonObject{
				{ u"id"_q, QString::number(entry.id) },
				{ u"pluginId"_q, entry.pluginId },
			});
		finishRecoveryOperation();
	} catch (...) {
		_registeringPluginId.clear();
		logEvent(
			u"observer"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(entry.id) },
				{ u"pluginId"_q, entry.pluginId },
				{ u"reason"_q, CurrentExceptionText() },
				{ u"context"_q, messageContextToJson(callContext) },
			});
		disablePlugin(
			entry.pluginId,
			u"Message observer failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin observer failed and was disabled."_q,
			u"Наблюдатель плагина завершился с ошибкой и был выключен."_q));
		finishRecoveryOperation();
	}
}

} // namespace Plugins
