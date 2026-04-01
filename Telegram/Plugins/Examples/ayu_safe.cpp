/*
AyuSafe-lite plugin for Telegram Desktop.
Provides host-managed Ayu-inspired settings, global font scaling, soft
widget chrome, and a local message-safety cache for edited/deleted events.
*/
#include "plugins/plugins_api.h"

#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>

TGD_PLUGIN_PREVIEW(
	"astro.ayu_safe",
	"AyuSafe",
	"0.4",
	"Astrogram",
	"AyuSafe-lite: Ayu-inspired visuals, deleted-message archive, streamer-lite, and local safety tools.",
	"https://github.com/AyuGram/AyuGramDesktop",
	"GusTheDuck/7")

namespace {

constexpr auto kPluginId = "astro.ayu_safe";
constexpr auto kPluginVersion = "0.4";
constexpr int kDefaultFontScale = 100;
constexpr int kMinFontScale = 85;
constexpr int kMaxFontScale = 130;
constexpr int kDefaultMaxCacheEntries = 2000;
constexpr int kMinCacheEntries = 200;
constexpr int kMaxCacheEntries = 10000;

constexpr auto kFontScaleSettingId = "font_scale";
constexpr auto kSoftChromeSettingId = "soft_chrome";
constexpr auto kGenericWindowTitlesSettingId = "generic_window_titles";
constexpr auto kCacheEnabledSettingId = "cache_enabled";
constexpr auto kCaptureEditsSettingId = "capture_edits";
constexpr auto kCaptureDeletesSettingId = "capture_deletes";
constexpr auto kPersistCacheSettingId = "persist_cache";
constexpr auto kLoadCacheOnStartSettingId = "load_cache_on_start";
constexpr auto kMaxCacheEntriesSettingId = "max_cache_entries";
constexpr auto kFilterZalgoOutgoingSettingId = "filter_zalgo_outgoing";
constexpr auto kDeletedMarkSettingId = "deleted_mark";
constexpr auto kEditedMarkSettingId = "edited_mark";
constexpr auto kExportCacheSettingId = "export_cache";
constexpr auto kOpenCacheFolderSettingId = "open_cache_folder";
constexpr auto kClearCacheSettingId = "clear_cache";

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
}

QString Normalize(const QString &value) {
	auto result = value;
	result.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	result.replace(QChar::fromLatin1('\r'), QChar::fromLatin1('\n'));
	return result.trimmed();
}

QString HostUiLanguage(const Plugins::HostInfo &hostInfo) {
	return hostInfo.appUiLanguage;
}

QString EventName(const Plugins::MessageEvent event) {
	switch (event) {
	case Plugins::MessageEvent::New:
		return QStringLiteral("new");
	case Plugins::MessageEvent::Edited:
		return QStringLiteral("edited");
	case Plugins::MessageEvent::Deleted:
		return QStringLiteral("deleted");
	}
	return QStringLiteral("unknown");
}

QString StripZalgo(const QString &value) {
	QString result;
	result.reserve(value.size());
	for (const auto ch : value) {
		switch (ch.category()) {
		case QChar::Mark_NonSpacing:
		case QChar::Mark_SpacingCombining:
		case QChar::Mark_Enclosing:
			continue;
		default:
			result.append(ch);
			break;
		}
	}
	return result;
}

QString AyuChromeStyle() {
	return QString::fromUtf8(R"CSS(
QPushButton, QToolButton, QLineEdit, QPlainTextEdit, QTextEdit, QComboBox {
	border-radius: 12px;
}
QAbstractScrollArea {
	border-radius: 16px;
	background-color: rgba(23, 28, 38, 0.08);
}
QMenu {
	border-radius: 14px;
	background-color: rgba(20, 24, 33, 0.94);
}
)CSS");
}

struct CacheEntry {
	QString timestampUtc;
	QString event;
	QString peerName;
	QString text;
	qint64 messageId = 0;
	qint64 sessionUniqueId = 0;
	bool outgoing = false;
};

} // namespace

class AyuSafePlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit AyuSafePlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host) {
		refreshInfo();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		refreshInfo();
		captureBaseVisualState();
		if (loadCacheOnStart()) {
			loadCacheFromDisk();
		}
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSettingChanged(setting);
			});
		_host->forEachWindowWidget([this](QWidget *widget) {
			registerWindow(widget);
		});
		_host->onWindowWidgetCreated([this](QWidget *widget) {
			registerWindow(widget);
		});
		applyVisualSettings();
		refreshObserver();
		refreshOutgoingInterceptor();
	}

	void onUnload() override {
		restoreWindowTitles();
		if (_observerId) {
			_host->unregisterMessageObserver(_observerId);
			_observerId = 0;
		}
		if (_outgoingInterceptorId) {
			_host->unregisterOutgoingTextInterceptor(_outgoingInterceptorId);
			_outgoingInterceptorId = 0;
		}
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		for (auto *widget : _trackedWindows) {
			if (widget) {
				widget->removeEventFilter(this);
			}
		}
		_trackedWindows.clear();
		_retitlePending.clear();
		_originalWindowTitles.clear();
		restoreVisualSettings();
	}

	bool eventFilter(QObject *watched, QEvent *event) override {
		const auto widget = qobject_cast<QWidget*>(watched);
		if (!widget || !event) {
			return QObject::eventFilter(watched, event);
		}
		switch (event->type()) {
		case QEvent::WindowTitleChange:
		case QEvent::Show:
			scheduleWindowRefresh(widget);
			break;
		default:
			break;
		}
		return QObject::eventFilter(watched, event);
	}

private:
	[[nodiscard]] bool useRussian() const {
		const auto hostInfo = _host->hostInfo();
		auto language = Normalize(HostUiLanguage(hostInfo));
		if (language.isEmpty()) {
			language = Normalize(_host->systemInfo().uiLanguage);
		}
		return language.startsWith(QStringLiteral("ru"), Qt::CaseInsensitive);
	}

	[[nodiscard]] QString tr(const char *en, const char *ru) const {
		return useRussian()
			? QString::fromUtf8(ru)
			: QString::fromUtf8(en);
	}

	void refreshInfo() {
		_info.id = Latin1(kPluginId);
		_info.name = QStringLiteral("AyuSafe");
		_info.version = Latin1(kPluginVersion);
		_info.author = QStringLiteral("Astrogram");
		_info.description = tr(
			"AyuSafe-lite: Ayu-inspired visuals, streamer-lite privacy, and local message safety tools.",
			"AyuSafe-lite: визуальные фишки в стиле Ayu, лайт-стример режим и локальные инструменты безопасности сообщений.");
		_info.website = QStringLiteral("https://github.com/AyuGram/AyuGramDesktop");
	}

	[[nodiscard]] int fontScalePercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				Latin1(kFontScaleSettingId),
				kDefaultFontScale),
			kMinFontScale,
			kMaxFontScale);
	}

	[[nodiscard]] bool softChromeEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kSoftChromeSettingId),
			true);
	}

	[[nodiscard]] bool genericWindowTitlesEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kGenericWindowTitlesSettingId),
			false);
	}

	[[nodiscard]] bool cacheEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kCacheEnabledSettingId),
			true);
	}

	[[nodiscard]] bool captureEdits() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kCaptureEditsSettingId),
			true);
	}

	[[nodiscard]] bool captureDeletes() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kCaptureDeletesSettingId),
			true);
	}

	[[nodiscard]] bool persistCacheEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kPersistCacheSettingId),
			true);
	}

	[[nodiscard]] bool loadCacheOnStart() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kLoadCacheOnStartSettingId),
			true);
	}

	[[nodiscard]] int maxCacheEntries() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				Latin1(kMaxCacheEntriesSettingId),
				kDefaultMaxCacheEntries),
			kMinCacheEntries,
			kMaxCacheEntries);
	}

	[[nodiscard]] bool filterZalgoOutgoingEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			Latin1(kFilterZalgoOutgoingSettingId),
			false);
	}

	[[nodiscard]] QString deletedMark() const {
		const auto value = Normalize(_host->settingStringValue(
			_info.id,
			Latin1(kDeletedMarkSettingId),
			QStringLiteral("deleted")));
		return value.isEmpty()
			? tr("deleted", "удалено")
			: value;
	}

	[[nodiscard]] QString editedMark() const {
		const auto value = Normalize(_host->settingStringValue(
			_info.id,
			Latin1(kEditedMarkSettingId),
			QStringLiteral("edited")));
		return value.isEmpty()
			? tr("edited", "изменено")
			: value;
	}

	[[nodiscard]] QString cacheStatePath() const {
		const auto baseDir = _host->pluginsPath();
		return baseDir.isEmpty()
			? QString()
			: QDir(baseDir).filePath(QStringLiteral("ayu_safe_state.json"));
	}

	void captureBaseVisualState() {
		if (_capturedBaseVisuals) {
			return;
		}
		_baseFont = QApplication::font();
		_baseStyleSheet = qApp ? qApp->styleSheet() : QString();
		_capturedBaseVisuals = true;
	}

	void applyVisualSettings() const {
		if (!_capturedBaseVisuals) {
			return;
		}
		auto font = _baseFont;
		const auto scale = fontScalePercent();
		if (const auto pointSize = _baseFont.pointSizeF(); pointSize > 0.) {
			font.setPointSizeF(pointSize * scale / 100.);
		} else if (const auto pixelSize = _baseFont.pixelSize(); pixelSize > 0) {
			font.setPixelSize(
				std::max(1, int(std::lround(pixelSize * scale / 100.))));
		}
		QApplication::setFont(font);
		if (qApp) {
			const auto style = softChromeEnabled()
				? (_baseStyleSheet + AyuChromeStyle())
				: _baseStyleSheet;
			qApp->setStyleSheet(style);
		}
		_host->forEachWindowWidget([](QWidget *widget) {
			if (widget) {
				widget->update();
			}
		});
		for (auto *widget : _trackedWindows) {
			applyWindowTitle(widget);
		}
	}

	void restoreVisualSettings() const {
		if (!_capturedBaseVisuals) {
			return;
		}
		QApplication::setFont(_baseFont);
		if (qApp) {
			qApp->setStyleSheet(_baseStyleSheet);
		}
		_host->forEachWindowWidget([](QWidget *widget) {
			if (widget) {
				widget->update();
			}
		});
		restoreWindowTitles();
	}

	void registerWindow(QWidget *widget) {
		if (!widget || !widget->isWindow() || widget->parentWidget()) {
			return;
		}
		if (_trackedWindows.contains(widget)) {
			scheduleWindowRefresh(widget);
			return;
		}
		_trackedWindows.insert(widget);
		widget->installEventFilter(this);
		QObject::connect(
			widget,
			&QObject::destroyed,
			this,
			[this, widget](QObject *) {
				_trackedWindows.remove(widget);
				_retitlePending.remove(widget);
				_originalWindowTitles.remove(widget);
			});
		scheduleWindowRefresh(widget);
	}

	void scheduleWindowRefresh(QWidget *widget) {
		if (!widget || !_trackedWindows.contains(widget) || _retitlePending.contains(widget)) {
			return;
		}
		_retitlePending.insert(widget);
		QTimer::singleShot(0, this, [this, guard = QPointer<QWidget>(widget)] {
			if (!guard) {
				return;
			}
			_retitlePending.remove(guard.data());
			applyWindowTitle(guard.data());
		});
	}

	void applyWindowTitle(QWidget *widget) const {
		if (!widget) {
			return;
		}
		const auto currentTitle = widget->windowTitle();
		if (!genericWindowTitlesEnabled()) {
			if (const auto i = _originalWindowTitles.constFind(widget);
				i != _originalWindowTitles.cend()
				&& currentTitle == QStringLiteral("Astrogram")
				&& !i.value().isEmpty()) {
				widget->setWindowTitle(i.value());
			}
			return;
		}
		if (currentTitle != QStringLiteral("Astrogram")) {
			_originalWindowTitles.insert(widget, currentTitle);
		}
		const auto generic = QStringLiteral("Astrogram");
		if (currentTitle != generic) {
			widget->setWindowTitle(generic);
		}
	}

	void restoreWindowTitles() const {
		for (auto i = _originalWindowTitles.cbegin(), end = _originalWindowTitles.cend();
			i != end;
			++i) {
			if (const auto widget = i.key();
				widget && widget->windowTitle() == QStringLiteral("Astrogram")
				&& !i.value().isEmpty()) {
				widget->setWindowTitle(i.value());
			}
		}
	}

	void refreshObserver() {
		if (_observerId) {
			_host->unregisterMessageObserver(_observerId);
			_observerId = 0;
		}
		if (!cacheEnabled()) {
			return;
		}
		auto options = Plugins::MessageObserverOptions();
		options.newMessages = true;
		options.editedMessages = captureEdits();
		options.deletedMessages = captureDeletes();
		options.incoming = true;
		options.outgoing = true;
		_observerId = _host->registerMessageObserver(
			_info.id,
			options,
			[this](const Plugins::MessageEventContext &context) {
				rememberEvent(context);
			});
	}

	void refreshOutgoingInterceptor() {
		if (_outgoingInterceptorId) {
			_host->unregisterOutgoingTextInterceptor(_outgoingInterceptorId);
			_outgoingInterceptorId = 0;
		}
		if (!filterZalgoOutgoingEnabled()) {
			return;
		}
		_outgoingInterceptorId = _host->registerOutgoingTextInterceptor(
			_info.id,
			[this](const Plugins::OutgoingTextContext &context) {
				Q_UNUSED(context);
				auto result = Plugins::CommandResult();
				const auto cleaned = StripZalgo(context.text);
				if (cleaned == context.text) {
					result.action = Plugins::CommandResult::Action::Continue;
					return result;
				}
				result.action = Plugins::CommandResult::Action::ReplaceText;
				result.replacementText = cleaned;
				return result;
			},
			80);
	}

	void rememberEvent(const Plugins::MessageEventContext &context) {
		auto entry = CacheEntry();
		entry.timestampUtc = QDateTime::currentDateTimeUtc().toString(
			Qt::ISODateWithMs);
		entry.event = EventName(context.event);
		entry.sessionUniqueId = context.session
			? qint64(context.session->uniqueId())
			: 0;
		if (context.history) {
			entry.peerName = context.history->peer->name();
		}
		if (context.item) {
			entry.messageId = qint64(context.item->id);
			entry.outgoing = context.item->out();
			entry.text = extractItemText(context.item);
		}
		if (entry.text.isEmpty()) {
			entry.text = findCachedText(entry.sessionUniqueId, entry.messageId);
		}
		if (context.event == Plugins::MessageEvent::Deleted && !entry.text.isEmpty()) {
			entry.text = QStringLiteral("[%1] %2").arg(deletedMark(), entry.text);
		} else if (context.event == Plugins::MessageEvent::Edited && !entry.text.isEmpty()) {
			entry.text = QStringLiteral("[%1] %2").arg(editedMark(), entry.text);
		}
		if (entry.text.isEmpty()) {
			entry.text = tr("[non-text or unavailable message]", "[нетекстовое или недоступное сообщение]");
		}
		_cache.push_back(std::move(entry));
		trimCache();
		saveCacheIfNeeded();
	}

	[[nodiscard]] QString findCachedText(qint64 sessionUniqueId, qint64 messageId) const {
		if (!sessionUniqueId || !messageId) {
			return QString();
		}
		for (auto i = _cache.crbegin(), end = _cache.crend(); i != end; ++i) {
			if (i->sessionUniqueId == sessionUniqueId
				&& i->messageId == messageId
				&& !i->text.isEmpty()) {
				return i->text;
			}
		}
		return QString();
	}

	void trimCache() {
		const auto limit = maxCacheEntries();
		while (_cache.size() > limit) {
			_cache.remove(0);
		}
	}

	void saveCacheIfNeeded() const {
		if (!persistCacheEnabled()) {
			return;
		}
		saveCacheStateToDisk();
	}

	void saveCacheStateToDisk() const {
		const auto path = cacheStatePath();
		if (path.isEmpty()) {
			return;
		}
		const auto baseDir = QFileInfo(path).absolutePath();
		if (!baseDir.isEmpty()) {
			QDir().mkpath(baseDir);
		}
		auto items = QJsonArray();
		for (const auto &entry : _cache) {
			items.push_back(QJsonObject{
				{ QStringLiteral("timestampUtc"), entry.timestampUtc },
				{ QStringLiteral("event"), entry.event },
				{ QStringLiteral("peerName"), entry.peerName },
				{ QStringLiteral("text"), entry.text },
				{ QStringLiteral("messageId"), QString::number(entry.messageId) },
				{ QStringLiteral("sessionUniqueId"), QString::number(entry.sessionUniqueId) },
				{ QStringLiteral("outgoing"), entry.outgoing },
			});
		}
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly)) {
			return;
		}
		file.write(QJsonDocument(QJsonObject{
			{ QStringLiteral("version"), QStringLiteral("1") },
			{ QStringLiteral("entries"), items },
		}).toJson(QJsonDocument::Indented));
		file.commit();
	}

	void loadCacheFromDisk() {
		_cache.clear();
		const auto path = cacheStatePath();
		if (path.isEmpty()) {
			return;
		}
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		QJsonParseError error;
		const auto document = QJsonDocument::fromJson(file.readAll(), &error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			return;
		}
		const auto items = document.object().value(QStringLiteral("entries")).toArray();
		_cache.reserve(items.size());
		for (const auto &value : items) {
			if (!value.isObject()) {
				continue;
			}
			const auto object = value.toObject();
			auto entry = CacheEntry();
			entry.timestampUtc = object.value(QStringLiteral("timestampUtc")).toString();
			entry.event = object.value(QStringLiteral("event")).toString();
			entry.peerName = object.value(QStringLiteral("peerName")).toString();
			entry.text = object.value(QStringLiteral("text")).toString();
			entry.messageId = object.value(QStringLiteral("messageId")).toString().toLongLong();
			entry.sessionUniqueId = object.value(QStringLiteral("sessionUniqueId")).toString().toLongLong();
			entry.outgoing = object.value(QStringLiteral("outgoing")).toBool();
			_cache.push_back(std::move(entry));
		}
		trimCache();
	}

	[[nodiscard]] QString extractItemText(HistoryItem *item) const {
		if (!item) {
			return QString();
		}
		if (const auto text = Normalize(item->originalText().text);
			!text.isEmpty()) {
			return text;
		}
		if (const auto text = Normalize(item->notificationText().text);
			!text.isEmpty()) {
			return text;
		}
		if (const auto text = Normalize(item->inReplyText().text);
			!text.isEmpty()) {
			return text;
		}
		return QString();
	}

	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto fontScale = Plugins::SettingDescriptor();
		fontScale.id = Latin1(kFontScaleSettingId);
		fontScale.title = tr("Font scale", "Масштаб шрифта");
		fontScale.description = tr(
			"A global font scale inspired by Ayu-style readability tweaks.",
			"Глобальный масштаб шрифта в стиле Ayu для более читаемого интерфейса.");
		fontScale.type = Plugins::SettingControl::IntSlider;
		fontScale.intValue = fontScalePercent();
		fontScale.intMinimum = kMinFontScale;
		fontScale.intMaximum = kMaxFontScale;
		fontScale.intStep = 1;
		fontScale.valueSuffix = QStringLiteral("%");

		auto softChrome = Plugins::SettingDescriptor();
		softChrome.id = Latin1(kSoftChromeSettingId);
		softChrome.title = tr("Soft chrome", "Мягкий хром");
		softChrome.description = tr(
			"Adds softer rounded Qt controls and menu surfaces where the host allows it.",
			"Добавляет более мягкие скруглённые Qt-контролы и поверхности меню там, где это позволяет хост.");
		softChrome.type = Plugins::SettingControl::Toggle;
		softChrome.boolValue = softChromeEnabled();

		auto genericTitles = Plugins::SettingDescriptor();
		genericTitles.id = Latin1(kGenericWindowTitlesSettingId);
		genericTitles.title = tr("Streamer mode lite", "Лайт-стример режим");
		genericTitles.description = tr(
			"Replaces Telegram window titles with a generic Astrogram title, similar to Ayu streamer-mode privacy.",
			"Заменяет заголовки окон Telegram на общий заголовок Astrogram, примерно как Ayu-privacy режим для стримов.");
		genericTitles.type = Plugins::SettingControl::Toggle;
		genericTitles.boolValue = genericWindowTitlesEnabled();

		auto visuals = Plugins::SettingsSectionDescriptor();
		visuals.id = QStringLiteral("appearance");
		visuals.title = tr("Appearance", "Внешний вид");
		visuals.description = tr(
			"Portable Ayu-style tweaks that fit the current plugin API.",
			"Переносимые настройки в стиле Ayu, которые укладываются в текущий plugin API.");
		visuals.settings.push_back(fontScale);
		visuals.settings.push_back(softChrome);
		visuals.settings.push_back(genericTitles);

		auto cacheToggle = Plugins::SettingDescriptor();
		cacheToggle.id = Latin1(kCacheEnabledSettingId);
		cacheToggle.title = tr("Local message safety cache", "Локальный кэш безопасности сообщений");
		cacheToggle.description = tr(
			"Caches new messages locally so edited and deleted events are easier to inspect later.",
			"Локально кэширует новые сообщения, чтобы затем было проще разбирать edited/deleted события.");
		cacheToggle.type = Plugins::SettingControl::Toggle;
		cacheToggle.boolValue = cacheEnabled();

		auto edits = Plugins::SettingDescriptor();
		edits.id = Latin1(kCaptureEditsSettingId);
		edits.title = tr("Track edits", "Отслеживать правки");
		edits.description = tr(
			"Adds edited-message events to the local AyuSafe cache.",
			"Добавляет события правки сообщений в локальный кэш AyuSafe.");
		edits.type = Plugins::SettingControl::Toggle;
		edits.boolValue = captureEdits();

		auto deletes = Plugins::SettingDescriptor();
		deletes.id = Latin1(kCaptureDeletesSettingId);
		deletes.title = tr("Track deletes", "Отслеживать удаления");
		deletes.description = tr(
			"Adds deleted-message events to the local AyuSafe cache.",
			"Добавляет события удаления сообщений в локальный кэш AyuSafe.");
		deletes.type = Plugins::SettingControl::Toggle;
		deletes.boolValue = captureDeletes();

		auto persistCache = Plugins::SettingDescriptor();
		persistCache.id = Latin1(kPersistCacheSettingId);
		persistCache.title = tr("Persist cache to disk", "Сохранять кэш на диск");
		persistCache.description = tr(
			"Keeps the local safety archive between restarts in ayu_safe_state.json.",
			"Сохраняет локальный архив безопасности между перезапусками в ayu_safe_state.json.");
		persistCache.type = Plugins::SettingControl::Toggle;
		persistCache.boolValue = persistCacheEnabled();

		auto loadCache = Plugins::SettingDescriptor();
		loadCache.id = Latin1(kLoadCacheOnStartSettingId);
		loadCache.title = tr("Load cache on startup", "Загружать кэш при старте");
		loadCache.description = tr(
			"Restores the AyuSafe archive from disk when the plugin starts.",
			"Восстанавливает архив AyuSafe с диска при старте плагина.");
		loadCache.type = Plugins::SettingControl::Toggle;
		loadCache.boolValue = loadCacheOnStart();

		auto maxEntries = Plugins::SettingDescriptor();
		maxEntries.id = Latin1(kMaxCacheEntriesSettingId);
		maxEntries.title = tr("Archive size", "Размер архива");
		maxEntries.description = tr(
			"Limits how many safety records AyuSafe keeps locally.",
			"Ограничивает, сколько записей безопасности AyuSafe хранит локально.");
		maxEntries.type = Plugins::SettingControl::IntSlider;
		maxEntries.intValue = maxCacheEntries();
		maxEntries.intMinimum = kMinCacheEntries;
		maxEntries.intMaximum = kMaxCacheEntries;
		maxEntries.intStep = 100;
		maxEntries.valueSuffix = tr(" entries", " записей");

		auto filterZalgo = Plugins::SettingDescriptor();
		filterZalgo.id = Latin1(kFilterZalgoOutgoingSettingId);
		filterZalgo.title = tr("Filter zalgo on send", "Фильтровать zalgo при отправке");
		filterZalgo.description = tr(
			"Strips combining spam marks from outgoing text before it is sent.",
			"Удаляет комбинируемые zalgo-символы из исходящего текста перед отправкой.");
		filterZalgo.type = Plugins::SettingControl::Toggle;
		filterZalgo.boolValue = filterZalgoOutgoingEnabled();

		auto deletedMarkSetting = Plugins::SettingDescriptor();
		deletedMarkSetting.id = Latin1(kDeletedMarkSettingId);
		deletedMarkSetting.title = tr("Deleted mark", "Метка удаления");
		deletedMarkSetting.description = tr(
			"Prefix used for deleted-message archive entries.",
			"Префикс, который используется у записей архива удалённых сообщений.");
		deletedMarkSetting.type = Plugins::SettingControl::TextInput;
		deletedMarkSetting.textValue = deletedMark();
		deletedMarkSetting.placeholderText = tr("deleted", "удалено");

		auto editedMarkSetting = Plugins::SettingDescriptor();
		editedMarkSetting.id = Latin1(kEditedMarkSettingId);
		editedMarkSetting.title = tr("Edited mark", "Метка правки");
		editedMarkSetting.description = tr(
			"Prefix used for edited-message archive entries.",
			"Префикс, который используется у записей архива изменённых сообщений.");
		editedMarkSetting.type = Plugins::SettingControl::TextInput;
		editedMarkSetting.textValue = editedMark();
		editedMarkSetting.placeholderText = tr("edited", "изменено");

		auto exportCache = Plugins::SettingDescriptor();
		exportCache.id = Latin1(kExportCacheSettingId);
		exportCache.title = tr("Export cache", "Экспортировать кэш");
		exportCache.description = tr(
			"Writes the current local cache to ayu_safe_cache.json in the plugins folder.",
			"Записывает текущий локальный кэш в ayu_safe_cache.json в папке плагинов.");
		exportCache.type = Plugins::SettingControl::ActionButton;
		exportCache.buttonText = tr("Export", "Экспорт");

		auto openCacheFolder = Plugins::SettingDescriptor();
		openCacheFolder.id = Latin1(kOpenCacheFolderSettingId);
		openCacheFolder.title = tr("Open cache folder", "Открыть папку кэша");
		openCacheFolder.description = tr(
			"Opens the plugins folder where AyuSafe exports and state files are stored.",
			"Открывает папку плагинов, где лежат экспорт и state-файлы AyuSafe.");
		openCacheFolder.type = Plugins::SettingControl::ActionButton;
		openCacheFolder.buttonText = tr("Open folder", "Открыть папку");

		auto clearCache = Plugins::SettingDescriptor();
		clearCache.id = Latin1(kClearCacheSettingId);
		clearCache.title = tr("Clear cache", "Очистить кэш");
		clearCache.description = tr(
			"Clears all cached AyuSafe message entries from memory.",
			"Очищает все кэшированные записи AyuSafe из памяти.");
		clearCache.type = Plugins::SettingControl::ActionButton;
		clearCache.buttonText = tr("Clear", "Очистить");

		auto safety = Plugins::SettingsSectionDescriptor();
		safety.id = QStringLiteral("message_safety");
		safety.title = tr("Message safety", "Безопасность сообщений");
		safety.description = tr(
			"Ayu-inspired local logging for new, edited, and deleted messages.",
			"Локальное логирование новых, изменённых и удалённых сообщений в духе Ayu.");
		safety.settings.push_back(cacheToggle);
		safety.settings.push_back(edits);
		safety.settings.push_back(deletes);
		safety.settings.push_back(persistCache);
		safety.settings.push_back(loadCache);
		safety.settings.push_back(maxEntries);
		safety.settings.push_back(filterZalgo);
		safety.settings.push_back(deletedMarkSetting);
		safety.settings.push_back(editedMarkSetting);
		safety.settings.push_back(exportCache);
		safety.settings.push_back(openCacheFolder);
		safety.settings.push_back(clearCache);

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("note");
		info.title = tr("Portable subset", "Переносимый набор");
		info.description = tr(
			"This plugin intentionally ships only the Ayu-style features that fit the current host API safely. It is not a full AyuGram clone.",
			"Этот плагин специально включает только те Ayu-фишки, которые можно безопасно поднять на текущем host API. Это не полный клон AyuGram.");
		info.type = Plugins::SettingControl::InfoText;

		auto notes = Plugins::SettingsSectionDescriptor();
		notes.id = QStringLiteral("notes");
		notes.title = tr("Notes", "Примечания");
		notes.settings.push_back(info);

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("ayu_safe");
		page.title = QStringLiteral("AyuSafe");
		page.description = tr(
			"Ayu-inspired appearance, streamer-lite privacy, and safety features for Astrogram.",
			"Ayu-вдохновлённые визуальные, стримерские и защитные функции для Astrogram.");
		page.sections.push_back(visuals);
		page.sections.push_back(safety);
		page.sections.push_back(notes);
		return page;
	}

	void handleSettingChanged(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kFontScaleSettingId)
			|| setting.id == Latin1(kSoftChromeSettingId)
			|| setting.id == Latin1(kGenericWindowTitlesSettingId)) {
			applyVisualSettings();
			for (auto *widget : _trackedWindows) {
				scheduleWindowRefresh(widget);
			}
			return;
		}
		if (setting.id == Latin1(kCacheEnabledSettingId)
			|| setting.id == Latin1(kCaptureEditsSettingId)
			|| setting.id == Latin1(kCaptureDeletesSettingId)) {
			refreshObserver();
			return;
		}
		if (setting.id == Latin1(kPersistCacheSettingId)
			|| setting.id == Latin1(kLoadCacheOnStartSettingId)
			|| setting.id == Latin1(kMaxCacheEntriesSettingId)
			|| setting.id == Latin1(kDeletedMarkSettingId)
			|| setting.id == Latin1(kEditedMarkSettingId)) {
			trimCache();
			saveCacheIfNeeded();
			return;
		}
		if (setting.id == Latin1(kFilterZalgoOutgoingSettingId)) {
			refreshOutgoingInterceptor();
			return;
		}
		if (setting.id == Latin1(kExportCacheSettingId)) {
			exportCacheToDisk();
			return;
		}
		if (setting.id == Latin1(kOpenCacheFolderSettingId)) {
			const auto baseDir = _host->pluginsPath();
			if (baseDir.isEmpty()
				|| !QDesktopServices::openUrl(QUrl::fromLocalFile(baseDir))) {
				_host->showToast(tr(
					"Could not open the plugins folder.",
					"Не удалось открыть папку плагинов."));
			}
			return;
		}
		if (setting.id == Latin1(kClearCacheSettingId)) {
			_cache.clear();
			saveCacheIfNeeded();
			_host->showToast(tr(
				"AyuSafe cache cleared.",
				"Кэш AyuSafe очищен."));
			return;
		}
	}

	void exportCacheToDisk() const {
		if (_cache.isEmpty()) {
			_host->showToast(tr(
				"AyuSafe cache is empty.",
				"Кэш AyuSafe пуст."));
			return;
		}
		const auto baseDir = _host->pluginsPath();
		if (baseDir.isEmpty()) {
			_host->showToast(tr(
				"Plugins path is unavailable.",
				"Путь к плагинам недоступен."));
			return;
		}
		QDir().mkpath(baseDir);
		const auto path = QDir(baseDir).filePath(QStringLiteral("ayu_safe_cache.json"));
		auto items = QJsonArray();
		for (const auto &entry : _cache) {
			items.push_back(QJsonObject{
				{ QStringLiteral("timestampUtc"), entry.timestampUtc },
				{ QStringLiteral("event"), entry.event },
				{ QStringLiteral("peerName"), entry.peerName },
				{ QStringLiteral("text"), entry.text },
				{ QStringLiteral("messageId"), QString::number(entry.messageId) },
				{ QStringLiteral("sessionUniqueId"), QString::number(entry.sessionUniqueId) },
				{ QStringLiteral("outgoing"), entry.outgoing },
			});
		}
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly)) {
			_host->showToast(tr(
				"Could not write AyuSafe cache file.",
				"Не удалось записать файл кэша AyuSafe."));
			return;
		}
		file.write(QJsonDocument(QJsonObject{
			{ QStringLiteral("version"), QStringLiteral("1") },
			{ QStringLiteral("entries"), items },
		}).toJson(QJsonDocument::Indented));
		if (!file.commit()) {
			_host->showToast(tr(
				"Could not save AyuSafe cache file.",
				"Не удалось сохранить файл кэша AyuSafe."));
			return;
		}
		_host->showToast(tr(
			"AyuSafe cache exported.",
			"Кэш AyuSafe экспортирован."));
	}

	Plugins::Host *_host = nullptr;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::MessageObserverId _observerId = 0;
	Plugins::OutgoingInterceptorId _outgoingInterceptorId = 0;
	Plugins::PluginInfo _info;
	QFont _baseFont;
	QString _baseStyleSheet;
	bool _capturedBaseVisuals = false;
	QVector<CacheEntry> _cache;
	QSet<QWidget*> _trackedWindows;
	QSet<QWidget*> _retitlePending;
	mutable QHash<QWidget*, QString> _originalWindowTitles;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AyuSafePlugin(host);
}
