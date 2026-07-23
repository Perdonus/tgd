/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QJsonValue>
#include <QtCore/QVector>
#include <QtCore/QtGlobal>

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

class QWidget;
class History;
class HistoryItem;

namespace Plugins {

constexpr int kApiVersion = 5;
constexpr int kBinaryInfoVersion = 1;
constexpr int kPreviewInfoVersion = 1;
constexpr int kHostInfoVersion = 2;
constexpr int kSystemInfoVersion = 1;

#if defined(_MSC_VER)
inline constexpr auto kCompilerId = "msvc";
inline constexpr auto kCompilerVersion = _MSC_VER;
#elif defined(__clang__)
inline constexpr auto kCompilerId = "clang";
inline constexpr auto kCompilerVersion = (__clang_major__ * 100) + __clang_minor__;
#elif defined(__GNUC__)
inline constexpr auto kCompilerId = "gcc";
inline constexpr auto kCompilerVersion = (__GNUC__ * 100) + __GNUC_MINOR__;
#else
inline constexpr auto kCompilerId = "unknown";
inline constexpr auto kCompilerVersion = 0;
#endif

#if defined(_WIN32)
inline constexpr auto kPlatformId = "windows";
#elif defined(__APPLE__)
inline constexpr auto kPlatformId = "macos";
#elif defined(__linux__)
inline constexpr auto kPlatformId = "linux";
#else
inline constexpr auto kPlatformId = "unknown";
#endif

struct BinaryInfo {
	int structVersion = kBinaryInfoVersion;
	int apiVersion = kApiVersion;
	int pointerSize = int(sizeof(void*));
	int qtMajor = QT_VERSION_MAJOR;
	int qtMinor = QT_VERSION_MINOR;
	int compilerVersion = kCompilerVersion;
	const char *compiler = kCompilerId;
	const char *platform = kPlatformId;
};

constexpr BinaryInfo MakeBinaryInfo() {
	auto result = BinaryInfo();
	result.structVersion = kBinaryInfoVersion;
	result.apiVersion = kApiVersion;
	result.pointerSize = int(sizeof(void*));
	result.qtMajor = QT_VERSION_MAJOR;
	result.qtMinor = QT_VERSION_MINOR;
	result.compilerVersion = kCompilerVersion;
	result.compiler = kCompilerId;
	result.platform = kPlatformId;
	return result;
}

inline constexpr auto kBinaryInfo = MakeBinaryInfo();

struct PreviewInfo {
	int structVersion = kPreviewInfoVersion;
	int apiVersion = kApiVersion;
	const char *id = nullptr;
	const char *name = nullptr;
	const char *version = nullptr;
	const char *author = nullptr;
	const char *description = nullptr;
	const char *website = nullptr;
	const char *icon = nullptr;
};

struct PluginInfo {
	QString id;
	QString name;
	QString version;
	QString author;
	QString description;
	QString website;
};

struct HostInfo {
	int structVersion = kHostInfoVersion;
	int apiVersion = kApiVersion;
	int pointerSize = int(sizeof(void*));
	int qtMajor = QT_VERSION_MAJOR;
	int qtMinor = QT_VERSION_MINOR;
	int compilerVersion = kCompilerVersion;
	QString appVersion;
	QString appUiLanguage;
	QString compiler;
	QString platform;
	QString workingPath;
	QString pluginsPath;
	bool safeModeEnabled = false;
	bool runtimeApiEnabled = false;
	int runtimeApiPort = 0;
	QString runtimeApiBaseUrl;
};

struct SystemInfo {
	int structVersion = kSystemInfoVersion;
	quint64 processId = 0;
	quint64 totalMemoryBytes = 0;
	quint64 availableMemoryBytes = 0;
	int logicalCpuCores = 0;
	int physicalCpuCores = 0;
	QString productType;
	QString productVersion;
	QString prettyProductName;
	QString kernelType;
	QString kernelVersion;
	QString architecture;
	QString buildAbi;
	QString hostName;
	QString userName;
	QString locale;
	QString uiLanguage;
	QString timeZone;
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

enum class SettingControl {
	Toggle,
	IntSlider,
	TextInput,
	ActionButton,
	InfoText,
};

struct SettingDescriptor {
	QString id;
	QString title;
	QString description;
	SettingControl type = SettingControl::InfoText;
	bool boolValue = false;
	int intValue = 0;
	int intMinimum = 0;
	int intMaximum = 100;
	int intStep = 1;
	QString valueSuffix;
	QString textValue;
	QString placeholderText;
	bool secret = false;
	QString buttonText;
};

struct SettingsSectionDescriptor {
	QString id;
	QString title;
	QString description;
	QVector<SettingDescriptor> settings;
};

struct SettingsPageDescriptor {
	QString id;
	QString title;
	QString description;
	QVector<SettingsSectionDescriptor> sections;
};

using SettingsChangedHandler = std::function<void(const SettingDescriptor &)>;
using SettingsPageId = uint64_t;

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

	virtual SettingsPageId registerSettingsPage(
		const QString &pluginId,
		SettingsPageDescriptor descriptor,
		SettingsChangedHandler handler) = 0;
	virtual void unregisterSettingsPage(SettingsPageId id) = 0;

	virtual void showToast(const QString &text) = 0;
	virtual void forEachWindow(
		std::function<void(Window::Controller*)> visitor) = 0;
	virtual void onWindowCreated(
		std::function<void(Window::Controller*)> handler) = 0;
	virtual void forEachWindowWidget(
		std::function<void(QWidget*)> visitor) = 0;
	virtual void onWindowWidgetCreated(
		std::function<void(QWidget*)> handler) = 0;

	virtual Window::Controller *activeWindow() const = 0;
	virtual QWidget *activeWindowWidget() const = 0;
	virtual QJsonValue storedSettingValue(
		const QString &pluginId,
		const QString &settingId) const = 0;
	virtual bool settingBoolValue(
		const QString &pluginId,
		const QString &settingId,
		bool fallback) const = 0;
	virtual int settingIntValue(
		const QString &pluginId,
		const QString &settingId,
		int fallback) const = 0;
	virtual QString settingStringValue(
		const QString &pluginId,
		const QString &settingId,
		const QString &fallback) const = 0;
	virtual Main::Session *activeSession() const = 0;
	virtual void forEachSession(
		std::function<void(Main::Session*)> visitor) = 0;
	virtual void onSessionActivated(
		std::function<void(Main::Session*)> handler) = 0;
	virtual HostInfo hostInfo() const = 0;
	virtual SystemInfo systemInfo() const = 0;
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
using BinaryInfoFn = const BinaryInfo* (*)();
using PreviewInfoFn = const PreviewInfo* (*)();

} // namespace Plugins

#if defined(_WIN32)
#define TGD_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define TGD_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define TGD_PLUGIN_EXPORT
#endif

#define TGD_PLUGIN_ENTRY \
	extern "C" TGD_PLUGIN_EXPORT const Plugins::BinaryInfo *TgdPluginBinaryInfo() { \
		return &Plugins::kBinaryInfo; \
	} \
	extern "C" TGD_PLUGIN_EXPORT Plugins::Plugin *TgdPluginEntry( \
		Plugins::Host *host, \
		int apiVersion)

#define TGD_PLUGIN_MANIFEST_MAGIC "TGD_PLUGIN_MANIFEST_V1"
#define TGD_PLUGIN_MANIFEST_END "TGD_PLUGIN_MANIFEST_END"

#define TGD_PLUGIN_PREVIEW(ID, NAME, VERSION, AUTHOR, DESCRIPTION, WEBSITE, ICON) \
	namespace { \
	inline constexpr Plugins::PreviewInfo kTgdPluginPreviewInfo = { \
		Plugins::kPreviewInfoVersion, \
		Plugins::kApiVersion, \
		ID, \
		NAME, \
		VERSION, \
		AUTHOR, \
		DESCRIPTION, \
		WEBSITE, \
		ICON, \
	}; \
	} \
	extern "C" TGD_PLUGIN_EXPORT const char TgdPluginManifest[] = \
		TGD_PLUGIN_MANIFEST_MAGIC "\0" \
		ID "\0" \
		NAME "\0" \
		VERSION "\0" \
		AUTHOR "\0" \
		DESCRIPTION "\0" \
		WEBSITE "\0" \
		ICON "\0" \
		TGD_PLUGIN_MANIFEST_END "\0"; \
	extern "C" TGD_PLUGIN_EXPORT const Plugins::PreviewInfo *TgdPluginPreviewInfo() { \
		return &kTgdPluginPreviewInfo; \
	}
