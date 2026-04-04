/*
Astrogram font tuning plugin.
Allows scale tuning and loading a custom font from file or URL.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>

TGD_PLUGIN_PREVIEW(
	"astro.font_tuner",
	"Font Tuner",
	"1.5",
	"@etopizdesblin",
	"Tunes Astrogram fonts, loads a custom font from file or URL, and applies the changes live.",
	"https://sosiskibot.ru",
	"GusTheDuck/11")

namespace {

constexpr auto kPluginId = "astro.font_tuner";
constexpr auto kScaleSettingId = "font_scale";
constexpr auto kUrlSettingId = "font_url";
constexpr auto kChooseFileSettingId = "choose_file";
constexpr auto kDownloadSettingId = "download_font";
constexpr auto kResetSettingId = "reset_font";
constexpr auto kInfoSettingId = "usage_info";

constexpr int kDefaultScalePercent = 100;
constexpr int kMinScalePercent = 70;
constexpr int kMaxScalePercent = 160;
constexpr int kDownloadTimeoutMs = 30000;

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
}

QString Utf8(const char *value) {
	return QString::fromUtf8(value);
}

QString Utf8(const char8_t *value) {
	return QString::fromUtf8(reinterpret_cast<const char*>(value));
}

bool UseRussian(const Plugins::Host *host) {
	auto language = host->hostInfo().appUiLanguage.trimmed();
	if (language.isEmpty()) {
		language = host->systemInfo().uiLanguage.trimmed();
	}
	return language.startsWith(QStringLiteral("ru"), Qt::CaseInsensitive);
}

QString Tr(const Plugins::Host *host, const char *en, const char8_t *ru) {
	return UseRussian(host) ? Utf8(ru) : Latin1(en);
}

QString Tr(const Plugins::Host *host, const char *en, const char *ru) {
	return UseRussian(host) ? Utf8(ru) : Latin1(en);
}

QString SafeFileStem(QString value) {
	value = value.trimmed();
	for (auto &ch : value) {
		if (!(ch.isLetterOrNumber() || ch == u'-' || ch == u'_' || ch == u'.')) {
			ch = u'_';
		}
	}
	return value.isEmpty() ? QStringLiteral("font") : value;
}

QString StorageRoot(const Plugins::Host *host) {
	const auto workingPath = host->hostInfo().workingPath.trimmed();
	return workingPath.isEmpty()
		? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
		: workingPath;
}

bool IsSupportedFontSuffix(QString suffix) {
	suffix = suffix.trimmed().toLower();
	return suffix == QStringLiteral("ttf")
		|| suffix == QStringLiteral("otf");
}

bool IsSupportedFontUrl(const QUrl &url) {
	const auto scheme = url.scheme().trimmed().toLower();
	return url.isValid()
		&& !url.host().trimmed().isEmpty()
		&& (scheme == QStringLiteral("http")
			|| scheme == QStringLiteral("https"));
}

} // namespace

class FontTunerPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit FontTunerPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host)
	, _network(new QNetworkAccessManager(this)) {
		_info.id = Latin1(kPluginId);
		_info.name = Tr(
			_host,
			"Font Tuner",
			u8"Тюнер шрифтов");
		_info.version = QStringLiteral("1.5");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = Tr(
			_host,
			"Tunes Astrogram fonts, loads a custom font from file or URL, and applies the changes live.",
			u8"Настраивает шрифты Astrogram, загружает пользовательский шрифт из файла или по ссылке и применяет изменения сразу.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_baseFont = QApplication::font();
		_scalePercent = readScalePercent();
		_fontUrl = readFontUrl();
		const auto storageRoot = StorageRoot(_host);
		_statePath = QDir(storageRoot).filePath(
			QStringLiteral("tdata/font_tuner_state.json"));
		_fontsDir = QDir(storageRoot).filePath(
			QStringLiteral("tdata/plugin-fonts/font_tuner"));
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_baseFont = QApplication::font();
		_scalePercent = readScalePercent();
		_fontUrl = readFontUrl();
		loadLocalState();
		applyConfiguredFont();

		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSetting(setting);
			});
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		if (_pendingReply) {
			_pendingReply->abort();
			_pendingReply->deleteLater();
			_pendingReply = nullptr;
		}
		unloadApplicationFont();
		QApplication::setFont(_baseFont);
	}

private:
	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto scale = Plugins::SettingDescriptor();
		scale.id = Latin1(kScaleSettingId);
		scale.title = Tr(_host, "Font scale", u8"Масштаб шрифта");
		scale.description = Tr(
			_host,
			"Scales the active Astrogram font without changing the chosen family.",
			u8"Меняет масштаб активного шрифта Astrogram, не меняя выбранное семейство.");
		scale.type = Plugins::SettingControl::IntSlider;
		scale.intValue = _scalePercent;
		scale.intMinimum = kMinScalePercent;
		scale.intMaximum = kMaxScalePercent;
		scale.intStep = 1;
		scale.valueSuffix = QStringLiteral("%");

		auto url = Plugins::SettingDescriptor();
		url.id = Latin1(kUrlSettingId);
		url.title = Tr(_host, "Font URL", u8"Ссылка на шрифт");
		url.description = Tr(
			_host,
			"Direct URL to a .ttf or .otf file. Use the download button below after pasting the link.",
			u8"Прямая ссылка на файл .ttf или .otf. После вставки нажми кнопку загрузки ниже.");
		url.type = Plugins::SettingControl::TextInput;
		url.textValue = _fontUrl;
		url.placeholderText = Tr(
			_host,
			"https://example.com/font.ttf",
			"https://example.com/font.ttf");

		auto chooseFile = Plugins::SettingDescriptor();
		chooseFile.id = Latin1(kChooseFileSettingId);
		chooseFile.title = Tr(_host, "Load font from file", u8"Загрузить шрифт из файла");
		chooseFile.description = Tr(
			_host,
			"Open a file picker and import a local .ttf/.otf into Astrogram.",
			u8"Открыть выбор файла и импортировать локальный .ttf/.otf в Astrogram.");
		chooseFile.type = Plugins::SettingControl::ActionButton;
		chooseFile.buttonText = Tr(_host, "Choose file", u8"Выбрать файл");

		auto download = Plugins::SettingDescriptor();
		download.id = Latin1(kDownloadSettingId);
		download.title = Tr(_host, "Download from URL", u8"Скачать по ссылке");
		download.description = Tr(
			_host,
			"Downloads the font from the URL above and applies it immediately.",
			u8"Скачивает шрифт по ссылке выше и сразу применяет его.");
		download.type = Plugins::SettingControl::ActionButton;
		download.buttonText = Tr(_host, "Download", u8"Скачать");

		auto reset = Plugins::SettingDescriptor();
		reset.id = Latin1(kResetSettingId);
		reset.title = Tr(_host, "Reset font", u8"Сбросить шрифт");
		reset.description = Tr(
			_host,
			"Return to the original Astrogram font while keeping the scale slider.",
			u8"Вернуть исходный шрифт Astrogram, сохранив ползунок масштаба.");
		reset.type = Plugins::SettingControl::ActionButton;
		reset.buttonText = Tr(_host, "Reset", u8"Сбросить");

		auto info = Plugins::SettingDescriptor();
		info.id = Latin1(kInfoSettingId);
		info.title = Tr(_host, "How it works", u8"Как это работает");
		info.description = Tr(
			_host,
			"The plugin stores the last imported font in tdata/plugin-fonts/font_tuner and reapplies it on restart.",
			u8"Плагин хранит последний импортированный шрифт в tdata/plugin-fonts/font_tuner и применяет его после перезапуска.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("font");
		section.title = Tr(_host, "Typography", u8"Типографика");
		section.settings = {
			scale,
			url,
			chooseFile,
			download,
			reset,
			info,
		};

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("font_tuner");
		page.title = Tr(_host, "Font Tuner", u8"Тюнер шрифтов");
		page.description = Tr(
			_host,
			"Custom fonts and scale for Astrogram without rebuilding the client.",
			u8"Пользовательские шрифты и масштаб для Astrogram без пересборки клиента.");
		page.sections.push_back(section);
		return page;
	}

	void handleSetting(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kScaleSettingId)) {
			_scalePercent = std::clamp(
				setting.intValue,
				kMinScalePercent,
				kMaxScalePercent);
			applyConfiguredFont();
			return;
		}
		if (setting.id == Latin1(kUrlSettingId)) {
			_fontUrl = setting.textValue.trimmed();
			return;
		}
		if (setting.id == Latin1(kChooseFileSettingId)) {
			scheduleChooseFontFile();
			return;
		}
		if (setting.id == Latin1(kDownloadSettingId)) {
			downloadFont();
			return;
		}
		if (setting.id == Latin1(kResetSettingId)) {
			resetFont();
		}
	}

	int readScalePercent() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id,
				Latin1(kScaleSettingId),
				kDefaultScalePercent),
			kMinScalePercent,
			kMaxScalePercent);
	}

	QString readFontUrl() const {
		return _host->settingStringValue(
			_info.id,
			Latin1(kUrlSettingId),
			QString());
	}

	void scheduleChooseFontFile() {
		if (_chooseFileScheduled) {
			return;
		}
		_chooseFileScheduled = true;
		QTimer::singleShot(0, this, [this] {
			_chooseFileScheduled = false;
			chooseFontFileNow();
		});
	}

	void chooseFontFileNow() {
		auto *parent = _host->activeWindowWidget();
		const auto path = QFileDialog::getOpenFileName(
			parent,
			Tr(_host, "Choose font file", u8"Выбери файл шрифта"),
			QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
			Tr(
				_host,
				"Fonts (*.ttf *.otf);;All files (*.*)",
				u8"Шрифты (*.ttf *.otf);;Все файлы (*.*)"));
		if (path.isEmpty()) {
			return;
		}
		importFontFile(path);
	}

	void downloadFont() {
		const auto trimmed = _fontUrl.trimmed();
		const auto url = QUrl(trimmed);
		if (trimmed.isEmpty() || !IsSupportedFontUrl(url)) {
			_host->showToast(Tr(
				_host,
				"Paste a direct http(s) URL to a .ttf or .otf font first.",
				u8"Сначала вставь прямую http(s)-ссылку на шрифт .ttf или .otf."));
			return;
		}
		if (_pendingReply) {
			_pendingReply->abort();
			_pendingReply->deleteLater();
			_pendingReply = nullptr;
		}
		auto request = QNetworkRequest(url);
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
		request.setTransferTimeout(kDownloadTimeoutMs);
		_pendingReply = _network->get(request);
		QObject::connect(
			_pendingReply,
			&QNetworkReply::finished,
			this,
			[this, url] {
				auto reply = _pendingReply;
				_pendingReply = nullptr;
				if (!reply) {
					return;
				}
				reply->deleteLater();
				const auto statusCode = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				if (reply->error() != QNetworkReply::NoError || statusCode >= 400) {
					_host->showToast(Tr(
						_host,
						"Could not download the font.",
						u8"Не удалось скачать шрифт."));
					return;
				}
				const auto bytes = reply->readAll();
				if (bytes.isEmpty()) {
					_host->showToast(Tr(
						_host,
						"Downloaded font is empty.",
						u8"Скачанный шрифт пустой."));
					return;
				}
				QDir().mkpath(_fontsDir);
				const auto suffix = QFileInfo(url.path()).suffix().trimmed().toLower();
				const auto fileName = SafeFileStem(
					QFileInfo(url.path()).completeBaseName().trimmed().isEmpty()
						? QStringLiteral("downloaded_font")
						: QFileInfo(url.path()).completeBaseName())
					+ QStringLiteral(".")
					+ (suffix.isEmpty() ? QStringLiteral("ttf") : suffix);
				const auto target = QDir(_fontsDir).filePath(fileName);
				QFile output(target);
				if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					_host->showToast(Tr(
						_host,
						"Could not save the downloaded font.",
						u8"Не удалось сохранить скачанный шрифт."));
					return;
				}
				output.write(bytes);
				output.close();
				loadFontFromPath(target);
			});
		_host->showToast(Tr(
			_host,
			"Downloading font...",
			u8"Скачиваю шрифт..."));
	}

	void importFontFile(const QString &path) {
		const auto sourceInfo = QFileInfo(path);
		if (!sourceInfo.exists() || !sourceInfo.isFile()) {
			_host->showToast(Tr(
				_host,
				"The selected font file could not be found.",
				u8"Не удалось найти выбранный файл шрифта."));
			return;
		}
		if (!IsSupportedFontSuffix(sourceInfo.suffix())) {
			_host->showToast(Tr(
				_host,
				"Choose a .ttf or .otf font file.",
				u8"Выбери файл шрифта в формате .ttf или .otf."));
			return;
		}
		QDir().mkpath(_fontsDir);
		const auto target = QDir(_fontsDir).filePath(
			SafeFileStem(sourceInfo.fileName()));
		if (sourceInfo.absoluteFilePath() == QFileInfo(target).absoluteFilePath()) {
			loadFontFromPath(target);
			return;
		}
		if (QFile::exists(target) && !QFile::remove(target)) {
			_host->showToast(Tr(
				_host,
				"Could not replace the previous imported font file.",
				u8"Не удалось заменить ранее импортированный файл шрифта."));
			return;
		}
		if (!QFile::copy(path, target)) {
			_host->showToast(Tr(
				_host,
				"Could not import the selected font file.",
				u8"Не удалось импортировать выбранный файл шрифта."));
			return;
		}
		loadFontFromPath(target);
	}

	void loadFontFromPath(const QString &path) {
		const auto fileInfo = QFileInfo(path);
		if (!fileInfo.exists() || !fileInfo.isFile()) {
			_host->showToast(Tr(
				_host,
				"The selected font file no longer exists.",
				u8"Выбранный файл шрифта больше не существует."));
			return;
		}
		unloadApplicationFont();
		const auto fontId = QFontDatabase::addApplicationFont(
			fileInfo.absoluteFilePath());
		if (fontId < 0) {
			_host->showToast(Tr(
				_host,
				"Qt could not load this font file.",
				u8"Qt не смог загрузить этот файл шрифта."));
			return;
		}
		const auto families = QFontDatabase::applicationFontFamilies(fontId);
		if (families.empty()) {
			QFontDatabase::removeApplicationFont(fontId);
			_host->showToast(Tr(
				_host,
				"The font loaded but exposed no families.",
				u8"Шрифт загрузился, но не дал ни одного семейства."));
			return;
		}
		_applicationFontId = fontId;
		_loadedFontPath = fileInfo.absoluteFilePath();
		_loadedFamily = families.front();
		saveLocalState();
		applyConfiguredFont();
		_host->showToast(Tr(
			_host,
			"Custom font applied.",
			u8"Пользовательский шрифт применён."));
	}

	void applyConfiguredFont() {
		auto font = _baseFont;
		if (!_loadedFamily.isEmpty()) {
			font.setFamily(_loadedFamily);
		}
		if (font.pointSizeF() > 0) {
			font.setPointSizeF(font.pointSizeF() * _scalePercent / 100.0);
		} else if (font.pixelSize() > 0) {
			font.setPixelSize(std::max(
				1,
				int(std::lround(font.pixelSize() * _scalePercent / 100.0))));
		}
		QApplication::setFont(font);
		for (auto *widget : QApplication::allWidgets()) {
			if (widget) {
				widget->updateGeometry();
				widget->update();
			}
		}
	}

	void resetFont() {
		unloadApplicationFont();
		_loadedFontPath.clear();
		_loadedFamily.clear();
		saveLocalState();
		applyConfiguredFont();
		_host->showToast(Tr(
			_host,
			"Original Astrogram font restored.",
			u8"Исходный шрифт Astrogram восстановлен."));
	}

	void unloadApplicationFont() {
		if (_applicationFontId >= 0) {
			QFontDatabase::removeApplicationFont(_applicationFontId);
			_applicationFontId = -1;
		}
	}

	void loadLocalState() {
		QFile file(_statePath);
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		const auto doc = QJsonDocument::fromJson(file.readAll());
		file.close();
		const auto object = doc.object();
		const auto path = object.value(QStringLiteral("fontPath")).toString().trimmed();
		if (path.isEmpty() || !QFileInfo::exists(path)) {
			return;
		}
		loadFontFromPath(path);
	}

	void saveLocalState() const {
		QDir().mkpath(QFileInfo(_statePath).absolutePath());
		QFile file(_statePath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return;
		}
		QJsonObject object;
		object.insert(QStringLiteral("fontPath"), _loadedFontPath);
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.close();
	}

	Plugins::Host *_host = nullptr;
	QNetworkAccessManager *_network = nullptr;
	QPointer<QNetworkReply> _pendingReply;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	QFont _baseFont;
	QString _statePath;
	QString _fontsDir;
	QString _fontUrl;
	QString _loadedFontPath;
	QString _loadedFamily;
	int _applicationFontId = -1;
	int _scalePercent = kDefaultScalePercent;
	bool _chooseFileScheduled = false;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new FontTunerPlugin(host);
}
