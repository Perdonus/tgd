/*
Astrogram Show Logs plugin.
Adds a side-menu action that opens a semi-transparent overlay with plugin logs.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QHideEvent>
#include <QtGui/QScreen>
#include <QtGui/QShowEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <functional>
#include <utility>

TGD_PLUGIN_PREVIEW(
	"astro.show_logs",
	"Show Logs",
	"1.3",
	"@etopizdesblin",
	"Shows plugin logs in a semi-transparent overlay with filtering, copy and clear actions.",
	"https://sosiskibot.ru",
	"GusTheDuck/24")

namespace {

constexpr auto kPluginId = "astro.show_logs";
constexpr auto kPluginVersion = "1.3";
constexpr auto kPluginAuthor = "@etopizdesblin";
constexpr auto kMaxLinesSettingId = "max_lines";
constexpr auto kOpenOverlaySettingId = "open_overlay";
constexpr auto kInfoSettingId = "log_info";

constexpr int kDefaultMaxLines = 300;
constexpr int kMinMaxLines = 50;
constexpr int kMaxMaxLines = 2000;
constexpr int kRefreshIntervalMs = 800;

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

QString WorkingRoot(const Plugins::Host *host) {
	const auto path = host->hostInfo().workingPath.trimmed();
	return path.isEmpty()
		? QDir::currentPath()
		: path;
}

QString PluginLogsPath(const Plugins::Host *host) {
	return QDir(WorkingRoot(host)).filePath(QStringLiteral("tdata/plugins.log"));
}

QString ClientLogsPath(const Plugins::Host *host) {
	return QDir(WorkingRoot(host)).filePath(QStringLiteral("tdata/client.log"));
}

QStringList AvailableLogPaths(const Plugins::Host *host) {
	auto result = QStringList{
		ClientLogsPath(host),
		PluginLogsPath(host),
	};
	result.erase(
		std::remove_if(result.begin(), result.end(), [](const QString &path) {
			return path.trimmed().isEmpty();
		}),
		result.end());
	result.removeDuplicates();
	return result;
}

QStringList TailLines(const QString &path, int wantedLines) {
	if (wantedLines <= 0) {
		return {};
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	constexpr qint64 kBlockSize = 4096;
	auto buffer = QByteArray();
	auto remaining = file.size();
	auto lineCount = 0;
	while (remaining > 0 && lineCount <= wantedLines + 8) {
		const auto take = std::min<qint64>(kBlockSize, remaining);
		remaining -= take;
		file.seek(remaining);
		buffer.prepend(file.read(take));
		lineCount = buffer.count('\n');
	}
	auto text = QString::fromUtf8(buffer);
	auto lines = text.split(QChar::fromLatin1('\n'), Qt::SkipEmptyParts);
	for (auto &line : lines) {
		line = line.trimmed();
	}
	lines.erase(
		std::remove_if(lines.begin(), lines.end(), [](const QString &line) {
			return line.isEmpty();
		}),
		lines.end());
	if (lines.size() > wantedLines) {
		lines = lines.mid(lines.size() - wantedLines);
	}
	return lines;
}

QString PathShortLabel(const QString &path) {
	const auto name = QFileInfo(path).fileName().trimmed();
	return name.isEmpty() ? path : name;
}

QStringList TailMergedLines(const Plugins::Host *host, int wantedLines) {
	auto result = QStringList();
	const auto paths = AvailableLogPaths(host);
	const auto perFile = std::max(wantedLines, 1);
	for (const auto &path : paths) {
		const auto prefix = QStringLiteral("[%1] ").arg(PathShortLabel(path));
		for (const auto &line : TailLines(path, perFile)) {
			result.push_back(prefix + line);
		}
	}
	if (result.size() > wantedLines) {
		result = result.mid(result.size() - wantedLines);
	}
	return result;
}

QStringList ApplyFilter(QStringList lines, const QString &filter) {
	const auto trimmed = filter.trimmed();
	if (trimmed.isEmpty()) {
		return lines;
	}
	const auto parts = trimmed.split(QChar::fromLatin1(' '), Qt::SkipEmptyParts);
	lines.erase(
		std::remove_if(lines.begin(), lines.end(), [&](const QString &line) {
			for (const auto &part : parts) {
				if (!line.contains(part, Qt::CaseInsensitive)) {
					return true;
				}
			}
			return false;
		}),
		lines.end());
	return lines;
}

void CenterOver(QWidget *anchor, QWidget *widget) {
	if (!widget) {
		return;
	}
	auto geometry = QRect();
	if (anchor) {
		geometry = anchor->frameGeometry();
	} else if (const auto screen = QApplication::primaryScreen()) {
		geometry = screen->availableGeometry();
	}
	if (!geometry.isValid()) {
		geometry = QRect(0, 0, 960, 720);
	}
	const auto size = widget->size();
	widget->move(
		geometry.center().x() - (size.width() / 2),
		geometry.center().y() - (size.height() / 2));
}

QWidget *StableAnchorWindow(QWidget *candidate) {
	auto *window = candidate ? candidate->window() : nullptr;
	if (!window || !window->isWindow() || window->parentWidget()) {
		return nullptr;
	}
	const auto type = window->windowType();
	if (type == Qt::Dialog
		|| type == Qt::Popup
		|| type == Qt::Tool
		|| type == Qt::ToolTip
		|| type == Qt::Sheet
		|| type == Qt::Drawer
		|| type == Qt::SplashScreen
		|| type == Qt::SubWindow) {
		return nullptr;
	}
	if (!window->testAttribute(Qt::WA_WState_Created)
		|| window->testAttribute(Qt::WA_DontShowOnScreen)
		|| !window->isVisible()) {
		return nullptr;
	}
	return window;
}

class LogsOverlay final : public QDialog {
public:
	using ReadHandler = std::function<QStringList(int, const QString &)>;
	using ClearHandler = std::function<bool()>;
	using ToastHandler = std::function<void(const QString &)>;

	LogsOverlay(
		const Plugins::Host *host,
		ReadHandler readHandler,
		ClearHandler clearHandler,
		ToastHandler toastHandler,
		QWidget *anchor)
		: QDialog(
			nullptr,
			Qt::Dialog
				| Qt::Tool
				| Qt::FramelessWindowHint
				| Qt::NoDropShadowWindowHint)
	, _host(host)
	, _readHandler(std::move(readHandler))
	, _clearHandler(std::move(clearHandler))
	, _toastHandler(std::move(toastHandler))
	, _anchor(anchor) {
		setModal(false);
		setAttribute(Qt::WA_DeleteOnClose, false);
		setAttribute(Qt::WA_QuitOnClose, false);
		setAttribute(Qt::WA_TranslucentBackground, true);
		setAttribute(Qt::WA_StyledBackground, true);
		resize(820, 580);
		setObjectName(QStringLiteral("showLogsOverlay"));
		setStyleSheet(QStringLiteral(
			"#showLogsOverlay {"
			" background-color: rgba(12, 14, 18, 220);"
			" border: 1px solid rgba(255,255,255,24);"
			" border-radius: 18px;"
			"}"
			"QLabel { color: #f4ebe3; }"
			"QLineEdit, QPlainTextEdit {"
			" background-color: rgba(255,255,255,18);"
			" color: #f4ebe3;"
			" border: 1px solid rgba(255,255,255,24);"
			" border-radius: 12px;"
			" padding: 8px 10px;"
			"}"
			"QPushButton {"
			" background-color: rgba(255,106,61,40);"
			" color: #fff7f0;"
			" border: 1px solid rgba(255,106,61,72);"
			" border-radius: 10px;"
			" padding: 7px 12px;"
			"}"
			"QPushButton:hover {"
			" background-color: rgba(255,106,61,72);"
			"}"));

		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(18, 18, 18, 18);
		layout->setSpacing(12);

		auto headerLayout = new QHBoxLayout();
		headerLayout->setSpacing(10);
		auto title = new QLabel(
			Tr(_host, "Plugin Logs", u8"Логи плагинов"),
			this);
		title->setStyleSheet(QStringLiteral(
			"font-size: 18px; font-weight: 700; color: #fff7f0;"));
		headerLayout->addWidget(title);
		headerLayout->addStretch(1);

		auto copyButton = new QPushButton(
			Tr(_host, "Copy", u8"Скопировать"),
			this);
		auto clearButton = new QPushButton(
			Tr(_host, "Clear", u8"Очистить"),
			this);
		auto closeButton = new QPushButton(
			Tr(_host, "Close", u8"Закрыть"),
			this);
		headerLayout->addWidget(copyButton);
		headerLayout->addWidget(clearButton);
		headerLayout->addWidget(closeButton);
		layout->addLayout(headerLayout);

		auto filterLayout = new QHBoxLayout();
		filterLayout->setSpacing(10);
		_filterEdit = new QLineEdit(this);
		_filterEdit->setPlaceholderText(
			Tr(
				_host,
				"Filter by plugin id or name",
				u8"Фильтр по id или имени плагина"));
		auto applyFilterButton = new QPushButton(
			Tr(_host, "Filter", u8"Фильтр"),
			this);
		filterLayout->addWidget(_filterEdit, 1);
		filterLayout->addWidget(applyFilterButton);
		layout->addLayout(filterLayout);

		_statusLabel = new QLabel(this);
		_statusLabel->setStyleSheet(QStringLiteral("color: rgba(244,235,227,0.72);"));
		layout->addWidget(_statusLabel);

		_view = new QPlainTextEdit(this);
		_view->setReadOnly(true);
		_view->setLineWrapMode(QPlainTextEdit::NoWrap);
		_view->setPlaceholderText(
			Tr(
				_host,
				"Plugin logs will appear here.",
				u8"Здесь появятся логи плагинов."));
		layout->addWidget(_view, 1);

		connect(copyButton, &QPushButton::clicked, this, [this] {
			if (auto *clipboard = QGuiApplication::clipboard()) {
				clipboard->setText(_view->toPlainText());
			}
			_toastHandler(Tr(_host, "Logs copied.", u8"Логи скопированы."));
		});
		connect(clearButton, &QPushButton::clicked, this, [this] {
			if (_clearHandler()) {
				refreshNow();
				_toastHandler(Tr(_host, "Logs cleared.", u8"Логи очищены."));
			} else {
				_toastHandler(
					Tr(
						_host,
						"Could not clear plugin logs.",
						u8"Не удалось очистить логи плагинов."));
			}
		});
		connect(closeButton, &QPushButton::clicked, this, [this] {
			close();
		});
		connect(applyFilterButton, &QPushButton::clicked, this, [this] {
			_filter = _filterEdit->text().trimmed();
			refreshNow();
		});
		connect(_filterEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
			_filter = text.trimmed();
			refreshNow();
		});
		connect(_filterEdit, &QLineEdit::returnPressed, this, [this] {
			_filter = _filterEdit->text().trimmed();
			refreshNow();
		});
		connect(&_refreshTimer, &QTimer::timeout, this, [this] {
			refreshNow();
		});
	}

	void setMaxLines(int maxLines) {
		_maxLines = std::clamp(maxLines, kMinMaxLines, kMaxMaxLines);
		refreshNow();
	}

	void setAnchor(QWidget *anchor) {
		_anchor = StableAnchorWindow(anchor);
		CenterOver(_anchor.data(), this);
	}

	void refreshNow() {
		const auto fetchCount = std::max(_maxLines * 6, 240);
		auto lines = _readHandler(fetchCount, _filter);
		auto sourceLabels = QStringList();
		for (const auto &path : AvailableLogPaths(_host)) {
			sourceLabels.push_back(PathShortLabel(path));
		}
		if (lines.size() > _maxLines) {
			lines = lines.mid(lines.size() - _maxLines);
		}
		_view->setPlainText(lines.join(QChar::fromLatin1('\n')));
		_statusLabel->setText(
			Tr(_host, "Shown lines:", u8"Показано строк:")
			+ QStringLiteral(" %1 • ").arg(lines.size())
			+ Tr(_host, "Sources:", u8"Источники:")
			+ QStringLiteral(" %1").arg(sourceLabels.join(QStringLiteral(", "))));
		CenterOver(_anchor.data(), this);
	}

protected:
	void showEvent(QShowEvent *event) override {
		QDialog::showEvent(event);
		refreshNow();
		_refreshTimer.start(kRefreshIntervalMs);
		CenterOver(_anchor.data(), this);
	}

	void hideEvent(QHideEvent *event) override {
		_refreshTimer.stop();
		QDialog::hideEvent(event);
	}

private:
	const Plugins::Host *_host = nullptr;
	ReadHandler _readHandler;
	ClearHandler _clearHandler;
	ToastHandler _toastHandler;
	QPointer<QWidget> _anchor;
	QLineEdit *_filterEdit = nullptr;
	QLabel *_statusLabel = nullptr;
	QPlainTextEdit *_view = nullptr;
	QTimer _refreshTimer;
	QString _filter;
	int _maxLines = kDefaultMaxLines;
};

class ShowLogsPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit ShowLogsPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host) {
		refreshInfo();
		_maxLines = readMaxLines();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		refreshInfo();
		_maxLines = readMaxLines();
		_actionId = _host->registerActionWithContext(
			_info.id,
			Tr(_host, "Show Logs", u8"Показать логи"),
			Tr(
				_host,
				"Open the Astrogram plugin log overlay.",
				u8"Открыть overlay-окно с логами плагинов Astrogram."),
			[this](const Plugins::ActionContext &context) {
				Q_UNUSED(context);
				scheduleShowOverlay();
			});
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSetting(setting);
			});
	}

	void onUnload() override {
		_overlayScheduled = false;
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		if (_actionId) {
			_host->unregisterAction(_actionId);
			_actionId = 0;
		}
		if (_overlay) {
			_overlay->close();
			_overlay->deleteLater();
			_overlay = nullptr;
		}
	}

private:
	void refreshInfo() {
		_info.id = Latin1(kPluginId);
		_info.name = Tr(_host, "Show Logs", u8"Показать логи");
		_info.version = Latin1(kPluginVersion);
		_info.author = Latin1(kPluginAuthor);
		_info.description = Tr(
			_host,
			"Opens a semi-transparent overlay with client and plugin logs, filtering and copy/clear actions.",
			u8"Открывает полупрозрачное overlay-окно с клиентскими логами и логами плагинов, фильтром и кнопками копирования/очистки.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
	}

	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto maxLines = Plugins::SettingDescriptor();
		maxLines.id = Latin1(kMaxLinesSettingId);
		maxLines.title = Tr(_host, "Visible lines", u8"Количество строк");
		maxLines.description = Tr(
			_host,
			"How many log lines the overlay should show at the same time.",
			u8"Сколько строк логов overlay-окно показывает одновременно.");
		maxLines.type = Plugins::SettingControl::IntSlider;
		maxLines.intValue = _maxLines;
		maxLines.intMinimum = kMinMaxLines;
		maxLines.intMaximum = kMaxMaxLines;
		maxLines.intStep = 10;

		auto open = Plugins::SettingDescriptor();
		open.id = Latin1(kOpenOverlaySettingId);
		open.title = Tr(_host, "Open overlay", u8"Открыть overlay");
		open.description = Tr(
			_host,
			"Shows the log overlay without leaving settings.",
			u8"Показывает overlay-окно с логами прямо из настроек.");
		open.type = Plugins::SettingControl::ActionButton;
		open.buttonText = Tr(_host, "Show Logs", u8"Показать логи");

		auto info = Plugins::SettingDescriptor();
		info.id = Latin1(kInfoSettingId);
		info.title = Tr(_host, "Source", u8"Источник");
		info.description = Tr(
			_host,
			"Reads tdata/client.log and tdata/plugins.log. Use the filter field inside the overlay to show only one plugin or subsystem.",
			u8"Читает tdata/client.log и tdata/plugins.log. Фильтр внутри overlay позволяет оставить только один плагин или подсистему.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("logs");
		section.title = Tr(_host, "Overlay", u8"Overlay");
		section.settings = { maxLines, open, info };

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("show_logs");
		page.title = Tr(_host, "Show Logs", u8"Показать логи");
		page.description = Tr(
			_host,
			"Quick log overlay for plugin debugging.",
			u8"Быстрый overlay для отладки плагинов.");
		page.sections = { section };
		return page;
	}

	int readMaxLines() const {
		return std::clamp(
			_host->settingIntValue(
				_info.id.isEmpty() ? Latin1(kPluginId) : _info.id,
				Latin1(kMaxLinesSettingId),
				kDefaultMaxLines),
			kMinMaxLines,
			kMaxMaxLines);
	}

	QStringList readLogs(int fetchCount, const QString &filter) const {
		return ApplyFilter(TailMergedLines(_host, fetchCount), filter);
	}

	bool clearLogs() const {
		auto clearedAny = false;
		for (const auto &path : AvailableLogPaths(_host)) {
			QDir().mkpath(QFileInfo(path).absolutePath());
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				continue;
			}
			file.close();
			clearedAny = true;
		}
		return clearedAny;
	}

	QWidget *resolveOverlayAnchor() const {
		if (auto *anchor = StableAnchorWindow(_host->activeWindowWidget())) {
			return anchor;
		}
		auto *fallback = static_cast<QWidget*>(nullptr);
		_host->forEachWindowWidget([&](QWidget *widget) {
			if (!fallback) {
				fallback = StableAnchorWindow(widget);
			}
		});
		if (fallback) {
			return fallback;
		}
		if (auto *active = StableAnchorWindow(QApplication::activeWindow())) {
			return active;
		}
		for (auto *widget : QApplication::topLevelWidgets()) {
			if (auto *stable = StableAnchorWindow(widget)) {
				return stable;
			}
		}
		return nullptr;
	}

	void scheduleShowOverlay() {
		if (_overlayScheduled) {
			return;
		}
		_overlayScheduled = true;
		QTimer::singleShot(75, this, [this] {
			_overlayScheduled = false;
			showOverlayNow();
		});
	}

	void showOverlayNow() {
		auto *anchor = resolveOverlayAnchor();
		if (!anchor) {
			_host->showToast(Tr(
				_host,
				"Could not find an Astrogram window for the log overlay.",
				u8"Не удалось найти окно Astrogram для overlay с логами."));
			return;
		}
		if (!_overlay) {
			_overlay = new LogsOverlay(
				_host,
				[this](int fetchCount, const QString &filter) {
					return readLogs(fetchCount, filter);
				},
				[this] {
					return clearLogs();
				},
					[this](const QString &text) {
						_host->showToast(text);
					},
					anchor);
			_overlay->setMaxLines(_maxLines);
		}
		_overlay->setAnchor(anchor);
		_overlay->setMaxLines(_maxLines);
		CenterOver(anchor, _overlay);
		_overlay->show();
		_overlay->showNormal();
		_overlay->raise();
		_overlay->activateWindow();
	}

	void handleSetting(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kMaxLinesSettingId)
			&& setting.type == Plugins::SettingControl::IntSlider) {
			_maxLines = std::clamp(setting.intValue, kMinMaxLines, kMaxMaxLines);
			if (_overlay) {
				_overlay->setMaxLines(_maxLines);
			}
		} else if (setting.id == Latin1(kOpenOverlaySettingId)
			&& setting.type == Plugins::SettingControl::ActionButton) {
			scheduleShowOverlay();
		}
	}

	Plugins::Host *_host = nullptr;
	Plugins::PluginInfo _info;
	Plugins::ActionId _actionId = 0;
	Plugins::SettingsPageId _settingsPageId = 0;
	int _maxLines = kDefaultMaxLines;
	bool _overlayScheduled = false;
	QPointer<LogsOverlay> _overlay;
};

} // namespace

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion || !host) {
		return nullptr;
	}
	return new ShowLogsPlugin(host);
}
