/*
Astrogram font tuning plugin.
Allows scale tuning and loading a custom font from a local file.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>

TGD_PLUGIN_PREVIEW(
	"astro.font_tuner",
	"Font Tuner",
	"1.8",
	"@etopizdesblin",
	"Tunes Astrogram fonts, loads a custom font from file, and applies the changes live.",
	"https://sosiskibot.ru",
	"GusTheDuck/11")

namespace {

constexpr auto kPluginId = "astro.font_tuner";
constexpr auto kScaleSettingId = "font_scale";
constexpr auto kChooseFileSettingId = "choose_file";
constexpr auto kResetSettingId = "reset_font";
constexpr auto kInfoSettingId = "usage_info";

constexpr int kDefaultScalePercent = 100;
constexpr int kMinScalePercent = 70;
constexpr int kMaxScalePercent = 160;

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

} // namespace

class FontTunerPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit FontTunerPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host) {
		_info.id = Latin1(kPluginId);
		_info.name = Tr(
			_host,
			"Font Tuner",
			u8"Тюнер шрифтов");
		_info.version = QStringLiteral("1.8");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = Tr(
			_host,
			"Tunes Astrogram fonts, loads a custom font from file, and applies the changes live.",
			u8"Настраивает шрифты Astrogram, загружает пользовательский шрифт из файла и применяет изменения сразу.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_baseFont = QApplication::font();
		_scalePercent = readScalePercent();
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
		if (_fileDialog) {
			_fileDialog->close();
			_fileDialog = nullptr;
		}
		unloadApplicationFont();
		QApplication::setFont(_baseFont);
		auto fontChangeEvent = QEvent(QEvent::ApplicationFontChange);
		for (auto *widget : QApplication::allWidgets()) {
			if (widget) {
				QCoreApplication::sendEvent(widget, &fontChangeEvent);
				widget->updateGeometry();
				widget->update();
			}
		}
	}

private:
	enum class LoadMode {
		UserSelection,
		SilentRestore,
	};

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

		auto chooseFile = Plugins::SettingDescriptor();
		chooseFile.id = Latin1(kChooseFileSettingId);
		chooseFile.title = Tr(_host, "Load font from file", u8"Загрузить шрифт из файла");
		chooseFile.description = Tr(
			_host,
			"Open a file picker and import a local .ttf/.otf into Astrogram.",
			u8"Открыть выбор файла и импортировать локальный .ttf/.otf в Astrogram.");
		chooseFile.type = Plugins::SettingControl::ActionButton;
		chooseFile.buttonText = Tr(_host, "Choose file", u8"Выбрать файл");

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
		info.title = Tr(_host, "Current behavior", u8"Текущее поведение");
		info.description = Tr(
			_host,
			"The plugin stores the last imported local font in tdata/plugin-fonts/font_tuner and reapplies it on restart. Supported formats: .ttf and .otf.",
			u8"Плагин хранит последний локально импортированный шрифт в tdata/plugin-fonts/font_tuner и применяет его после перезапуска. Поддерживаются форматы .ttf и .otf.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("font");
		section.title = Tr(_host, "Typography", u8"Типографика");
		section.settings = {
			scale,
			chooseFile,
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
		if (setting.id == Latin1(kChooseFileSettingId)) {
			scheduleChooseFontFile();
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

	void scheduleChooseFontFile() {
		if (_chooseFileScheduled) {
			return;
		}
		_chooseFileScheduled = true;
		QTimer::singleShot(120, this, [this] {
			_chooseFileScheduled = false;
			chooseFontFileNow();
		});
	}

	QWidget *resolveDialogParent() const {
		auto accept = [](QWidget *candidate) -> QWidget* {
			if (!candidate) {
				return nullptr;
			}
			auto *window = candidate->window();
			if (!window || !window->isWindow() || window->parentWidget()) {
				return nullptr;
			}
			if (!window->isVisible()
				|| !window->testAttribute(Qt::WA_WState_Created)
				|| window->testAttribute(Qt::WA_DontShowOnScreen)) {
				return nullptr;
			}
			return window;
		};
		if (auto *window = accept(_host->activeWindowWidget())) {
			return window;
		}
		auto *fallback = static_cast<QWidget*>(nullptr);
		_host->forEachWindowWidget([&](QWidget *widget) {
			if (!fallback) {
				fallback = accept(widget);
			}
		});
		if (fallback) {
			return fallback;
		}
		if (auto *window = accept(QApplication::activeWindow())) {
			return window;
		}
		for (auto *widget : QApplication::topLevelWidgets()) {
			if (auto *window = accept(widget)) {
				return window;
			}
		}
		return nullptr;
	}

	void chooseFontFileNow() {
		if (_fileDialog) {
			_fileDialog->raise();
			_fileDialog->activateWindow();
			return;
		}
		auto *parent = resolveDialogParent();
		const auto startDirectory = QFileInfo(_loadedFontPath).exists()
			? QFileInfo(_loadedFontPath).absolutePath()
			: QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
		auto *dialog = new QFileDialog(
			parent,
			Tr(_host, "Choose font file", u8"Выберите файл шрифта"),
			startDirectory,
			Tr(
				_host,
				"Fonts (*.ttf *.otf);;All files (*.*)",
				u8"Шрифты (*.ttf *.otf);;Все файлы (*.*)"));
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setFileMode(QFileDialog::ExistingFile);
		dialog->setAcceptMode(QFileDialog::AcceptOpen);
		dialog->setOption(QFileDialog::ReadOnly, true);
		dialog->setOption(QFileDialog::DontUseNativeDialog, true);
		dialog->setWindowModality(Qt::WindowModal);
		_fileDialog = dialog;

		QObject::connect(dialog, &QFileDialog::fileSelected, this, [this](const QString &path) {
			importFontFile(path);
		});
		QObject::connect(dialog, &QObject::destroyed, this, [this] {
			_fileDialog = nullptr;
		});

		dialog->open();
		dialog->raise();
		dialog->activateWindow();
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
			loadFontFromPath(target, LoadMode::UserSelection);
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
		loadFontFromPath(target, LoadMode::UserSelection);
	}

	bool loadFontFromPath(const QString &path, LoadMode mode) {
		const auto notifyUser = (mode == LoadMode::UserSelection);
		const auto fileInfo = QFileInfo(path);
		if (!fileInfo.exists() || !fileInfo.isFile()) {
			if (notifyUser) {
				_host->showToast(Tr(
					_host,
					"The selected font file no longer exists.",
					u8"Выбранный файл шрифта больше не существует."));
			}
			return false;
		}
		unloadApplicationFont();
		const auto fontId = QFontDatabase::addApplicationFont(
			fileInfo.absoluteFilePath());
		if (fontId < 0) {
			if (notifyUser) {
				_host->showToast(Tr(
					_host,
					"Qt could not load this font file.",
					u8"Qt не смог загрузить этот файл шрифта."));
			}
			return false;
		}
		const auto families = QFontDatabase::applicationFontFamilies(fontId);
		if (families.empty()) {
			QFontDatabase::removeApplicationFont(fontId);
			if (notifyUser) {
				_host->showToast(Tr(
					_host,
					"The font loaded but exposed no families.",
					u8"Шрифт загрузился, но не дал ни одного семейства."));
			}
			return false;
		}
		_applicationFontId = fontId;
		_loadedFontPath = fileInfo.absoluteFilePath();
		_loadedFamily = families.front();
		if (mode == LoadMode::UserSelection) {
			saveLocalState();
		}
		applyConfiguredFont();
		if (notifyUser) {
			_host->showToast(Tr(
				_host,
				"Custom font applied.",
				u8"Пользовательский шрифт применён."));
		}
		return true;
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
		auto fontChangeEvent = QEvent(QEvent::ApplicationFontChange);
		for (auto *widget : QApplication::allWidgets()) {
			if (widget) {
				QCoreApplication::sendEvent(widget, &fontChangeEvent);
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
		loadFontFromPath(path, LoadMode::SilentRestore);
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
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	QFont _baseFont;
	QString _statePath;
	QString _fontsDir;
	QString _loadedFontPath;
	QString _loadedFamily;
	int _applicationFontId = -1;
	int _scalePercent = kDefaultScalePercent;
	bool _chooseFileScheduled = false;
	QPointer<QFileDialog> _fileDialog;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new FontTunerPlugin(host);
}
