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
#include "core/update_checker.h"
#include "data/data_changes.h"
#include "data/data_history_messages.h"
#include "history/history_item.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "logs.h"
#include "storage/storage_sparse_ids_list.h"
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
#include <QtCore/QEventLoop>
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
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QAbstractSocket>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif // _WIN32

namespace Plugins {
namespace {

constexpr auto kPluginsFolder = "tdata/plugins";
constexpr auto kConfigFile = "tdata/plugins.json";
constexpr auto kClientLogFile = "client.log";
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

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

[[nodiscard]] QString CompactClientLogText(QString text, int maxLength = 640) {
	text.replace(u'\r', u' ');
	text.replace(u'\n', u' ');
	while (text.contains(u"  "_q)) {
		text.replace(u"  "_q, u" "_q);
	}
	text = text.trimmed();
	if (text.size() > maxLength) {
		text = text.left(std::max(0, maxLength - 1)).trimmed() + u"…"_q;
	}
	return text;
}

[[nodiscard]] bool ShouldMirrorPluginEventToClient(
		const QString &phase,
		const QString &event) {
	if (phase == u"runtime-api"_q
		|| phase == u"recovery"_q
		|| phase == u"safe-mode"_q) {
		return true;
	}
	if (phase == u"package"_q
		&& (event.startsWith(u"install"_q)
			|| event.startsWith(u"remove"_q)
			|| event.startsWith(u"rollback"_q))) {
		return true;
	}
	if (phase == u"manager"_q && event == u"reload-requested"_q) {
		return true;
	}
	if (phase == u"ui"_q && event != u"toast"_q) {
		return true;
	}
	return event.contains(u"failed"_q)
		|| event.contains(u"rejected"_q)
		|| event.contains(u"missing"_q)
		|| event.contains(u"disabled"_q)
		|| event.contains(u"mismatch"_q)
		|| event.contains(u"listen"_q)
		|| event.contains(u"stopped"_q)
		|| event.contains(u"rotated"_q);
}

[[nodiscard]] int VisibleTopLevelWidgetCount() {
	auto count = 0;
	for (auto *widget : QApplication::topLevelWidgets()) {
		if (!widget
			|| !widget->isWindow()
			|| widget->parentWidget()) {
			continue;
		}
		const auto type = widget->windowType();
		if (type == Qt::Dialog
			|| type == Qt::Popup
			|| type == Qt::Tool
			|| type == Qt::ToolTip
			|| type == Qt::Sheet
			|| type == Qt::Drawer
			|| type == Qt::SplashScreen
			|| type == Qt::SubWindow) {
			continue;
		}
		if (widget->isVisible() && !widget->isMinimized()) {
			++count;
		}
	}
	return count;
}

struct UiRecoveryAttemptResult {
	int visibleBefore = 0;
	int visibleAfter = 0;
	int updatedWindows = 0;
	bool primaryExists = false;
	bool primaryWasVisible = false;
	bool primaryNowVisible = false;
	bool primaryShown = false;
};

[[nodiscard]] UiRecoveryAttemptResult AttemptPluginUiRecoveryNow() {
	auto result = UiRecoveryAttemptResult();
	result.visibleBefore = VisibleTopLevelWidgetCount();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
	for (auto *widget : QApplication::topLevelWidgets()) {
		if (!widget || !widget->isWindow()) {
			continue;
		}
		widget->update();
		widget->repaint();
		++result.updatedWindows;
	}
	if (const auto primary = Core::App().activePrimaryWindow()) {
		if (const auto widget = primary->widget()) {
			result.primaryExists = true;
			result.primaryWasVisible = widget->isVisible();
			if (!widget->isVisible()) {
				widget->showNormal();
				widget->raise();
				widget->activateWindow();
				result.primaryShown = true;
			}
			widget->update();
			widget->repaint();
			result.primaryNowVisible = widget->isVisible();
		}
	}
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
	result.visibleAfter = VisibleTopLevelWidgetCount();
	return result;
}

void SchedulePluginUiRecoveryAttempt(QString pluginId, QString reason) {
	const auto application = QCoreApplication::instance();
	if (!application) {
		return;
	}
	pluginId = CompactClientLogText(std::move(pluginId), 160);
	reason = CompactClientLogText(std::move(reason));
	QTimer::singleShot(0, application, [pluginId = std::move(pluginId), reason = std::move(reason)] {
		const auto attempt = AttemptPluginUiRecoveryNow();
		Logs::writeClient(QString::fromLatin1(
			"[plugins-ui] recovery-attempt plugin=%1 reason=%2 visibleBefore=%3 visibleAfter=%4 primaryExists=%5 primaryWasVisible=%6 primaryNowVisible=%7 primaryShown=%8 updatedWindows=%9 appState=%10")
			.arg(pluginId)
			.arg(reason)
			.arg(attempt.visibleBefore)
			.arg(attempt.visibleAfter)
			.arg(attempt.primaryExists ? u"true"_q : u"false"_q)
			.arg(attempt.primaryWasVisible ? u"true"_q : u"false"_q)
			.arg(attempt.primaryNowVisible ? u"true"_q : u"false"_q)
			.arg(attempt.primaryShown ? u"true"_q : u"false"_q)
			.arg(attempt.updatedWindows)
			.arg(int(QGuiApplication::applicationState())));
		if (!Core::App().plugins().safeModeEnabled()
			&& QGuiApplication::applicationState() == Qt::ApplicationActive
			&& attempt.primaryExists
			&& !attempt.primaryNowVisible) {
			Logs::writeClient(QString::fromLatin1(
				"[plugins-ui] recovery-escalation plugin=%1 action=enable-safe-mode reason=%2")
				.arg(pluginId, reason));
			Core::App().plugins().setSafeModeEnabled(true);
		}
	});
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
	} else if (kind == u"settings"_q) {
		return PluginUiText(
			u"plugin settings"_q,
			u"настроек плагина"_q);
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

void FlushPluginUnload() {
	for (auto attempt = 0; attempt != 6; ++attempt) {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents();
		QThread::msleep(25);
	}
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
			p.drawText(rect, Qt::AlignCenter, QString::fromUtf8("🦆"));
		}, lifetime());
	}
};

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

QString SehExceptionText(unsigned int code) {
	return QString::fromLatin1("Structured exception (0x%1).")
		.arg(QString::number(quint64(code), 16)
			.rightJustified(8, QLatin1Char('0'))
			.toUpper());
}

[[nodiscard]] QString RuntimeSocketPeerLabel(const QTcpSocket *socket) {
	if (!socket) {
		return QString();
	}
	return socket->peerAddress().toString()
		+ u":"_q
		+ QString::number(socket->peerPort());
}

[[nodiscard]] int RuntimeResponseStatusCode(const QByteArray &response) {
	const auto end = response.indexOf("\r\n");
	const auto line = response.left(end < 0 ? response.size() : end);
	const auto parts = line.split(' ');
	return (parts.size() >= 2) ? parts[1].toInt() : 0;
}

[[nodiscard]] QString RuntimeBodyPreview(const QByteArray &body) {
	auto text = QString::fromUtf8(body).trimmed();
	if (text.isEmpty()) {
		return QString();
	}
	if (text.size() > 400) {
		text = text.left(399) + u"…"_q;
	}
	return CompactClientLogText(std::move(text), 400);
}

#if defined(_WIN32) && defined(_MSC_VER)

constexpr auto kMsvcCppExceptionCode = 0xE06D7363u;

int PluginSehFilter(unsigned int code) noexcept {
	return (code == kMsvcCppExceptionCode)
		? EXCEPTION_CONTINUE_SEARCH
		: EXCEPTION_EXECUTE_HANDLER;
}

bool InvokeWithSehGuard(
		void (*callback)(void*) noexcept,
		void *opaque,
		unsigned int *exceptionCode) noexcept {
	__try {
		callback(opaque);
		if (exceptionCode) {
			*exceptionCode = 0;
		}
		return true;
	} __except (PluginSehFilter(GetExceptionCode())) {
		if (exceptionCode) {
			*exceptionCode = GetExceptionCode();
		}
		return false;
	}
}

#endif // _WIN32 && _MSC_VER

template <typename Callback>
void InvokePluginCallbackOrThrow(Callback &&callback) {
#if defined(_WIN32) && defined(_MSC_VER)
	unsigned int code = 0;
	auto *opaque = static_cast<void*>(&callback);
	const auto ok = InvokeWithSehGuard(
		+[](void *context) noexcept {
			auto *fn = static_cast<std::remove_reference_t<Callback>*>(context);
			(*fn)();
		},
		opaque,
		&code);
	if (!ok) {
		throw std::runtime_error(SehExceptionText(code).toStdString());
	}
#else // _WIN32 && _MSC_VER
	callback();
#endif // _WIN32 && _MSC_VER
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
	const auto compiler = QString::fromLatin1(
		info.compiler ? info.compiler : "unknown");
	const auto platform = QString::fromLatin1(
		info.platform ? info.platform : "unknown");
	auto result = QString::fromLatin1(
		"api=%1 ptr=%2 qt=%3.%4 compiler=%5/%6 platform=%7");
	return result
		.arg(info.apiVersion)
		.arg(info.pointerSize)
		.arg(info.qtMajor)
		.arg(info.qtMinor)
		.arg(compiler)
		.arg(info.compilerVersion)
		.arg(platform);
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

struct PackageLoadabilityCheckResult {
	bool compatible = false;
	bool libraryLoaded = false;
	bool previewExportFound = false;
	bool previewExportValid = false;
	bool binaryInfoFound = false;
	bool entryFound = false;
	PluginInfo previewInfo;
	QString previewIcon;
	QString error;
	QString binarySummary;
	QString mismatchField;
};

PackageLoadabilityCheckResult CheckPackageLoadability(const QString &path) {
	auto result = PackageLoadabilityCheckResult();
	auto library = QLibrary(path);
	if (!library.load()) {
		const auto message = library.errorString().trimmed();
		result.error = message.isEmpty()
			? u"Could not load the plugin package as a library."_q
			: message;
		return result;
	}
	result.libraryLoaded = true;

	const auto finish = [&](QString error = QString()) {
		result.compatible = error.isEmpty();
		if (!error.isEmpty()) {
			result.error = std::move(error);
		}
		library.unload();
		return result;
	};

	if (const auto previewInfo = reinterpret_cast<PreviewInfoFn>(
			library.resolve(kPreviewInfoName))) {
		result.previewExportFound = true;
		auto preview = static_cast<const Plugins::PreviewInfo*>(nullptr);
		try {
			InvokePluginCallbackOrThrow([&] {
				preview = previewInfo();
			});
		} catch (...) {
			return finish(u"Plugin preview export failed: "_q + CurrentExceptionText());
		}
		if (preview
			&& preview->structVersion == kPreviewInfoVersion
			&& preview->apiVersion == kApiVersion) {
			result.previewExportValid = true;
			result.previewInfo = PluginInfoFromPreview(*preview);
			result.previewIcon = PreviewIconFromInfo(*preview);
		}
	}

	const auto binaryInfo = reinterpret_cast<BinaryInfoFn>(
		library.resolve(kBinaryInfoName));
	if (!binaryInfo) {
		return finish(u"Missing TgdPluginBinaryInfo export."_q);
	}
	result.binaryInfoFound = true;

	auto info = static_cast<const Plugins::BinaryInfo*>(nullptr);
	try {
		InvokePluginCallbackOrThrow([&] {
			info = binaryInfo();
		});
	} catch (...) {
		return finish(u"TgdPluginBinaryInfo failed: "_q + CurrentExceptionText());
	}
	if (!info) {
		return finish(u"Plugin binary metadata is null."_q);
	}
	result.binarySummary = DescribeBinaryInfo(*info);
	if (const auto mismatch = DescribeBinaryInfoMismatch(*info);
		!mismatch.isEmpty()) {
		result.mismatchField = DescribeBinaryInfoMismatchField(*info);
		return finish(mismatch);
	}

	result.entryFound = (library.resolve(kEntryName) != nullptr);
	if (!result.entryFound) {
		return finish(u"Missing TgdPluginEntry export."_q);
	}

	return finish();
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

QString SettingControlName(const Plugins::SettingControl control) {
	using Control = Plugins::SettingControl;
	switch (control) {
	case Control::Toggle:
		return u"toggle"_q;
	case Control::IntSlider:
		return u"int_slider"_q;
	case Control::TextInput:
		return u"text_input"_q;
	case Control::ActionButton:
		return u"action_button"_q;
	case Control::InfoText:
		return u"info_text"_q;
	}
	return u"unknown"_q;
}

bool HasAnySettings(const Plugins::SettingsPageDescriptor &descriptor) {
	for (const auto &section : descriptor.sections) {
		if (!section.settings.isEmpty()) {
			return true;
		}
	}
	return false;
}

Plugins::SettingDescriptor NormalizeSettingDescriptor(
		Plugins::SettingDescriptor descriptor) {
	descriptor.id = descriptor.id.trimmed();
	descriptor.title = descriptor.title.trimmed();
	descriptor.description = descriptor.description.trimmed();
	descriptor.textValue = descriptor.textValue.trimmed();
	descriptor.placeholderText = descriptor.placeholderText.trimmed();
	descriptor.valueSuffix = descriptor.valueSuffix.trimmed();
	descriptor.buttonText = descriptor.buttonText.trimmed();
	switch (descriptor.type) {
	case Plugins::SettingControl::Toggle:
	case Plugins::SettingControl::TextInput:
	case Plugins::SettingControl::ActionButton:
	case Plugins::SettingControl::InfoText:
		break;
	case Plugins::SettingControl::IntSlider: {
		if (descriptor.intMinimum > descriptor.intMaximum) {
			std::swap(descriptor.intMinimum, descriptor.intMaximum);
		}
		if (descriptor.intStep <= 0) {
			descriptor.intStep = 1;
		}
		descriptor.intValue = std::clamp(
			descriptor.intValue,
			descriptor.intMinimum,
			descriptor.intMaximum);
	} break;
	}
	return descriptor;
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

QByteArray RuntimeStatusText(int code) {
	switch (code) {
	case 200: return "OK";
	case 201: return "Created";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	default: return "OK";
	}
}

QByteArray RuntimeJsonResponse(
		int code,
		const QJsonDocument &document) {
	const auto payload = document.toJson(QJsonDocument::Compact);
	auto response = QByteArray();
	response.reserve(payload.size() + 256);
	response += "HTTP/1.1 ";
	response += QByteArray::number(code);
	response += ' ';
	response += RuntimeStatusText(code);
	response += "\r\nContent-Type: application/json; charset=utf-8";
	response += "\r\nConnection: close";
	response += "\r\nContent-Length: ";
	response += QByteArray::number(payload.size());
	response += "\r\n\r\n";
	response += payload;
	return response;
}

QByteArray RuntimeErrorResponse(
		int code,
		const QString &message) {
	return RuntimeJsonResponse(
		code,
		QJsonDocument(QJsonObject{
			{ u"ok"_q, false },
			{ u"error"_q, message },
		}));
}

QByteArray RuntimeOkResponse(const QJsonObject &payload) {
	auto result = payload;
	if (!result.contains(u"ok"_q)) {
		result.insert(u"ok"_q, true);
	}
	return RuntimeJsonResponse(200, QJsonDocument(result));
}

bool ParseRuntimeHttpRequest(
		const QByteArray &buffer,
		QString &method,
		QString &target,
		QHash<QByteArray, QByteArray> &headers,
		QByteArray &body,
		qsizetype &consumed,
		QString &error) {
	const auto headerEnd = buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		error = u"incomplete"_q;
		return false;
	}
	const auto headersPart = buffer.left(headerEnd);
	const auto lines = headersPart.split('\n');
	if (lines.isEmpty()) {
		error = u"empty request"_q;
		return false;
	}
	const auto requestLine = QByteArray(lines.front()).trimmed();
	const auto requestParts = requestLine.split(' ');
	if (requestParts.size() < 2) {
		error = u"invalid request line"_q;
		return false;
	}
	method = QString::fromLatin1(requestParts[0]).trimmed().toUpper();
	target = QString::fromLatin1(requestParts[1]).trimmed();
	headers.clear();
	for (auto i = 1; i < lines.size(); ++i) {
		const auto line = QByteArray(lines[i]).trimmed();
		if (line.isEmpty()) {
			continue;
		}
		const auto colon = line.indexOf(':');
		if (colon <= 0) {
			continue;
		}
		const auto key = line.left(colon).trimmed().toLower();
		const auto value = line.mid(colon + 1).trimmed();
		headers.insert(key, value);
	}
	auto contentLength = 0;
	if (const auto value = headers.value("content-length"); !value.isEmpty()) {
		contentLength = value.toInt();
		if (contentLength < 0) {
			error = u"invalid content-length"_q;
			return false;
		}
	}
	const auto bodyOffset = headerEnd + 4;
	if (buffer.size() < (bodyOffset + contentLength)) {
		error = u"incomplete"_q;
		return false;
	}
	body = buffer.mid(bodyOffset, contentLength);
	consumed = bodyOffset + contentLength;
	return true;
}

QString RuntimeSourceBadgeKind(const PluginState &state) {
	return state.sourceVerified ? u"confirmed"_q : u"unconfirmed"_q;
}

QJsonObject RuntimePluginJson(const PluginState &state) {
	const auto source = QJsonObject{
		{ u"verified"_q, state.sourceVerified },
		{ u"badge"_q, state.sourceTrustText },
		{ u"badgeKind"_q, RuntimeSourceBadgeKind(state) },
		{ u"details"_q, state.sourceTrustDetails },
		{ u"reason"_q, state.sourceTrustReason },
		{ u"channelId"_q, QString::number(state.sourceChannelId) },
		{ u"messageId"_q, QString::number(state.sourceMessageId) },
		{ u"sha256"_q, state.sha256 },
	};
	return QJsonObject{
		{ u"id"_q, state.info.id },
		{ u"name"_q, state.info.name },
		{ u"version"_q, state.info.version },
		{ u"author"_q, state.info.author },
		{ u"description"_q, state.info.description },
		{ u"path"_q, state.path },
		{ u"sha256"_q, state.sha256 },
		{ u"enabled"_q, state.enabled },
		{ u"loaded"_q, state.loaded },
		{ u"error"_q, state.error },
		{ u"disabledByRecovery"_q, state.disabledByRecovery },
		{ u"recoverySuspected"_q, state.recoverySuspected },
		{ u"recoveryReason"_q, state.recoveryReason },
		{ u"sourceVerified"_q, state.sourceVerified },
		{ u"sourceBadgeKind"_q, RuntimeSourceBadgeKind(state) },
		{ u"sourceTrustText"_q, state.sourceTrustText },
		{ u"sourceTrustDetails"_q, state.sourceTrustDetails },
		{ u"sourceTrustReason"_q, state.sourceTrustReason },
		{ u"sourceChannelId"_q, QString::number(state.sourceChannelId) },
		{ u"sourceMessageId"_q, QString::number(state.sourceMessageId) },
		{ u"source"_q, source },
	};
}

QStringList RuntimeTailLines(const QString &path, int wantedLines) {
	if (wantedLines <= 0) {
		return {};
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return {};
	}
	constexpr auto kBlock = qint64(4096);
	auto buffer = QByteArray();
	auto left = file.size();
	auto lines = 0;
	while (left > 0 && lines <= wantedLines + 16) {
		const auto take = std::min(kBlock, left);
		left -= take;
		file.seek(left);
		buffer.prepend(file.read(take));
		lines = buffer.count('\n');
	}
	auto text = QString::fromUtf8(buffer);
	auto split = text.split(QChar::fromLatin1('\n'));
	for (auto &line : split) {
		line = line.trimmed();
	}
	split.erase(
		std::remove_if(split.begin(), split.end(), [](const QString &line) {
			return line.isEmpty();
		}),
		split.end());
	if (split.size() > wantedLines) {
		split = split.mid(split.size() - wantedLines);
	}
	return split;
}

QString RuntimeApiAdvertisedHost() {
	const auto interfaces = QNetworkInterface::allInterfaces();
	for (const auto &iface : interfaces) {
		if (!(iface.flags() & QNetworkInterface::IsUp)
			|| !(iface.flags() & QNetworkInterface::IsRunning)
			|| (iface.flags() & QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const auto &entry : iface.addressEntries()) {
			const auto ip = entry.ip();
			if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
				continue;
			}
			if (ip.isLoopback()) {
				continue;
			}
			const auto value = ip.toString();
			if (value.startsWith(u"169.254."_q)) {
				continue;
			}
			return value;
		}
	}
	return u"127.0.0.1"_q;
}

QHostAddress RuntimeApiListenAddress() {
	return QHostAddress::AnyIPv4;
}

std::optional<PeerId> RuntimePeerIdFromString(const QString &text) {
	bool ok = false;
	const auto value = text.trimmed().toULongLong(&ok);
	if (!ok || value == 0) {
		return std::nullopt;
	}
	return PeerId(PeerIdHelper{ value });
}

QJsonObject RuntimeMessageJson(
		not_null<HistoryItem*> item,
		PeerId peerId) {
	const auto fullId = item->fullId();
	auto result = QJsonObject();
	result.insert(u"peerId"_q, QString::number(peerId.value));
	result.insert(u"id"_q, item->id.bare);
	result.insert(u"fullMsgPeerId"_q, QString::number(fullId.peer.value));
	result.insert(u"date"_q, item->date());
	result.insert(u"out"_q, item->out());
	result.insert(u"text"_q, item->notificationText().text);
	if (const auto from = item->from()) {
		result.insert(u"fromPeerId"_q, QString::number(from->id.value));
	}
	return result;
}

QString RuntimeUpdateStateText(Core::UpdateChecker::State state) {
	switch (state) {
	case Core::UpdateChecker::State::Download:
		return u"download"_q;
	case Core::UpdateChecker::State::Ready:
		return u"ready"_q;
	case Core::UpdateChecker::State::None:
	default:
		return u"idle"_q;
	}
}

QString RuntimeUpdateChannelText(Core::UpdateChannel channel) {
	switch (channel) {
	case Core::UpdateChannel::DevBeta:
		return u"beta"_q;
	case Core::UpdateChannel::Alpha:
		return u"alpha"_q;
	case Core::UpdateChannel::Stable:
	default:
		return u"stable"_q;
	}
}

QJsonObject RuntimeReleaseInfoJson(const Core::UpdateReleaseInfo &info) {
	return QJsonObject{
		{ u"available"_q, info.available },
		{ u"channel"_q, RuntimeUpdateChannelText(info.channel) },
		{ u"version"_q, info.version },
		{ u"versionText"_q, info.versionText },
		{ u"title"_q, info.title },
		{ u"url"_q, info.url },
		{ u"changelog"_q, info.changelog },
		{ u"changelogLoading"_q, info.changelogLoading },
		{ u"changelogFailed"_q, info.changelogFailed },
	};
}

QJsonObject RuntimeUpdatesJson() {
	const auto checker = Core::UpdateChecker();
	const auto state = checker.state();
	return QJsonObject{
		{ u"autoUpdateEnabled"_q, cAutoUpdate() },
		{ u"updaterDisabled"_q, Core::UpdaterDisabled() },
		{ u"state"_q, RuntimeUpdateStateText(state) },
		{ u"ready"_q, state == Core::UpdateChecker::State::Ready },
		{ u"downloadedBytes"_q, checker.already() },
		{ u"totalBytes"_q, checker.size() },
		{ u"currentVersion"_q, AppVersion },
		{ u"currentVersionText"_q, Core::FormatVersionWithBuild(AppVersion) },
		{ u"preferredChannel"_q, cInstallBetaVersion() ? u"beta"_q : u"stable"_q },
		{ u"installBetaVersion"_q, cInstallBetaVersion() },
		{ u"alphaVersion"_q, QString::number(cAlphaVersion()) },
		{ u"devHooksConfigPath"_q, QDir::toNativeSeparators(Core::DevUpdateHooksConfigPath()) },
		{ u"devHooksSummary"_q, Core::ActiveDevUpdateHooksSummary() },
		{ u"devHooksState"_q, Core::DescribeDevUpdateHooksState() },
		{ u"releasesPageUrl"_q, Core::CurrentUpdateFeedPageUrl() },
		{ u"release"_q, RuntimeReleaseInfoJson(checker.releaseInfo()) },
	};
}

bool RuntimeResolveInstallBetaChannel(const QString &channel, bool &enabled) {
	const auto normalized = channel.trimmed().toLower();
	if (normalized == u"stable"_q
		|| normalized == u"release"_q
		|| normalized == u"off"_q) {
		enabled = false;
		return true;
	}
	if (normalized == u"beta"_q
		|| normalized == u"dev"_q
		|| normalized == u"devbeta"_q) {
		enabled = true;
		return true;
	}
	return false;
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
		u"Astrogram Desktop plugin recovery log"_q,
		u"Лог восстановления плагинов Astrogram Desktop"_q));
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
			u"Astrogram noticed a crash during "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nSuspected plugin: "_q
				+ pluginText
				+ u"\n\nSafe mode was enabled automatically, the plugin was turned off, and the recovery log was copied to the clipboard."_q,
			u"Astrogram заметил сбой во время "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nПодозреваемый плагин: "_q
				+ pluginText
				+ u"\n\nБезопасный режим включён автоматически, плагин выключен, лог восстановления уже скопирован в буфер обмена."_q)
		: PluginUiText(
			u"Astrogram noticed a crash during "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nSuspected plugins: "_q
				+ pluginText
				+ u"\n\nSafe mode was enabled automatically, the listed plugins were turned off, and the recovery log was copied to the clipboard."_q,
			u"Astrogram заметил сбой во время "_q
				+ RecoveryOperationText(_recoveryNotice.kind)
				+ u".\n\nПодозреваемые плагины: "_q
				+ pluginText
				+ u"\n\nБезопасный режим включён автоматически, указанные плагины выключены, лог восстановления уже скопирован в буфер обмена."_q);

	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(title));
		box->addLeftButton(rpl::single(
			PluginUiText(u"Copy"_q, u"Копировать"_q)), [=] {
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
		box->addButton(rpl::single(
			PluginUiText(u"Continue"_q, u"Продолжить"_q)), [=] {
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
	ensureRuntimeApiServerState();
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
	Logs::writeClient(QString::fromLatin1("[plugins] reload requested: safeMode=%1 knownPlugins=%2")
		.arg(safeModeEnabled() ? u"true"_q : u"false"_q)
		.arg(_plugins.size()));
	logEvent(
		u"manager"_q,
		u"reload-requested"_q,
		QJsonObject{
			{ u"safeMode"_q, safeModeEnabled() },
			{ u"knownPlugins"_q, int(_plugins.size()) },
		});
	unloadAll();
	loadConfig();
	ensureRuntimeApiServerState();
	loadRecoveryState();
	FlushPluginUnload();
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

std::vector<PluginState> Manager::visiblePluginStatesFromRecords() const {
	auto result = std::vector<PluginState>();
	result.reserve(_plugins.size());
	for (const auto &plugin : _plugins) {
		auto state = plugin.state;
		syncRecoveryFlags(state);
		syncSourceTrustState(state);
		result.push_back(std::move(state));
	}
	return result;
}

void Manager::beginUiTransientPluginSnapshot() {
	_uiTransientPlugins = visiblePluginStatesFromRecords();
	_uiTransientPluginsActive = !_uiTransientPlugins.empty();
}

void Manager::finishUiTransientPluginSnapshot() {
	_uiTransientPlugins = visiblePluginStatesFromRecords();
	_uiTransientPluginsActive = false;
}

std::vector<PluginState> Manager::plugins() const {
	if (_uiTransientPluginsActive
		&& _plugins.empty()
		&& !_uiTransientPlugins.empty()) {
		return _uiTransientPlugins;
	}
	return visiblePluginStatesFromRecords();
}

rpl::producer<ManagerStateChange> Manager::stateChanges() const {
	return _stateChanges.events();
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
	if (!enabled) {
		const auto recovered = _disabledByRecovery.values();
		for (const auto &pluginId : recovered) {
			_disabled.remove(pluginId);
			clearRecoveryDisabled(pluginId);
		}
		saveConfig();
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

bool Manager::runtimeApiEnabled() const {
	return _runtimeApiEnabled;
}

int Manager::runtimeApiPort() const {
	return _runtimeApiPort;
}

bool Manager::setRuntimeApiEnabled(bool enabled) {
	if (_runtimeApiEnabled == enabled) {
		return true;
	}
	_runtimeApiEnabled = enabled;
	ensureRuntimeApiServerState();
	saveConfig();
	logEvent(
		u"runtime-api"_q,
		u"set-enabled"_q,
		QJsonObject{
			{ u"enabled"_q, enabled },
			{ u"port"_q, _runtimeApiPort },
		});
	notifyStateChanged(u"runtime-api"_q, QString(), false, false);
	return true;
}

void Manager::ensureRuntimeApiServerState() {
	if (_runtimeApiEnabled) {
		startRuntimeApiServer();
	} else {
		stopRuntimeApiServer();
	}
}

bool Manager::startRuntimeApiServer() {
	if (_runtimeApiServer && _runtimeApiServer->isListening()) {
		return true;
	}
	if (!_runtimeApiServer) {
		_runtimeApiServer = std::make_unique<QTcpServer>();
		_runtimeApiServer->setParent(this);
		QObject::connect(
			_runtimeApiServer.get(),
			&QTcpServer::newConnection,
			this,
			&Manager::onRuntimeApiNewConnection);
	}
	if (_runtimeApiPort <= 0 || _runtimeApiPort > 65535) {
		_runtimeApiPort = 37080;
	}
	const auto listenAddress = RuntimeApiListenAddress();
	if (!_runtimeApiServer->listen(listenAddress, quint16(_runtimeApiPort))) {
		logEvent(
			u"runtime-api"_q,
			u"listen-failed"_q,
			QJsonObject{
				{ u"address"_q, listenAddress.toString() },
				{ u"port"_q, _runtimeApiPort },
				{ u"error"_q, _runtimeApiServer->errorString() },
			});
		return false;
	}
	logEvent(
		u"runtime-api"_q,
		u"listening"_q,
		QJsonObject{
			{ u"port"_q, _runtimeApiPort },
			{ u"address"_q, listenAddress.toString() },
			{ u"advertisedAddress"_q, RuntimeApiAdvertisedHost() },
		});
	return true;
}

void Manager::stopRuntimeApiServer() {
	for (auto i = _runtimeApiBuffers.begin(); i != _runtimeApiBuffers.end(); ++i) {
		const auto socket = i.key();
		if (socket) {
			socket->disconnectFromHost();
			socket->deleteLater();
		}
	}
	_runtimeApiBuffers.clear();
	if (_runtimeApiServer && _runtimeApiServer->isListening()) {
		_runtimeApiServer->close();
		logEvent(
			u"runtime-api"_q,
			u"stopped"_q,
			QJsonObject{
				{ u"port"_q, _runtimeApiPort },
			});
	}
}

void Manager::onRuntimeApiNewConnection() {
	if (!_runtimeApiServer) {
		return;
	}
	while (_runtimeApiServer->hasPendingConnections()) {
		const auto socket = _runtimeApiServer->nextPendingConnection();
		if (!socket) {
			continue;
		}
		_runtimeApiBuffers.insert(socket, QByteArray());
		QObject::connect(
			socket,
			&QTcpSocket::readyRead,
			this,
			[this, socket] { onRuntimeApiSocketReadyRead(socket); });
		QObject::connect(
			socket,
			&QTcpSocket::disconnected,
			this,
			[this, socket] {
				_runtimeApiBuffers.remove(socket);
				socket->deleteLater();
			});
	}
}

void Manager::onRuntimeApiSocketReadyRead(QTcpSocket *socket) {
	if (!socket) {
		return;
	}
	auto &buffer = _runtimeApiBuffers[socket];
	buffer += socket->readAll();
	QString method;
	QString target;
	QHash<QByteArray, QByteArray> headers;
	QByteArray body;
	qsizetype consumed = 0;
	QString error;
	const auto peer = RuntimeSocketPeerLabel(socket);
	if (!ParseRuntimeHttpRequest(
			buffer,
			method,
			target,
			headers,
			body,
			consumed,
			error)) {
		if (error == u"incomplete"_q) {
			return;
		}
		logEvent(
			u"runtime-api"_q,
			u"http-parse-failed"_q,
			QJsonObject{
				{ u"peer"_q, peer },
				{ u"bufferBytes"_q, QString::number(buffer.size()) },
				{ u"error"_q, error },
			});
		socket->write(RuntimeErrorResponse(400, error));
		socket->disconnectFromHost();
		_runtimeApiBuffers.remove(socket);
		return;
	}
	buffer.remove(0, consumed);
	logEvent(
		u"runtime-api"_q,
		u"http-request"_q,
		QJsonObject{
			{ u"peer"_q, peer },
			{ u"method"_q, method.trimmed().toUpper() },
			{ u"target"_q, target },
			{ u"headerCount"_q, headers.size() },
			{ u"bodyBytes"_q, QString::number(body.size()) },
			{ u"bodyPreview"_q, RuntimeBodyPreview(body) },
		});
	auto disableAfterResponse = false;
	const auto response = processRuntimeApiRequest(
		method,
		target,
		body,
		disableAfterResponse);
	logEvent(
		u"runtime-api"_q,
		u"http-response"_q,
		QJsonObject{
			{ u"peer"_q, peer },
			{ u"method"_q, method.trimmed().toUpper() },
			{ u"target"_q, target },
			{ u"status"_q, RuntimeResponseStatusCode(response) },
			{ u"bytes"_q, QString::number(response.size()) },
			{ u"disableAfterResponse"_q, disableAfterResponse },
		});
	socket->write(response);
	socket->disconnectFromHost();
	_runtimeApiBuffers.remove(socket);
	if (disableAfterResponse) {
		QTimer::singleShot(0, this, [=] {
			setRuntimeApiEnabled(false);
		});
	}
}

QByteArray Manager::processRuntimeApiRequest(
		const QString &method,
		const QString &target,
		const QByteArray &body,
		bool &disableRuntimeApiAfterResponse) {
	disableRuntimeApiAfterResponse = false;
	if (method.isEmpty() || target.isEmpty()) {
		return RuntimeErrorResponse(400, u"invalid request"_q);
	}
	const auto url = QUrl(u"http://runtime.local"_q + target);
	const auto path = url.path();
	const auto query = QUrlQuery(url);
	const auto resolvedMethod = method.trimmed().toUpper();

	const auto session = activeSession();
	if (!session
		&& path != u"/"_q
		&& path != u"/v1"_q
		&& path != u"/v1/health"_q
		&& path != u"/v1/system"_q
		&& path != u"/v1/runtime"_q
		&& path != u"/v1/host"_q
		&& path != u"/v1/diagnostics"_q
		&& path != u"/v1/updates"_q
		&& path != u"/v1/updates/check"_q
		&& path != u"/v1/updates/channel"_q) {
		return RuntimeErrorResponse(503, u"no active telegram session"_q);
	}

	auto parseBodyObject = [&]() -> QJsonObject {
		if (body.trimmed().isEmpty()) {
			return QJsonObject();
		}
		const auto doc = QJsonDocument::fromJson(body);
		return doc.isObject() ? doc.object() : QJsonObject();
	};

	if (resolvedMethod == u"GET"_q && (path == u"/"_q || path == u"/v1"_q)) {
		return RuntimeOkResponse(QJsonObject{
			{ u"name"_q, u"Astrogram Runtime API"_q },
			{ u"version"_q, 1 },
			{ u"baseUrl"_q, hostInfo().runtimeApiBaseUrl },
			{ u"endpoints"_q, QJsonArray{
				u"GET /v1/health"_q,
				u"GET /v1/host"_q,
				u"GET /v1/system"_q,
				u"GET /v1/diagnostics"_q,
				u"GET /v1/updates"_q,
				u"POST /v1/updates/check"_q,
				u"POST /v1/updates/channel"_q,
				u"GET /v1/plugins"_q,
				u"GET /v1/plugins/<id>"_q,
				u"POST /v1/plugins/reload"_q,
				u"POST /v1/plugins/<id>/enable"_q,
				u"POST /v1/plugins/<id>/disable"_q,
				u"DELETE /v1/plugins/<id>"_q,
				u"GET /v1/logs?lines=200"_q,
				u"POST /v1/runtime"_q,
				u"POST /v1/safe-mode"_q,
				u"GET /v1/chats?limit=100"_q,
				u"GET /v1/chats/<peerId>/messages?limit=50"_q,
				u"POST /v1/messages/send"_q,
			} },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/health"_q) {
		return RuntimeOkResponse(QJsonObject{
			{ u"status"_q, u"ok"_q },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
			{ u"runtimeApiHost"_q, RuntimeApiAdvertisedHost() },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/host"_q) {
		const auto host = hostInfo();
		return RuntimeOkResponse(QJsonObject{
			{ u"compiler"_q, host.compiler },
			{ u"platform"_q, host.platform },
			{ u"appVersion"_q, host.appVersion },
			{ u"workingPath"_q, host.workingPath },
			{ u"pluginsPath"_q, host.pluginsPath },
			{ u"appUiLanguage"_q, host.appUiLanguage },
			{ u"safeModeEnabled"_q, host.safeModeEnabled },
			{ u"runtimeApiEnabled"_q, host.runtimeApiEnabled },
			{ u"runtimeApiPort"_q, host.runtimeApiPort },
			{ u"runtimeApiBaseUrl"_q, host.runtimeApiBaseUrl },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/diagnostics"_q) {
		const auto states = plugins();
		auto enabledCount = 0;
		auto loadedCount = 0;
		auto errorCount = 0;
		auto recoveryCount = 0;
		auto verifiedSourceCount = 0;
		auto unverifiedSourceCount = 0;
		for (const auto &state : states) {
			enabledCount += state.enabled ? 1 : 0;
			loadedCount += state.loaded ? 1 : 0;
			errorCount += !state.error.trimmed().isEmpty() ? 1 : 0;
			recoveryCount += (state.recoverySuspected || state.disabledByRecovery) ? 1 : 0;
			verifiedSourceCount += state.sourceVerified ? 1 : 0;
			unverifiedSourceCount += state.sourceVerified ? 0 : 1;
		}
		auto trustedChannelIds = std::vector<int64>();
		auto trustedRecords = std::vector<QString>();
		if (session) {
			const auto &appConfig = session->appConfig();
			trustedChannelIds = appConfig.astrogramTrustedPluginChannelIds();
			trustedRecords = appConfig.astrogramTrustedPluginRecords();
		}
		auto trustedChannelsJson = QJsonArray();
		for (const auto channelId : trustedChannelIds) {
			trustedChannelsJson.push_back(QString::number(channelId));
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"safeModeEnabled"_q, safeModeEnabled() },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
			{ u"hasActiveSession"_q, session != nullptr },
			{ u"visibleTopLevelWindows"_q, VisibleTopLevelWidgetCount() },
			{ u"recoveryPending"_q, _recoveryPending.active },
			{ u"recoveryNotice"_q, _recoveryNotice.active },
			{ u"paths"_q, QJsonObject{
				{ u"workingDir"_q, cWorkingDir() },
				{ u"pluginsDir"_q, _pluginsPath },
				{ u"config"_q, _configPath },
				{ u"clientLog"_q, cWorkingDir() + QString::fromLatin1(kClientLogFile) },
				{ u"pluginsLog"_q, _logPath },
				{ u"trace"_q, _tracePath },
				{ u"safeModeFlag"_q, _safeModePath },
				{ u"recovery"_q, _recoveryPath },
			} },
			{ u"plugins"_q, QJsonObject{
				{ u"count"_q, int(states.size()) },
				{ u"enabled"_q, enabledCount },
				{ u"loaded"_q, loadedCount },
				{ u"errors"_q, errorCount },
				{ u"recoveryDisabled"_q, recoveryCount },
				{ u"verifiedSources"_q, verifiedSourceCount },
				{ u"unverifiedSources"_q, unverifiedSourceCount },
			} },
			{ u"trustedSources"_q, QJsonObject{
				{ u"channels"_q, trustedChannelsJson },
				{ u"recordsCount"_q, int(trustedRecords.size()) },
			} },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/system"_q) {
		const auto system = systemInfo();
		return RuntimeOkResponse(QJsonObject{
			{ u"processId"_q, QString::number(system.processId) },
			{ u"logicalCpuCores"_q, system.logicalCpuCores },
			{ u"physicalCpuCores"_q, system.physicalCpuCores },
			{ u"productType"_q, system.productType },
			{ u"productVersion"_q, system.productVersion },
			{ u"prettyProductName"_q, system.prettyProductName },
			{ u"kernelType"_q, system.kernelType },
			{ u"kernelVersion"_q, system.kernelVersion },
			{ u"architecture"_q, system.architecture },
			{ u"buildAbi"_q, system.buildAbi },
			{ u"hostName"_q, system.hostName },
			{ u"userName"_q, system.userName },
			{ u"locale"_q, system.locale },
			{ u"uiLanguage"_q, system.uiLanguage },
			{ u"timeZone"_q, system.timeZone },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/updates"_q) {
		return RuntimeOkResponse(QJsonObject{
			{ u"updates"_q, RuntimeUpdatesJson() },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/updates/check"_q) {
		if (Core::UpdaterDisabled()) {
			return RuntimeErrorResponse(409, u"updater is disabled"_q);
		}
		if (!cAutoUpdate()) {
			return RuntimeErrorResponse(409, u"auto update is disabled"_q);
		}
		Core::UpdateChecker().test();
		return RuntimeOkResponse(QJsonObject{
			{ u"message"_q, u"update check started"_q },
			{ u"updates"_q, RuntimeUpdatesJson() },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/updates/channel"_q) {
		if (cAlphaVersion() != 0) {
			return RuntimeErrorResponse(409, u"channel override is unavailable on alpha builds"_q);
		}
		const auto object = parseBodyObject();
		const auto channel = object.value(u"channel"_q).toString();
		auto installBeta = false;
		if (!RuntimeResolveInstallBetaChannel(channel, installBeta)) {
			return RuntimeErrorResponse(400, u"body.channel must be stable or beta"_q);
		}
		cSetInstallBetaVersion(installBeta);
		Core::Launcher::Instance().writeInstallBetaVersionsSetting();
		return RuntimeOkResponse(QJsonObject{
			{ u"preferredChannel"_q, installBeta ? u"beta"_q : u"stable"_q },
			{ u"installBetaVersion"_q, installBeta },
			{ u"message"_q, u"update channel preference saved"_q },
			{ u"updates"_q, RuntimeUpdatesJson() },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/plugins"_q) {
		auto list = QJsonArray();
		for (const auto &state : plugins()) {
			auto plugin = RuntimePluginJson(state);
			plugin.insert(
				u"actionsCount"_q,
				int(actionsFor(state.info.id).size()));
			plugin.insert(
				u"settingsPagesCount"_q,
				int(settingsPagesFor(state.info.id).size()));
			list.push_back(plugin);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"plugins"_q, list },
			{ u"count"_q, list.size() },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/plugins/reload"_q) {
		reload();
		return RuntimeOkResponse(QJsonObject{
			{ u"message"_q, u"plugins reloaded"_q },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/logs"_q) {
		auto lines = std::max(1, query.queryItemValue(u"lines"_q).toInt());
		lines = std::min(lines, 5000);
		auto array = QJsonArray();
		for (const auto &line : RuntimeTailLines(_logPath, lines)) {
			array.push_back(line);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"lines"_q, array },
			{ u"count"_q, array.size() },
			{ u"path"_q, _logPath },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/safe-mode"_q) {
		const auto object = parseBodyObject();
		if (!object.contains(u"enabled"_q) || !object.value(u"enabled"_q).isBool()) {
			return RuntimeErrorResponse(400, u"body.enabled bool is required"_q);
		}
		const auto enabled = object.value(u"enabled"_q).toBool();
		if (!setSafeModeEnabled(enabled)) {
			return RuntimeErrorResponse(500, u"failed to change safe mode"_q);
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"safeModeEnabled"_q, enabled },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/runtime"_q) {
		const auto object = parseBodyObject();
		if (!object.contains(u"enabled"_q) || !object.value(u"enabled"_q).isBool()) {
			return RuntimeErrorResponse(400, u"body.enabled bool is required"_q);
		}
		const auto enabled = object.value(u"enabled"_q).toBool();
		if (object.value(u"port"_q).isDouble()) {
			const auto port = object.value(u"port"_q).toInt();
			if (port > 0 && port <= 65535) {
				_runtimeApiPort = port;
			}
		}
		if (enabled) {
			if (!setRuntimeApiEnabled(true)) {
				return RuntimeErrorResponse(500, u"failed to enable runtime api"_q);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"runtimeApiEnabled"_q, true },
				{ u"runtimeApiPort"_q, _runtimeApiPort },
				{ u"runtimeApiBaseUrl"_q, hostInfo().runtimeApiBaseUrl },
			});
		}
		disableRuntimeApiAfterResponse = true;
		return RuntimeOkResponse(QJsonObject{
			{ u"runtimeApiEnabled"_q, false },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
			{ u"message"_q, u"runtime api will stop after this response"_q },
		});
	}
	if (resolvedMethod == u"GET"_q && path == u"/v1/chats"_q) {
		auto limit = query.queryItemValue(u"limit"_q).toInt();
		if (limit <= 0) {
			limit = 100;
		}
		limit = std::min(limit, 1000);
		auto result = QJsonArray();
		auto count = 0;
		const auto &list = session->data().chatsList()->indexed()->all();
		for (const auto row : list) {
			if (!row || !row->history()) {
				continue;
			}
			const auto history = row->history();
			const auto peer = history->peer.get();
			auto chat = QJsonObject{
				{ u"peerId"_q, QString::number(peer->id.value) },
				{ u"type"_q, peerIsUser(peer->id)
					? u"user"_q
					: (peerIsChannel(peer->id) ? u"channel"_q : u"chat"_q) },
				{ u"name"_q, peer->name() },
				{ u"username"_q, peer->username() },
				{ u"muted"_q, history->muted() },
				{ u"unreadCount"_q, history->unreadCountKnown()
					? history->unreadCount()
					: -1 },
			};
			const auto last = history->lastMessage();
			if (last) {
				chat.insert(u"lastMessage"_q, RuntimeMessageJson(last, peer->id));
			}
			result.push_back(chat);
			++count;
			if (count >= limit) {
				break;
			}
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"count"_q, result.size() },
			{ u"chats"_q, result },
		});
	}
	if (resolvedMethod == u"POST"_q && path == u"/v1/messages/send"_q) {
		const auto object = parseBodyObject();
		const auto peerText = object.value(u"peerId"_q).toString().trimmed();
		const auto text = object.value(u"text"_q).toString();
		if (peerText.isEmpty()) {
			return RuntimeErrorResponse(400, u"body.peerId string is required"_q);
		}
		if (text.trimmed().isEmpty()) {
			return RuntimeErrorResponse(400, u"body.text string is required"_q);
		}
		const auto peerId = RuntimePeerIdFromString(peerText);
		if (!peerId) {
			return RuntimeErrorResponse(400, u"invalid peerId"_q);
		}
		auto history = session->data().history(*peerId);
		auto message = Api::MessageToSend(Api::SendAction(history));
		message.textWithTags.text = text;
		session->api().sendMessage(std::move(message));
		return RuntimeOkResponse(QJsonObject{
			{ u"peerId"_q, QString::number(peerId->value) },
			{ u"sent"_q, true },
		});
	}

	const auto prefix = u"/v1/plugins/"_q;
	if (path.startsWith(prefix)) {
		auto rest = path.mid(prefix.size());
		if (resolvedMethod == u"GET"_q
			&& !rest.isEmpty()
			&& !rest.contains(u'/')) {
			const auto id = QUrl::fromPercentEncoding(rest.toUtf8());
			if (id.isEmpty()) {
				return RuntimeErrorResponse(400, u"plugin id is required"_q);
			}
			const auto record = findRecord(id);
			if (!record) {
				return RuntimeErrorResponse(404, u"plugin not found"_q);
			}
			auto commands = QJsonArray();
			for (const auto &command : commandsFor(id)) {
				commands.push_back(commandDescriptorToJson(command));
			}
			auto actions = QJsonArray();
			for (const auto &action : actionsFor(id)) {
				actions.push_back(QJsonObject{
					{ u"id"_q, QString::number(action.id) },
					{ u"title"_q, action.title },
					{ u"description"_q, action.description },
				});
			}
			auto panels = QJsonArray();
			for (const auto &panel : panelsFor(id)) {
				panels.push_back(QJsonObject{
					{ u"id"_q, QString::number(panel.id) },
					{ u"title"_q, panel.title },
					{ u"description"_q, panel.description },
				});
			}
			auto settingsPages = QJsonArray();
			for (const auto &page : settingsPagesFor(id)) {
				settingsPages.push_back(QJsonObject{
					{ u"id"_q, QString::number(page.id) },
					{ u"title"_q, page.title },
					{ u"description"_q, page.description },
					{ u"sectionsCount"_q, page.sections.size() },
				});
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"plugin"_q, RuntimePluginJson(record->state) },
				{ u"commands"_q, commands },
				{ u"actions"_q, actions },
				{ u"panels"_q, panels },
				{ u"settingsPages"_q, settingsPages },
			});
		}
		if (rest.endsWith(u"/enable"_q) || rest.endsWith(u"/disable"_q)) {
			if (resolvedMethod != u"POST"_q) {
				return RuntimeErrorResponse(405, u"method not allowed"_q);
			}
			const auto enable = rest.endsWith(u"/enable"_q);
			rest.chop(enable ? 7 : 8);
			const auto id = QUrl::fromPercentEncoding(rest.toUtf8());
			if (id.isEmpty()) {
				return RuntimeErrorResponse(400, u"plugin id is required"_q);
			}
			if (!setEnabled(id, enable)) {
				return RuntimeErrorResponse(404, u"plugin not found or state unchanged"_q);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"id"_q, id },
				{ u"enabled"_q, enable },
			});
		}
		if (resolvedMethod == u"DELETE"_q) {
			const auto id = QUrl::fromPercentEncoding(rest.toUtf8());
			if (id.isEmpty()) {
				return RuntimeErrorResponse(400, u"plugin id is required"_q);
			}
			QString error;
			if (!removePlugin(id, &error)) {
				return RuntimeErrorResponse(
					409,
					error.isEmpty() ? u"failed to remove plugin"_q : error);
			}
			return RuntimeOkResponse(QJsonObject{
				{ u"id"_q, id },
				{ u"removed"_q, true },
			});
		}
	}
	const auto chatPrefix = u"/v1/chats/"_q;
	if (resolvedMethod == u"GET"_q
		&& path.startsWith(chatPrefix)
		&& path.endsWith(u"/messages"_q)) {
		auto rest = path.mid(chatPrefix.size());
		rest.chop(QString(u"/messages"_q).size());
		const auto peerId = RuntimePeerIdFromString(rest);
		if (!peerId) {
			return RuntimeErrorResponse(400, u"invalid chat peer id"_q);
		}
		auto limit = query.queryItemValue(u"limit"_q).toInt();
		if (limit <= 0) {
			limit = 50;
		}
		limit = std::min(limit, 200);
		const auto history = session->data().historyLoaded(*peerId);
		if (!history) {
			return RuntimeOkResponse(QJsonObject{
				{ u"peerId"_q, QString::number(peerId->value) },
				{ u"loaded"_q, false },
				{ u"messages"_q, QJsonArray() },
				{ u"count"_q, 0 },
			});
		}
		const auto lastMessage = history->lastMessage();
		if (!lastMessage) {
			return RuntimeOkResponse(QJsonObject{
				{ u"peerId"_q, QString::number(peerId->value) },
				{ u"loaded"_q, true },
				{ u"messages"_q, QJsonArray() },
				{ u"count"_q, 0 },
			});
		}
		const auto snapshot = history->messages().snapshot(
			Storage::SparseIdsListQuery(lastMessage->id, limit, 0));
		auto ids = std::vector<MsgId>();
		ids.reserve(snapshot.messageIds.size());
		for (const auto id : snapshot.messageIds) {
			ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end(), std::greater<MsgId>());
		if (int(ids.size()) > limit) {
			ids.resize(limit);
		}
		auto array = QJsonArray();
		for (const auto msgId : ids) {
			const auto item = session->data().message(*peerId, msgId);
			if (!item) {
				continue;
			}
			array.push_back(RuntimeMessageJson(item, *peerId));
		}
		return RuntimeOkResponse(QJsonObject{
			{ u"peerId"_q, QString::number(peerId->value) },
			{ u"loaded"_q, true },
			{ u"messages"_q, array },
			{ u"count"_q, array.size() },
		});
	}

	return RuntimeErrorResponse(404, u"endpoint not found"_q);
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
	const auto loadability = CheckPackageLoadability(path);
	if (loadability.previewExportValid) {
		result.previewAvailable = true;
		MergePluginInfo(result.info, loadability.previewInfo);
		if (result.icon.isEmpty()) {
			result.icon = loadability.previewIcon;
		}
	}
	result.compatible = loadability.compatible;
	result.error = loadability.error;
	{
		auto trustProbe = PluginState();
		trustProbe.path = path;
		syncSourceTrustState(trustProbe);
		result.sha256 = trustProbe.sha256;
		result.sourceVerified = trustProbe.sourceVerified;
		result.sourceTrustText = trustProbe.sourceTrustText;
		result.sourceTrustDetails = trustProbe.sourceTrustDetails;
		result.sourceTrustReason = trustProbe.sourceTrustReason;
		result.sourceChannelId = trustProbe.sourceChannelId;
		result.sourceMessageId = trustProbe.sourceMessageId;
	}

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
			{ u"sourceVerified"_q, result.sourceVerified },
			{ u"sourceTrustReason"_q, result.sourceTrustReason },
			{ u"sourceChannelId"_q, QString::number(result.sourceChannelId) },
			{ u"sourceMessageId"_q, QString::number(result.sourceMessageId) },
			{ u"installedVersion"_q, result.installedVersion },
			{ u"installedPath"_q, result.installedPath },
			{ u"icon"_q, result.icon },
			{ u"plugin"_q, pluginInfoToJson(result.info) },
			{ u"error"_q, result.error },
			{ u"loadability"_q, QJsonObject{
				{ u"libraryLoaded"_q, loadability.libraryLoaded },
				{ u"previewExportFound"_q, loadability.previewExportFound },
				{ u"previewExportValid"_q, loadability.previewExportValid },
				{ u"binaryInfoFound"_q, loadability.binaryInfoFound },
				{ u"entryFound"_q, loadability.entryFound },
				{ u"binarySummary"_q, loadability.binarySummary },
				{ u"mismatchField"_q, loadability.mismatchField },
				{ u"error"_q, loadability.error },
			}},
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
	const auto rollbackBackupPath = targetPath + u".rollback"_q;
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
	QFile::remove(rollbackBackupPath);

	auto rollbackRestorePath = QString();
	auto rollbackBackupCreated = false;
	const auto restoreRollbackBackup = [&](const QString &reason) {
		QFile::remove(tempPath);
		if (QFileInfo::exists(targetPath)
			&& QFileInfo(targetPath).absoluteFilePath() != sourceInfo.absoluteFilePath()) {
			QFile::remove(targetPath);
		}
		if (!rollbackBackupCreated) {
			logEvent(
				u"package"_q,
				u"rollback-skipped"_q,
				QJsonObject{
					{ u"sourcePath"_q, sourcePath },
					{ u"targetPath"_q, targetPath },
					{ u"reason"_q, reason },
				});
			return;
		}
		if (QFileInfo::exists(rollbackRestorePath)) {
			QFile::remove(rollbackRestorePath);
		}
		if (!QFile::rename(rollbackBackupPath, rollbackRestorePath)) {
			logEvent(
				u"package"_q,
				u"rollback-restore-failed"_q,
				QJsonObject{
					{ u"backupPath"_q, rollbackBackupPath },
					{ u"restorePath"_q, rollbackRestorePath },
					{ u"reason"_q, reason },
				});
			return;
		}
		logEvent(
			u"package"_q,
			u"rollback-restored"_q,
			QJsonObject{
				{ u"backupPath"_q, rollbackBackupPath },
				{ u"restorePath"_q, rollbackRestorePath },
				{ u"reason"_q, reason },
			});
	};
	const auto rollbackInstalledPackage = [&](const QString &reason) {
		logEvent(
			u"package"_q,
			u"rollback-start"_q,
			QJsonObject{
				{ u"sourcePath"_q, sourcePath },
				{ u"targetPath"_q, targetPath },
				{ u"backupPath"_q, rollbackBackupPath },
				{ u"restorePath"_q, rollbackRestorePath },
				{ u"reason"_q, reason },
			});
		unloadAll();
		restoreRollbackBackup(reason);
		loadConfig();
		loadRecoveryState();
		rescanNow();
	};

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
		const auto existingPathToProtect = (!preview.installedPath.isEmpty()
			&& QFileInfo::exists(preview.installedPath))
			? QFileInfo(preview.installedPath).absoluteFilePath()
			: (QFileInfo::exists(targetPath)
				? QFileInfo(targetPath).absoluteFilePath()
				: QString());
		if (!existingPathToProtect.isEmpty()) {
			rollbackRestorePath = existingPathToProtect;
			if (!QFile::rename(existingPathToProtect, rollbackBackupPath)) {
				logEvent(
					u"package"_q,
					u"backup-existing-failed"_q,
					QJsonObject{
						{ u"existingPath"_q, existingPathToProtect },
						{ u"backupPath"_q, rollbackBackupPath },
					});
				QFile::remove(tempPath);
				rescanNow();
				if (error) {
					*error = u"Could not prepare rollback for the previous plugin file."_q;
				}
				finishRecoveryOperation();
				return false;
			}
			rollbackBackupCreated = true;
		}
		if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath)) {
			logEvent(
				u"package"_q,
				u"overwrite-remove-failed"_q,
				QJsonObject{
					{ u"targetPath"_q, targetPath },
				});
			restoreRollbackBackup(u"overwrite-remove-failed"_q);
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
			restoreRollbackBackup(u"rename-failed"_q);
			rescanNow();
			if (error) {
				*error = u"Could not finalize the installed plugin file."_q;
			}
			finishRecoveryOperation();
			return false;
		}
	}

	if (!preview.info.id.isEmpty()) {
		_disabled.remove(preview.info.id);
		clearRecoveryDisabled(preview.info.id);
		saveConfig();
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
				rollbackInstalledPackage(record->state.error.trimmed());
				if (error) {
					*error = record->state.error.trimmed();
				}
				finishRecoveryOperation();
				return false;
			}
		}
	}
	finishRecoveryOperation();
	QFile::remove(rollbackBackupPath);
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

bool Manager::removePlugin(const QString &pluginId, QString *error) {
	Logs::writeClient(QString::fromLatin1("[plugins] remove requested: %1").arg(pluginId));
	const auto normalizedId = pluginId.trimmed();
	const auto *record = findRecord(normalizedId);
	if (!record) {
		if (error) {
			*error = u"Plugin was not found."_q;
		}
		logEvent(
			u"package"_q,
			u"remove-missing"_q,
			QJsonObject{
				{ u"pluginId"_q, normalizedId },
			});
		return false;
	}

	const auto pluginInfo = record->state.info;
	auto packagePaths = QStringList();
	auto seenPaths = QSet<QString>();
	const auto rememberPackagePath = [&](QString path) {
		path = path.trimmed();
		if (path.isEmpty()) {
			return;
		}
		path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN)
		const auto key = path.toLower();
#else // Q_OS_WIN
		const auto key = path;
#endif // Q_OS_WIN
		if (seenPaths.contains(key)) {
			return;
		}
		seenPaths.insert(key);
		packagePaths.push_back(path);
	};
	for (const auto &plugin : _plugins) {
		if (plugin.state.info.id.trimmed() == normalizedId) {
			rememberPackagePath(plugin.state.path);
		}
	}
	rememberPackagePath(record->state.path);
	const auto pluginPath = packagePaths.isEmpty()
		? record->state.path.trimmed()
		: packagePaths.front();
	const auto rescanNow = [this] {
		if (safeModeEnabled()) {
			logEvent(u"safe-mode"_q, u"remove-rescan-metadata-only"_q);
			scanPlugins(true);
		} else {
			scanPlugins();
		}
	};

	startRecoveryOperation(
		u"remove"_q,
		{ normalizedId },
		pluginInfo.name.isEmpty() ? pluginId : pluginInfo.name);
	logEvent(
		u"package"_q,
		u"remove-start"_q,
		QJsonObject{
			{ u"pluginId"_q, normalizedId },
			{ u"path"_q, pluginPath },
			{ u"paths"_q, QJsonArray::fromStringList(packagePaths) },
			{ u"pathCount"_q, packagePaths.size() },
			{ u"plugin"_q, pluginInfoToJson(pluginInfo) },
		});

	unloadAll();
	loadConfig();
	loadRecoveryState();

	if (packagePaths.isEmpty()) {
		logEvent(
			u"package"_q,
			u"remove-missing-file"_q,
			QJsonObject{
				{ u"pluginId"_q, normalizedId },
				{ u"path"_q, pluginPath },
				{ u"paths"_q, QJsonArray::fromStringList(packagePaths) },
			});
		rescanNow();
		if (error) {
			*error = u"Plugin package path was not found."_q;
		}
		finishRecoveryOperation();
		return false;
	}

	auto removedPaths = QStringList();
	auto missingPaths = QStringList();
	auto failedPaths = QStringList();
	auto firstFailureReason = QString();
	for (const auto &path : packagePaths) {
		if (!QFileInfo::exists(path)) {
			missingPaths.push_back(path);
			continue;
		}
		auto removed = false;
		auto removeError = QString();
		for (auto attempt = 0; attempt != 16; ++attempt) {
			QFile file(path);
			if (file.remove() || !QFileInfo::exists(path)) {
				removed = true;
				break;
			}
			removeError = file.errorString();
			FlushPluginUnload();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
			QThread::msleep(20);
		}
		if (removed) {
			removedPaths.push_back(path);
		} else {
			failedPaths.push_back(path);
			if (firstFailureReason.isEmpty()) {
				firstFailureReason = removeError;
			}
		}
	}

	if (!failedPaths.isEmpty()) {
		Logs::writeClient(QString::fromLatin1(
			"[plugins] remove failed: %1 failed=%2 reason=%3")
			.arg(normalizedId)
			.arg(failedPaths.front())
			.arg(firstFailureReason));
		logEvent(
			u"package"_q,
			u"remove-file-failed"_q,
			QJsonObject{
				{ u"pluginId"_q, normalizedId },
				{ u"path"_q, pluginPath },
				{ u"paths"_q, QJsonArray::fromStringList(packagePaths) },
				{ u"removedPaths"_q, QJsonArray::fromStringList(removedPaths) },
				{ u"missingPaths"_q, QJsonArray::fromStringList(missingPaths) },
				{ u"failedPaths"_q, QJsonArray::fromStringList(failedPaths) },
				{ u"reason"_q, firstFailureReason },
			});
		rescanNow();
		if (error) {
			*error = !firstFailureReason.isEmpty()
				? firstFailureReason
				: (failedPaths.size() > 1)
				? u"Could not delete all plugin package files."_q
				: u"Could not delete the plugin package file."_q;
		}
		finishRecoveryOperation();
		return false;
	}

	_disabled.remove(normalizedId);
	_disabledByRecovery.remove(normalizedId);
	_storedSettings.remove(normalizedId);
	saveConfig();
	saveRecoveryState();
	rescanNow();
	Logs::writeClient(QString::fromLatin1(
		"[plugins] remove finished: %1 removed=%2 missing=%3")
		.arg(normalizedId)
		.arg(removedPaths.size())
		.arg(missingPaths.size()));
	finishRecoveryOperation();

	if (error) {
		error->clear();
	}
	logEvent(
		u"package"_q,
		u"remove-finished"_q,
		QJsonObject{
			{ u"pluginId"_q, normalizedId },
			{ u"path"_q, pluginPath },
			{ u"paths"_q, QJsonArray::fromStringList(packagePaths) },
			{ u"removedPaths"_q, QJsonArray::fromStringList(removedPaths) },
			{ u"missingPaths"_q, QJsonArray::fromStringList(missingPaths) },
			{ u"plugin"_q, pluginInfoToJson(pluginInfo) },
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

std::vector<SettingsPageState> Manager::settingsPagesFor(
		const QString &pluginId) const {
	auto result = std::vector<SettingsPageState>();
	const auto it = _settingsPagesByPlugin.find(pluginId);
	if (it == _settingsPagesByPlugin.end()) {
		return result;
	}
	for (const auto id : it.value()) {
		const auto entry = _settingsPages.find(id);
		if (entry != _settingsPages.end()) {
			result.push_back({
				.id = entry->id,
				.title = entry->descriptor.title,
				.description = entry->descriptor.description,
				.sections = entry->descriptor.sections,
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
	const auto previousPluginId = _registeringPluginId;
	try {
		if (it->handlerWithContext) {
			startRecoveryOperation(u"action"_q, { it->pluginId }, it->title);
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
			InvokePluginCallbackOrThrow([&] {
				it->handlerWithContext(context);
			});
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
			notifyStateChanged(u"action"_q, it->pluginId, false, false);
			return true;
		}
		if (it->handler) {
			startRecoveryOperation(u"action"_q, { it->pluginId }, it->title);
			_registeringPluginId = it->pluginId;
			logEvent(
				u"action"_q,
				u"invoke"_q,
				QJsonObject{
					{ u"id"_q, QString::number(id) },
					{ u"pluginId"_q, it->pluginId },
					{ u"title"_q, it->title },
				});
			InvokePluginCallbackOrThrow([&] {
				it->handler();
			});
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
			notifyStateChanged(u"action"_q, it->pluginId, false, false);
			return true;
		}
	} catch (...) {
		_registeringPluginId = previousPluginId;
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
	const auto previousPluginId = _registeringPluginId;
	try {
		startRecoveryOperation(u"panel"_q, { it->pluginId }, it->descriptor.title);
		_registeringPluginId = it->pluginId;
		logEvent(
			u"panel"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"descriptor"_q, panelDescriptorToJson(it->descriptor) },
			});
		InvokePluginCallbackOrThrow([&] {
			it->handler(window);
		});
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
		notifyStateChanged(u"panel"_q, it->pluginId, false, false);
		return true;
	} catch (...) {
		_registeringPluginId = previousPluginId;
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

bool Manager::updateSetting(SettingsPageId id, SettingDescriptor setting) {
	const auto it = _settingsPages.find(id);
	if (it == _settingsPages.end()) {
		logEvent(
			u"settings"_q,
			u"missing-page"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
			});
		return false;
	}
	if (!it->handler) {
		logEvent(
			u"settings"_q,
			u"missing-handler"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
			});
		return false;
	}
	setting = NormalizeSettingDescriptor(std::move(setting));
	auto *target = (SettingDescriptor*)nullptr;
	for (auto &section : it->descriptor.sections) {
		for (auto &candidate : section.settings) {
			if (candidate.id == setting.id) {
				target = &candidate;
				break;
			}
		}
		if (target) {
			break;
		}
	}
	if (!target) {
		logEvent(
			u"settings"_q,
			u"missing-setting"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"settingId"_q, setting.id },
			});
		return false;
	}
	const auto previous = *target;
	switch (target->type) {
	case SettingControl::Toggle:
		target->boolValue = setting.boolValue;
		break;
	case SettingControl::IntSlider:
		target->intValue = std::clamp(
			setting.intValue,
			target->intMinimum,
			target->intMaximum);
		break;
	case SettingControl::TextInput:
		target->textValue = setting.textValue.trimmed();
		break;
	case SettingControl::ActionButton:
	case SettingControl::InfoText:
		break;
	}
	const auto snapshot = *target;
	rememberSettingValue(it->pluginId, snapshot);
	saveConfig();
	const auto previousPluginId = _registeringPluginId;
	try {
		startRecoveryOperation(
			u"settings"_q,
			{ it->pluginId },
			snapshot.title.isEmpty() ? snapshot.id : snapshot.title);
		_registeringPluginId = it->pluginId;
		logEvent(
			u"settings"_q,
			u"invoke"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"setting"_q, settingDescriptorToJson(snapshot) },
			});
		InvokePluginCallbackOrThrow([&] {
			it->handler(snapshot);
		});
		_registeringPluginId = previousPluginId;
		logEvent(
			u"settings"_q,
			u"success"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"settingId"_q, snapshot.id },
			});
		finishRecoveryOperation();
		notifyStateChanged(u"settings"_q, it->pluginId, false, false);
		return true;
	} catch (...) {
		*target = previous;
		rememberSettingValue(it->pluginId, previous);
		saveConfig();
		_registeringPluginId = previousPluginId;
		logEvent(
			u"settings"_q,
			u"failed"_q,
			QJsonObject{
				{ u"id"_q, QString::number(id) },
				{ u"pluginId"_q, it->pluginId },
				{ u"setting"_q, settingDescriptorToJson(snapshot) },
				{ u"reason"_q, CurrentExceptionText() },
			});
		disablePlugin(
			it->pluginId,
			u"Settings update failed: "_q + CurrentExceptionText());
		showToast(PluginUiText(
			u"Plugin settings failed and plugin was disabled."_q,
			u"Настройки плагина завершились с ошибкой, плагин был выключен."_q));
		finishRecoveryOperation();
		return false;
	}
}

bool Manager::setEnabled(const QString &pluginId, bool enabled) {
	Logs::writeClient(QString::fromLatin1("[plugins] toggle requested: %1 -> %2")
		.arg(pluginId)
		.arg(enabled ? u"enabled"_q : u"disabled"_q));
	if (!findRecord(pluginId)) {
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
	Logs::writeClient(QString::fromLatin1("[plugins] toggle applied: %1 -> %2")
		.arg(pluginId)
		.arg(enabled ? u"enabled"_q : u"disabled"_q));
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
				auto handled = CommandResult();
				InvokePluginCallbackOrThrow([&] {
					handled = entry.handler(context);
				});
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
			auto handled = CommandResult();
			InvokePluginCallbackOrThrow([&] {
				handled = entryIt->handler(context);
			});
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
			{ u"hasWidget"_q, (window && window->widget()) },
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
			InvokePluginCallbackOrThrow([&] {
				entry.handler(window);
			});
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
	if (!window) {
		return;
	}
	const auto widget = window->widget();
	const auto widgetHandlers = _windowWidgetHandlers;
	for (const auto &entry : widgetHandlers) {
		if (!entry.handler) {
			continue;
		}
		try {
			startRecoveryOperation(u"window"_q, { entry.pluginId });
			const auto previousPluginId = _registeringPluginId;
			_registeringPluginId = entry.pluginId;
			logEvent(
				u"window"_q,
				u"invoke-widget-callback"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"hasWidget"_q, true },
				});
			InvokePluginCallbackOrThrow([&] {
				entry.handler(widget);
			});
			_registeringPluginId = previousPluginId;
			logEvent(
				u"window"_q,
				u"widget-callback-success"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
				});
			finishRecoveryOperation();
		} catch (...) {
			_registeringPluginId.clear();
			logEvent(
				u"window"_q,
				u"widget-callback-failed"_q,
				QJsonObject{
					{ u"pluginId"_q, entry.pluginId },
					{ u"reason"_q, CurrentExceptionText() },
				});
			if (!entry.pluginId.isEmpty()) {
				disablePlugin(
					entry.pluginId,
					u"Window widget callback failed: "_q + CurrentExceptionText());
			}
			showToast(PluginUiText(
				u"Plugin window widget callback failed."_q,
				u"Window widget callback плагина завершился с ошибкой."_q));
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

SettingsPageId Manager::registerSettingsPage(
		const QString &pluginId,
		SettingsPageDescriptor descriptor,
		SettingsChangedHandler handler) {
	if (!hasPlugin(pluginId) || !handler || !HasAnySettings(descriptor)) {
		logEvent(
			u"registry"_q,
			u"register-settings-page-rejected"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"descriptor"_q, settingsPageDescriptorToJson(descriptor) },
				{ u"hasHandler"_q, handler != nullptr },
			});
		return 0;
	}
	for (auto &section : descriptor.sections) {
		section.id = section.id.trimmed();
		section.title = section.title.trimmed();
		section.description = section.description.trimmed();
		for (auto &setting : section.settings) {
			setting = NormalizeSettingDescriptor(std::move(setting));
		}
	}
	applyStoredSettings(pluginId, descriptor);
	const auto id = _nextSettingsPageId++;
	_settingsPages.insert(id, {
		.id = id,
		.pluginId = pluginId,
		.descriptor = std::move(descriptor),
		.handler = std::move(handler),
	});
	_settingsPagesByPlugin[pluginId].push_back(id);
	if (auto record = findRecord(pluginId)) {
		record->settingsPageIds.push_back(id);
	}
	logEvent(
		u"registry"_q,
		u"register-settings-page"_q,
		QJsonObject{
			{ u"id"_q, QString::number(id) },
			{ u"pluginId"_q, pluginId },
			{ u"descriptor"_q, settingsPageDescriptorToJson(_settingsPages.value(id).descriptor) },
		});
	return id;
}

void Manager::unregisterSettingsPage(SettingsPageId id) {
	const auto it = _settingsPages.find(id);
	if (it == _settingsPages.end()) {
		return;
	}
	const auto pluginId = it->pluginId;
	_settingsPages.remove(id);
	const auto listIt = _settingsPagesByPlugin.find(pluginId);
	if (listIt != _settingsPagesByPlugin.end()) {
		listIt->removeAll(id);
		if (listIt->isEmpty()) {
			_settingsPagesByPlugin.erase(listIt);
		}
	}
	if (auto record = findRecord(pluginId)) {
		record->settingsPageIds.removeAll(id);
	}
	logEvent(
		u"registry"_q,
		u"unregister-settings-page"_q,
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
		const auto replay = _windowHandlers.back().handler;
		forEachWindow([&](Window::Controller *window) {
			InvokePluginCallbackOrThrow([&] {
				replay(window);
			});
		});
	}
}

void Manager::forEachWindowWidget(
		std::function<void(QWidget*)> visitor) {
	if (!visitor) {
		return;
	}
	Core::App().forEachWindow([&](not_null<Window::Controller*> window) {
		if (const auto widget = window->widget()) {
			visitor(widget);
		}
	});
}

void Manager::onWindowWidgetCreated(
		std::function<void(QWidget*)> handler) {
	if (handler) {
		_windowWidgetHandlers.push_back({
			.pluginId = _registeringPluginId,
			.handler = std::move(handler),
		});
		logEvent(
			u"registry"_q,
			u"register-window-widget-handler"_q,
			QJsonObject{
				{ u"pluginId"_q, _registeringPluginId },
			});
		const auto replay = _windowWidgetHandlers.back().handler;
		forEachWindowWidget([&](QWidget *widget) {
			InvokePluginCallbackOrThrow([&] {
				replay(widget);
			});
		});
	}
}

Window::Controller *Manager::activeWindow() const {
	if (const auto window = Core::App().activeWindow()) {
		return window;
	}
	return Core::App().activePrimaryWindow();
}

QWidget *Manager::activeWindowWidget() const {
	if (const auto window = activeWindow()) {
		return window->widget();
	}
	return nullptr;
}

bool Manager::settingBoolValue(
		const QString &pluginId,
		const QString &settingId,
		bool fallback) const {
	const auto value = storedSettingValue(pluginId, settingId);
	return value.isBool() ? value.toBool(fallback) : fallback;
}

int Manager::settingIntValue(
		const QString &pluginId,
		const QString &settingId,
		int fallback) const {
	const auto value = storedSettingValue(pluginId, settingId);
	return value.isDouble() ? value.toInt(fallback) : fallback;
}

QString Manager::settingStringValue(
		const QString &pluginId,
		const QString &settingId,
		const QString &fallback) const {
	const auto value = storedSettingValue(pluginId, settingId);
	return value.isString() ? value.toString(fallback) : fallback;
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
		const auto replay = _sessionHandlers.back().handler;
		forEachSession([&](Main::Session *session) {
			InvokePluginCallbackOrThrow([&] {
				replay(session);
			});
		});
	}
}

HostInfo Manager::hostInfo() const {
	auto info = HostInfo();
	info.compiler = QString::fromLatin1(kCompilerId);
	info.platform = QString::fromLatin1(kPlatformId);
	info.appVersion = Core::FormatVersionWithBuild(AppVersion);
	info.workingPath = cWorkingDir();
	info.pluginsPath = _pluginsPath;
	info.appUiLanguage = Lang::LanguageIdOrDefault(Lang::Id());
	info.safeModeEnabled = safeModeEnabled();
	info.runtimeApiEnabled = _runtimeApiEnabled;
	info.runtimeApiPort = _runtimeApiEnabled ? _runtimeApiPort : 0;
	info.runtimeApiBaseUrl = _runtimeApiEnabled
		? (u"http://%1:%2"_q.arg(RuntimeApiAdvertisedHost()).arg(_runtimeApiPort))
		: QString();
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
	_storedSettings.clear();
	_runtimeApiEnabled = false;
	_runtimeApiPort = 37080;
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
	const auto object = document.object();
	const auto array = object.value(u"disabled"_q).toArray();
	for (const auto &value : array) {
		if (value.isString()) {
			_disabled.insert(value.toString());
		}
	}
	const auto settings = object.value(u"settings"_q).toObject();
	for (const auto &pluginId : SortedJsonKeys(settings)) {
		const auto value = settings.value(pluginId);
		if (value.isObject()) {
			_storedSettings.insert(pluginId, value.toObject());
		}
	}
	if (const auto runtimeValue = object.value(u"runtimeApi"_q); runtimeValue.isObject()) {
		const auto runtimeObject = runtimeValue.toObject();
		_runtimeApiEnabled = runtimeObject.value(u"enabled"_q).toBool(false);
		const auto loadedPort = runtimeObject.value(u"port"_q).toInt(_runtimeApiPort);
		_runtimeApiPort = std::clamp(loadedPort, 1, 65535);
	}
	logEvent(
		u"config"_q,
		u"loaded"_q,
		QJsonObject{
			{ u"path"_q, _configPath },
			{ u"disabled"_q, JsonArrayFromStrings(_disabled.values()) },
			{ u"settingsPlugins"_q, _storedSettings.size() },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
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
	auto settings = QJsonObject();
	auto settingPluginIds = _storedSettings.keys();
	settingPluginIds.sort(Qt::CaseInsensitive);
	for (const auto &pluginId : settingPluginIds) {
		const auto values = _storedSettings.value(pluginId);
		if (!values.isEmpty()) {
			settings.insert(pluginId, values);
		}
	}
	const auto object = QJsonObject{
		{ u"disabled"_q, array },
		{ u"settings"_q, settings },
		{ u"runtimeApi"_q, QJsonObject{
			{ u"enabled"_q, _runtimeApiEnabled },
			{ u"port"_q, _runtimeApiPort },
		} },
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
			{ u"settingsPlugins"_q, settings.size() },
			{ u"runtimeApiEnabled"_q, _runtimeApiEnabled },
			{ u"runtimeApiPort"_q, _runtimeApiPort },
		});
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
	const auto text = formatLogEventText(payload);
	appendLogLine(text);
	appendTraceLine(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
	if (ShouldMirrorPluginEventToClient(
			payload.value(u"phase"_q).toString(),
			payload.value(u"event"_q).toString())) {
		Logs::writeClient(u"[plugins] "_q + CompactClientLogText(text, 2000));
	}
}

void Manager::notifyStateChanged(
		QString reason,
		QString pluginId,
		bool structural,
		bool failed) {
	auto enabledCount = 0;
	auto loadedCount = 0;
	auto errorCount = 0;
	for (const auto &plugin : _plugins) {
		enabledCount += plugin.state.enabled ? 1 : 0;
		loadedCount += plugin.state.loaded ? 1 : 0;
		errorCount += plugin.state.error.trimmed().isEmpty() ? 0 : 1;
	}
	auto change = ManagerStateChange();
	change.sequence = _nextStateChangeSequence++;
	change.reason = std::move(reason);
	change.pluginId = pluginId.trimmed();
	change.structural = structural;
	change.failed = failed;
	logEvent(
		u"ui"_q,
		u"state-change"_q,
		QJsonObject{
			{ u"sequence"_q, QString::number(change.sequence) },
			{ u"reason"_q, change.reason },
			{ u"pluginId"_q, change.pluginId },
			{ u"structural"_q, structural },
			{ u"failed"_q, failed },
			{ u"plugins"_q, int(_plugins.size()) },
			{ u"enabled"_q, enabledCount },
			{ u"loaded"_q, loadedCount },
			{ u"errors"_q, errorCount },
		});
	_stateChanges.fire_copy(change);
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
	result.insert(u"sha256"_q, state.sha256);
	result.insert(u"enabled"_q, state.enabled);
	result.insert(u"loaded"_q, state.loaded);
	result.insert(u"error"_q, state.error);
	result.insert(u"disabledByRecovery"_q, state.disabledByRecovery);
	result.insert(u"recoverySuspected"_q, state.recoverySuspected);
	result.insert(u"recoveryReason"_q, state.recoveryReason);
	result.insert(u"sourceVerified"_q, state.sourceVerified);
	result.insert(u"sourceBadgeKind"_q, RuntimeSourceBadgeKind(state));
	result.insert(u"sourceTrustText"_q, state.sourceTrustText);
	result.insert(u"sourceTrustDetails"_q, state.sourceTrustDetails);
	result.insert(u"sourceTrustReason"_q, state.sourceTrustReason);
	result.insert(u"sourceChannelId"_q, QString::number(state.sourceChannelId));
	result.insert(u"sourceMessageId"_q, QString::number(state.sourceMessageId));
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

QJsonObject Manager::settingDescriptorToJson(
		const SettingDescriptor &descriptor) const {
	return QJsonObject{
		{ u"id"_q, descriptor.id },
		{ u"title"_q, descriptor.title },
		{ u"description"_q, descriptor.description },
		{ u"type"_q, SettingControlName(descriptor.type) },
		{ u"boolValue"_q, descriptor.boolValue },
		{ u"intValue"_q, descriptor.intValue },
		{ u"intMinimum"_q, descriptor.intMinimum },
		{ u"intMaximum"_q, descriptor.intMaximum },
		{ u"intStep"_q, descriptor.intStep },
		{ u"valueSuffix"_q, descriptor.valueSuffix },
		{ u"textLength"_q, descriptor.textValue.size() },
		{ u"hasTextValue"_q, !descriptor.textValue.isEmpty() },
		{ u"placeholderText"_q, descriptor.placeholderText },
		{ u"secret"_q, descriptor.secret },
		{ u"buttonText"_q, descriptor.buttonText },
	};
}

QJsonObject Manager::settingsPageDescriptorToJson(
		const SettingsPageDescriptor &descriptor) const {
	auto sections = QJsonArray();
	for (const auto &section : descriptor.sections) {
		auto settings = QJsonArray();
		for (const auto &setting : section.settings) {
			settings.push_back(settingDescriptorToJson(setting));
		}
		sections.push_back(QJsonObject{
			{ u"id"_q, section.id },
			{ u"title"_q, section.title },
			{ u"description"_q, section.description },
			{ u"settings"_q, settings },
		});
	}
	return QJsonObject{
		{ u"id"_q, descriptor.id },
		{ u"title"_q, descriptor.title },
		{ u"description"_q, descriptor.description },
		{ u"sections"_q, sections },
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
	auto windowWidgetHandlers = 0;
	for (const auto &entry : _windowWidgetHandlers) {
		if (entry.pluginId == record.state.info.id) {
			++windowWidgetHandlers;
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
		{ u"settingsPages"_q, record.settingsPageIds.size() },
		{ u"outgoingInterceptors"_q, record.outgoingInterceptorIds.size() },
		{ u"messageObservers"_q, record.messageObserverIds.size() },
		{ u"windowHandlers"_q, windowHandlers },
		{ u"windowWidgetHandlers"_q, windowWidgetHandlers },
		{ u"sessionHandlers"_q, sessionHandlers },
	};
}

QString Manager::fileSha256(const QString &path) const {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		hash.addData(file.read(256 * 1024));
	}
	return QString::fromLatin1(hash.result().toHex());
}

void Manager::syncSourceTrustState(PluginState &state) const {
	const auto normalizeHash = [](QString value) {
		value = value.trimmed().toLower();
		if (value.startsWith(u"sha256:"_q)) {
			value = value.mid(7);
		}
		if (value.size() != 64) {
			return QString();
		}
		for (const auto ch : value) {
			const auto hex = ch.unicode();
			const auto digit = (hex >= '0' && hex <= '9');
			const auto lower = (hex >= 'a' && hex <= 'f');
			if (!digit && !lower) {
				return QString();
			}
		}
		return value;
	};
	struct ParsedRecord {
		QString sha256;
		int64 channelId = 0;
		int64 messageId = 0;
		QString label;
	};
	const auto parseRecordObject = [&](const QJsonObject &object) -> ParsedRecord {
		auto result = ParsedRecord();
		for (const auto &key : { u"sha256"_q, u"hash"_q, u"digest"_q }) {
			const auto value = object.value(key).toString().trimmed();
			if (!value.isEmpty()) {
				result.sha256 = normalizeHash(value);
				if (!result.sha256.isEmpty()) {
					break;
				}
			}
		}
		for (const auto &key : {
			u"channel_id"_q,
			u"channelId"_q,
			u"source_channel_id"_q,
			u"sourceChannelId"_q,
		}) {
			const auto value = object.value(key).toVariant().toLongLong();
			if (value != 0) {
				result.channelId = value;
				break;
			}
		}
		for (const auto &key : {
			u"message_id"_q,
			u"messageId"_q,
			u"post_id"_q,
			u"postId"_q,
			u"source_message_id"_q,
			u"sourceMessageId"_q,
		}) {
			const auto value = object.value(key).toVariant().toLongLong();
			if (value != 0) {
				result.messageId = value;
				break;
			}
		}
		for (const auto &key : { u"label"_q, u"title"_q, u"name"_q }) {
			const auto value = object.value(key).toString().trimmed();
			if (!value.isEmpty()) {
				result.label = value;
				break;
			}
		}
		return result;
	};
	const auto parseRecord = [&](QString raw) -> ParsedRecord {
		raw = raw.trimmed();
		if (raw.isEmpty()) {
			return {};
		}
		if (raw.startsWith(u'{')) {
			const auto document = QJsonDocument::fromJson(raw.toUtf8());
			if (document.isObject()) {
				return parseRecordObject(document.object());
			}
		}
		auto delimiter = u'|';
		auto parts = raw.split(delimiter, Qt::KeepEmptyParts);
		if (parts.size() < 3) {
			delimiter = u';';
			parts = raw.split(delimiter, Qt::KeepEmptyParts);
		}
		if (parts.size() < 3) {
			return {};
		}
		auto result = ParsedRecord();
		result.sha256 = normalizeHash(parts[0]);
		result.channelId = parts[1].trimmed().toLongLong();
		result.messageId = parts[2].trimmed().toLongLong();
		if (parts.size() > 3) {
			result.label = parts.mid(3).join(delimiter).trimmed();
		}
		return result;
	};

	state.sha256 = normalizeHash(fileSha256(state.path));
	state.sourceVerified = false;
	state.sourceTrustText = u"unverified"_q;
	state.sourceTrustDetails.clear();
	state.sourceTrustReason.clear();
	state.sourceChannelId = 0;
	state.sourceMessageId = 0;

	if (state.sha256.isEmpty()) {
		state.sourceTrustReason = u"sha256-unavailable"_q;
		state.sourceTrustDetails = u"sha256-unavailable"_q;
		return;
	}
	const auto session = activeSession();
	if (!session) {
		state.sourceTrustReason = u"no-active-session"_q;
		state.sourceTrustDetails = u"no-active-session"_q;
		return;
	}
	const auto &appConfig = session->appConfig();
	const std::vector<int64> trustedChannels
		= appConfig.astrogramTrustedPluginChannelIds();
	const std::vector<QString> trustedRecords
		= appConfig.astrogramTrustedPluginRecords();
	auto matchedHashInUntrustedChannel = false;
	auto matchedHashWithoutOrigin = false;
	auto hasValidTrustedRecord = false;
	auto matchedChannelId = int64(0);
	auto matchedMessageId = int64(0);
	for (const auto &rawRecord : trustedRecords) {
		const auto record = parseRecord(rawRecord);
		if (!record.sha256.isEmpty()) {
			hasValidTrustedRecord = true;
		}
		if (record.sha256.isEmpty() || record.sha256 != state.sha256) {
			continue;
		}
		if (!record.channelId || (record.messageId <= 0)) {
			matchedHashWithoutOrigin = true;
			continue;
		}
		if (record.channelId) {
			const auto trusted = !trustedChannels.empty()
				&& (std::find(
					trustedChannels.begin(),
					trustedChannels.end(),
					record.channelId) != trustedChannels.end());
			if (!trusted) {
				matchedHashInUntrustedChannel = true;
				matchedChannelId = record.channelId;
				matchedMessageId = record.messageId;
				continue;
			}
		}
		state.sourceVerified = true;
		state.sourceTrustText = u"verified"_q;
		state.sourceTrustReason = u"exact-sha256-trusted-record"_q;
		state.sourceChannelId = record.channelId;
		state.sourceMessageId = record.messageId;
		state.sourceTrustDetails = !record.label.isEmpty()
			? record.label
			: (record.channelId
				? (QString::number(record.channelId)
					+ u":"_q
					+ QString::number(record.messageId))
				: QString());
		return;
	}

	state.sourceChannelId = matchedChannelId;
	state.sourceMessageId = matchedMessageId;
	state.sourceTrustReason = trustedRecords.empty()
		? u"no-trusted-records"_q
		: !hasValidTrustedRecord
		? u"no-valid-trusted-records"_q
		: matchedHashInUntrustedChannel
		? u"hash-found-in-untrusted-channel"_q
		: matchedHashWithoutOrigin
		? u"matching-record-missing-origin"_q
		: u"hash-not-in-trusted-records"_q;
	state.sourceTrustDetails = state.sourceTrustReason;
}

QJsonValue Manager::storedSettingValue(
		const QString &pluginId,
		const QString &settingId) const {
	const auto it = _storedSettings.constFind(pluginId);
	if (it == _storedSettings.cend()) {
		return QJsonValue();
	}
	return it.value().value(settingId);
}

void Manager::applyStoredSettings(
		const QString &pluginId,
		SettingsPageDescriptor &descriptor) const {
	for (auto &section : descriptor.sections) {
		for (auto &setting : section.settings) {
			const auto value = storedSettingValue(pluginId, setting.id);
			if (value.isUndefined() || value.isNull()) {
				continue;
			}
			switch (setting.type) {
			case SettingControl::Toggle:
				if (value.isBool()) {
					setting.boolValue = value.toBool(setting.boolValue);
				}
				break;
			case SettingControl::IntSlider:
				if (value.isDouble()) {
					setting.intValue = std::clamp(
						value.toInt(setting.intValue),
						setting.intMinimum,
						setting.intMaximum);
				}
				break;
			case SettingControl::TextInput:
				if (value.isString()) {
					setting.textValue = value.toString(setting.textValue).trimmed();
				}
				break;
			case SettingControl::ActionButton:
			case SettingControl::InfoText:
				break;
			}
		}
	}
}

void Manager::rememberSettingValue(
		const QString &pluginId,
		const SettingDescriptor &descriptor) {
	if (pluginId.trimmed().isEmpty() || descriptor.id.trimmed().isEmpty()) {
		return;
	}
	auto values = _storedSettings.value(pluginId);
	switch (descriptor.type) {
	case SettingControl::Toggle:
		values.insert(descriptor.id, descriptor.boolValue);
		break;
	case SettingControl::IntSlider:
		values.insert(descriptor.id, descriptor.intValue);
		break;
	case SettingControl::TextInput:
		values.insert(descriptor.id, descriptor.textValue);
		break;
	case SettingControl::ActionButton:
	case SettingControl::InfoText:
		values.remove(descriptor.id);
		break;
	}
	if (values.isEmpty()) {
		_storedSettings.remove(pluginId);
	} else {
		_storedSettings.insert(pluginId, values);
	}
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
	finishUiTransientPluginSnapshot();
	notifyStateChanged(
		metadataOnly ? u"scan-metadata-only"_q : u"scan"_q,
		QString(),
		true,
		false);
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
		auto preview = static_cast<const Plugins::PreviewInfo*>(nullptr);
		try {
			InvokePluginCallbackOrThrow([&] {
				preview = previewInfo();
			});
		} catch (...) {
			record.state.error = u"Plugin preview export failed: "_q + CurrentExceptionText();
			logLoadFailure(path, record.state.error);
			library->unload();
			_plugins.push_back(std::move(record));
			finishRecoveryOperation();
			return;
		}
		if (preview
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
	auto info = static_cast<const Plugins::BinaryInfo*>(nullptr);
	try {
		InvokePluginCallbackOrThrow([&] {
			info = binaryInfo();
		});
	} catch (...) {
		record.state.error = u"TgdPluginBinaryInfo failed: "_q + CurrentExceptionText();
		logLoadFailure(path, record.state.error);
		library->unload();
		_plugins.push_back(std::move(record));
		finishRecoveryOperation();
		return;
	}
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
		InvokePluginCallbackOrThrow([&] {
			instance.reset(entry(this, kApiVersion));
		});
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
			InvokePluginCallbackOrThrow([&] {
				record.state.info = instance->info();
			});
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
				InvokePluginCallbackOrThrow([&] {
					_plugins.back().instance->onLoad();
				});
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
	beginUiTransientPluginSnapshot();
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
			unregisterPluginCommands(plugin.state.info.id);
			unregisterPluginActions(plugin.state.info.id);
			unregisterPluginPanels(plugin.state.info.id);
			unregisterPluginSettingsPages(plugin.state.info.id);
			unregisterPluginOutgoingInterceptors(plugin.state.info.id);
			unregisterPluginMessageObservers(plugin.state.info.id);
			unregisterPluginWindowHandlers(plugin.state.info.id);
			unregisterPluginWindowWidgetHandlers(plugin.state.info.id);
			unregisterPluginSessionHandlers(plugin.state.info.id);
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
				InvokePluginCallbackOrThrow([&] {
					plugin.instance->onUnload();
				});
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
	_settingsPages.clear();
	_settingsPagesByPlugin.clear();
	_nextSettingsPageId = 1;
	_outgoingInterceptors.clear();
	_outgoingInterceptorsByPlugin.clear();
	_nextOutgoingInterceptorId = 1;
	_messageObservers.clear();
	_messageObserversByPlugin.clear();
	_nextMessageObserverId = 1;
	_messageObserverLifetime.destroy();
	_windowHandlers.clear();
	_windowWidgetHandlers.clear();
	_sessionHandlers.clear();

	for (auto &plugin : _plugins) {
		plugin.commandIds.clear();
		plugin.actionIds.clear();
		plugin.panelIds.clear();
		plugin.settingsPageIds.clear();
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

int Manager::findRecordIndex(const QString &pluginId) const {
	const auto normalized = pluginId.trimmed();
	if (normalized.isEmpty()) {
		return -1;
	}
	const auto it = _pluginIndexById.find(normalized);
	if (it != _pluginIndexById.end()) {
		const auto index = it.value();
		if (index >= 0
			&& index < int(_plugins.size())
			&& _plugins[index].state.info.id.trimmed() == normalized) {
			return index;
		}
	}
	for (auto i = 0; i != int(_plugins.size()); ++i) {
		if (_plugins[i].state.info.id.trimmed() == normalized) {
			return i;
		}
	}
	return -1;
}

Manager::PluginRecord *Manager::findRecord(const QString &pluginId) {
	const auto index = findRecordIndex(pluginId);
	return (index >= 0) ? &_plugins[index] : nullptr;
}

const Manager::PluginRecord *Manager::findRecord(
		const QString &pluginId) const {
	const auto index = findRecordIndex(pluginId);
	return (index >= 0) ? &_plugins[index] : nullptr;
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

void Manager::unregisterPluginSettingsPages(const QString &pluginId) {
	const auto ids = _settingsPagesByPlugin.value(pluginId);
	for (const auto id : ids) {
		unregisterSettingsPage(id);
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

void Manager::unregisterPluginWindowWidgetHandlers(const QString &pluginId) {
	_windowWidgetHandlers.erase(
		std::remove_if(
			_windowWidgetHandlers.begin(),
			_windowWidgetHandlers.end(),
			[&](const WindowWidgetHandlerEntry &entry) {
				return entry.pluginId == pluginId;
			}),
		_windowWidgetHandlers.end());
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
	return findRecordIndex(pluginId) >= 0;
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
	const auto trimmedReason = reason.trimmed();
	const auto clientReason = !trimmedReason.isEmpty()
		? trimmedReason
		: recoveryReason.trimmed();
	const auto shouldAttemptUiRecovery = disabledByRecovery
		|| clientReason.contains(u"failed"_q, Qt::CaseInsensitive)
		|| clientReason.contains(u"crash"_q, Qt::CaseInsensitive)
		|| clientReason.contains(u"exception"_q, Qt::CaseInsensitive);
	Logs::writeClient(QString::fromLatin1(
		"[plugins] disable plugin=%1 enabled=%2 loaded=%3 recovery=%4 reason=%5")
		.arg(pluginId)
		.arg(record->state.enabled ? u"true"_q : u"false"_q)
		.arg(record->state.loaded ? u"true"_q : u"false"_q)
		.arg(disabledByRecovery ? u"true"_q : u"false"_q)
		.arg(CompactClientLogText(clientReason)));
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
	unregisterPluginSettingsPages(pluginId);
	unregisterPluginOutgoingInterceptors(pluginId);
	unregisterPluginMessageObservers(pluginId);
	unregisterPluginWindowHandlers(pluginId);
	unregisterPluginWindowWidgetHandlers(pluginId);
	unregisterPluginSessionHandlers(pluginId);
	if (record->instance) {
		_registeringPluginId = pluginId;
		try {
			InvokePluginCallbackOrThrow([&] {
				record->instance->onUnload();
			});
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
	if (shouldAttemptUiRecovery) {
		logEvent(
			u"ui"_q,
			u"recovery-scheduled"_q,
			QJsonObject{
				{ u"pluginId"_q, pluginId },
				{ u"reason"_q, clientReason },
				{ u"disabledByRecovery"_q, disabledByRecovery },
			});
		SchedulePluginUiRecoveryAttempt(pluginId, clientReason);
	}
	logEvent(
		u"plugin"_q,
		u"disabled-finished"_q,
		QJsonObject{
			{ u"pluginId"_q, pluginId },
			{ u"stateAfter"_q, pluginStateToJson(record->state) },
		});
	notifyStateChanged(
		disabledByRecovery ? u"plugin-disabled-recovery"_q : u"plugin-disabled"_q,
		pluginId,
		true,
		!clientReason.isEmpty());
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
			InvokePluginCallbackOrThrow([&] {
				entry.handler(session);
			});
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
		InvokePluginCallbackOrThrow([&] {
			entry.handler(callContext);
		});
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
