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
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <cmath>

TGD_PLUGIN_PREVIEW(
	"astro.ayu_safe",
	"AyuSafe",
	"0.1",
	"Astrogram",
	"AyuSafe-lite: Ayu-inspired visuals plus local message safety tools.",
	"https://github.com/AyuGram/AyuGramDesktop",
	"GusTheDuck/7")

namespace {

constexpr auto kPluginId = "astro.ayu_safe";
constexpr auto kPluginVersion = "0.1";
constexpr int kDefaultFontScale = 100;
constexpr int kMinFontScale = 85;
constexpr int kMaxFontScale = 130;

constexpr auto kFontScaleSettingId = "font_scale";
constexpr auto kSoftChromeSettingId = "soft_chrome";
constexpr auto kCacheEnabledSettingId = "cache_enabled";
constexpr auto kCaptureEditsSettingId = "capture_edits";
constexpr auto kCaptureDeletesSettingId = "capture_deletes";
constexpr auto kExportCacheSettingId = "export_cache";
constexpr auto kClearCacheSettingId = "clear_cache";

QString Normalize(const QString &value) {
	auto result = value;
	result.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	result.replace(QChar::fromLatin1('\r'), QChar::fromLatin1('\n'));
	return result.trimmed();
}

template <typename HostInfoType>
QString HostUiLanguage(const HostInfoType &hostInfo) {
	if constexpr (requires(const HostInfoType &value) {
		value.appUiLanguage;
	}) {
		return hostInfo.appUiLanguage;
	} else {
		return QString();
	}
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

class AyuSafePlugin final : public Plugins::Plugin {
public:
	explicit AyuSafePlugin(Plugins::Host *host) : _host(host) {
		refreshInfo();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		refreshInfo();
		captureBaseVisualState();
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSettingChanged(setting);
			});
		applyVisualSettings();
		refreshObserver();
	}

	void onUnload() override {
		if (_observerId) {
			_host->unregisterMessageObserver(_observerId);
			_observerId = 0;
		}
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		restoreVisualSettings();
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
		_info.id = QStringLiteral(kPluginId);
		_info.name = QStringLiteral("AyuSafe");
		_info.version = QStringLiteral(kPluginVersion);
		_info.author = QStringLiteral("Astrogram");
		_info.description = tr(
			"AyuSafe-lite: Ayu-inspired visuals plus local message safety tools.",
			"AyuSafe-lite: визуальные фишки в стиле Ayu и локальные инструменты безопасности сообщений.");
		_info.website = QStringLiteral("https://github.com/AyuGram/AyuGramDesktop");
	}

	[[nodiscard]] int fontScalePercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				QStringLiteral(kFontScaleSettingId),
				kDefaultFontScale),
			kMinFontScale,
			kMaxFontScale);
	}

	[[nodiscard]] bool softChromeEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			QStringLiteral(kSoftChromeSettingId),
			true);
	}

	[[nodiscard]] bool cacheEnabled() const {
		return _host->settingBoolValue(
			_info.id,
			QStringLiteral(kCacheEnabledSettingId),
			true);
	}

	[[nodiscard]] bool captureEdits() const {
		return _host->settingBoolValue(
			_info.id,
			QStringLiteral(kCaptureEditsSettingId),
			true);
	}

	[[nodiscard]] bool captureDeletes() const {
		return _host->settingBoolValue(
			_info.id,
			QStringLiteral(kCaptureDeletesSettingId),
			true);
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
			entry.text = tr("[non-text or unavailable message]", "[нетекстовое или недоступное сообщение]");
		}
		_cache.push_back(std::move(entry));
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
		fontScale.id = QStringLiteral(kFontScaleSettingId);
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
		softChrome.id = QStringLiteral(kSoftChromeSettingId);
		softChrome.title = tr("Soft chrome", "Мягкий хром");
		softChrome.description = tr(
			"Adds softer rounded Qt controls and menu surfaces where the host allows it.",
			"Добавляет более мягкие скруглённые Qt-контролы и поверхности меню там, где это позволяет хост.");
		softChrome.type = Plugins::SettingControl::Toggle;
		softChrome.boolValue = softChromeEnabled();

		auto visuals = Plugins::SettingsSectionDescriptor();
		visuals.id = QStringLiteral("appearance");
		visuals.title = tr("Appearance", "Внешний вид");
		visuals.description = tr(
			"Portable Ayu-style tweaks that fit the current plugin API.",
			"Переносимые настройки в стиле Ayu, которые укладываются в текущий plugin API.");
		visuals.settings.push_back(fontScale);
		visuals.settings.push_back(softChrome);

		auto cacheToggle = Plugins::SettingDescriptor();
		cacheToggle.id = QStringLiteral(kCacheEnabledSettingId);
		cacheToggle.title = tr("Local message safety cache", "Локальный кэш безопасности сообщений");
		cacheToggle.description = tr(
			"Caches new messages locally so edited and deleted events are easier to inspect later.",
			"Локально кэширует новые сообщения, чтобы затем было проще разбирать edited/deleted события.");
		cacheToggle.type = Plugins::SettingControl::Toggle;
		cacheToggle.boolValue = cacheEnabled();

		auto edits = Plugins::SettingDescriptor();
		edits.id = QStringLiteral(kCaptureEditsSettingId);
		edits.title = tr("Track edits", "Отслеживать правки");
		edits.description = tr(
			"Adds edited-message events to the local AyuSafe cache.",
			"Добавляет события правки сообщений в локальный кэш AyuSafe.");
		edits.type = Plugins::SettingControl::Toggle;
		edits.boolValue = captureEdits();

		auto deletes = Plugins::SettingDescriptor();
		deletes.id = QStringLiteral(kCaptureDeletesSettingId);
		deletes.title = tr("Track deletes", "Отслеживать удаления");
		deletes.description = tr(
			"Adds deleted-message events to the local AyuSafe cache.",
			"Добавляет события удаления сообщений в локальный кэш AyuSafe.");
		deletes.type = Plugins::SettingControl::Toggle;
		deletes.boolValue = captureDeletes();

		auto exportCache = Plugins::SettingDescriptor();
		exportCache.id = QStringLiteral(kExportCacheSettingId);
		exportCache.title = tr("Export cache", "Экспортировать кэш");
		exportCache.description = tr(
			"Writes the current local cache to ayu_safe_cache.json in the plugins folder.",
			"Записывает текущий локальный кэш в ayu_safe_cache.json в папке плагинов.");
		exportCache.type = Plugins::SettingControl::ActionButton;
		exportCache.buttonText = tr("Export", "Экспорт");

		auto clearCache = Plugins::SettingDescriptor();
		clearCache.id = QStringLiteral(kClearCacheSettingId);
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
		safety.settings.push_back(exportCache);
		safety.settings.push_back(clearCache);

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("note");
		info.title = tr("Portable subset", "Переносимый набор");
		info.description = tr(
			"This plugin intentionally ships only the Ayu-style features that fit the current host API safely.",
			"Этот плагин специально включает только те Ayu-фишки, которые можно безопасно поднять на текущем host API.");
		info.type = Plugins::SettingControl::InfoText;

		auto notes = Plugins::SettingsSectionDescriptor();
		notes.id = QStringLiteral("notes");
		notes.title = tr("Notes", "Примечания");
		notes.settings.push_back(info);

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("ayu_safe");
		page.title = QStringLiteral("AyuSafe");
		page.description = tr(
			"Ayu-inspired appearance and safety features for Astrogram.",
			"Ayu-вдохновлённые визуальные и защитные функции для Astrogram.");
		page.sections.push_back(visuals);
		page.sections.push_back(safety);
		page.sections.push_back(notes);
		return page;
	}

	void handleSettingChanged(const Plugins::SettingDescriptor &setting) {
		if (setting.id == QStringLiteral(kFontScaleSettingId)
			|| setting.id == QStringLiteral(kSoftChromeSettingId)) {
			applyVisualSettings();
			return;
		}
		if (setting.id == QStringLiteral(kCacheEnabledSettingId)
			|| setting.id == QStringLiteral(kCaptureEditsSettingId)
			|| setting.id == QStringLiteral(kCaptureDeletesSettingId)) {
			refreshObserver();
			return;
		}
		if (setting.id == QStringLiteral(kExportCacheSettingId)) {
			exportCacheToDisk();
			return;
		}
		if (setting.id == QStringLiteral(kClearCacheSettingId)) {
			_cache.clear();
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
		file.write(QJsonDocument(items).toJson(QJsonDocument::Indented));
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
	Plugins::PluginInfo _info;
	QFont _baseFont;
	QString _baseStyleSheet;
	bool _capturedBaseVisuals = false;
	QVector<CacheEntry> _cache;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AyuSafePlugin(host);
}
