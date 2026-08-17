/*
Astrogram transparency plugin.
Adds separate host-managed sliders for interface, message, and text opacity.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsOpacityEffect>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <algorithm>

TGD_PLUGIN_PREVIEW(
	"astro.transparent",
	"AstroTransparent",
	"3.2",
	"@etopizdesblin",
	"Adds separate interface, message, and text transparency controls for Astrogram.",
	"https://sosiskibot.ru",
	"GusTheDuck/4")

namespace {

constexpr auto kPluginId = "astro.transparent";
constexpr auto kLegacyPluginId = "example.transparent_telegram";

constexpr int kDefaultWindowOpacityPercent = 100;
constexpr int kDefaultMessageOpacityPercent = 100;
constexpr int kDefaultTextOpacityPercent = 100;
constexpr int kMinOpacityPercent = 20;
constexpr int kMaxOpacityPercent = 100;
constexpr int kAppearanceApplyDelayMs = 32;
constexpr int kAppearanceRefreshMs = 1500;

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
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

bool IsReadyWidget(QWidget *widget) {
	return widget
		&& widget->testAttribute(Qt::WA_WState_Created)
		&& !widget->testAttribute(Qt::WA_DontShowOnScreen);
}

bool IsSupportedWindowWidget(QWidget *widget) {
	if (!widget || !widget->isWindow() || widget->parentWidget()) {
		return false;
	}
	// Telegram's main window is a plain top-level widget without an explicit
	// Qt::Window flag, so its windowType() is Qt::Widget. Accept the common
	// top-level window types (Window, Widget, Dialog) but skip transient
	// popups, tooltips and menus.
	const auto type = widget->windowType();
	return type == Qt::Window || type == Qt::Widget || type == Qt::Dialog;
}

bool IsReadyWindowWidget(QWidget *widget) {
	return IsSupportedWindowWidget(widget) && IsReadyWidget(widget);
}

int WidgetDepth(QWidget *widget) {
	auto depth = 0;
	for (auto *parent = widget ? widget->parentWidget() : nullptr;
		parent;
		parent = parent->parentWidget()) {
		++depth;
	}
	return depth;
}

QList<QWidget*> WindowRoots(Plugins::Host *host) {
	auto result = QList<QWidget*>();
	auto seen = QSet<QWidget*>();
	host->forEachWindowWidget([&](QWidget *widget) {
		if (IsReadyWindowWidget(widget) && !seen.contains(widget)) {
			seen.insert(widget);
			result.push_back(widget);
		}
	});
	if (!result.isEmpty()) {
		return result;
	}
	for (auto *widget : QApplication::topLevelWidgets()) {
		if (IsReadyWindowWidget(widget) && !seen.contains(widget)) {
			seen.insert(widget);
			result.push_back(widget);
		}
	}
	return result;
}

bool IsTextWidget(QWidget *widget) {
	if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
		return false;
	}
	if (widget->inherits("QAbstractButton")) {
		return false;
	}
	return widget->inherits("QLabel")
		|| widget->inherits("QLineEdit")
		|| widget->inherits("QTextEdit")
		|| widget->inherits("QPlainTextEdit");
}

bool HasAncestorInSet(
		QWidget *widget,
		const QSet<QWidget*> &set) {
	if (!widget || set.isEmpty()) {
		return false;
	}
	for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
		if (set.contains(parent)) {
			return true;
		}
	}
	return false;
}

// Locate the message area inside a top-level window.
//
// Telegram renders message bubbles inside a single large widget
// (HistoryView::ListWidget) that fills the chat pane between the top bar
// and the compose area. Telegram widgets do not declare Q_OBJECT, so
// metaObject()->className() collapses to a generic name ("QWidget") and
// class-name based detection is unreliable. Instead we locate the message
// area structurally: the deepest non-window descendant that spans a large
// middle/right portion of the window.
QWidget *FindMessageArea(QWidget *window) {
	if (!window || window->width() <= 0 || window->height() <= 0) {
		return nullptr;
	}
	const auto windowWidth = window->width();
	const auto windowHeight = window->height();

	struct Candidate {
		QWidget *widget = nullptr;
		int area = 0;
		int depth = 0;
	};
	auto candidates = QList<Candidate>();
	auto maxArea = 0;
	const auto widgets = window->findChildren<QWidget*>();
	for (auto *widget : widgets) {
		if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
			continue;
		}
		const auto width = widget->width();
		const auto height = widget->height();
		// Ignore tiny chrome (top bar, compose, buttons, scrollbars).
		if (width < 180 || height < 120) {
			continue;
		}
		// The chat pane spans a large middle/right portion of the window.
		if (width < 0.42 * windowWidth || width > 0.97 * windowWidth) {
			continue;
		}
		if (height < 0.35 * windowHeight || height > 0.97 * windowHeight) {
			continue;
		}
		const auto area = width * height;
		maxArea = std::max(maxArea, area);
		candidates.push_back({
			.widget = widget,
			.area = area,
			.depth = WidgetDepth(widget),
		});
	}
	// Prefer the deepest candidate among the largest ones, so we dim the
	// message list itself rather than the whole chat pane.
	QWidget *best = nullptr;
	auto bestDepth = -1;
	for (const auto &candidate : candidates) {
		if (candidate.area * 5 < maxArea * 2) {
			continue;
		}
		if (candidate.depth > bestDepth) {
			bestDepth = candidate.depth;
			best = candidate.widget;
		}
	}
	return best;
}

} // namespace

class AstroTransparentPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit AstroTransparentPlugin(Plugins::Host *host)
	: QObject(nullptr)
	, _host(host) {
		_info.id = Latin1(kPluginId);
		_info.name = Tr(_host, "AstroTransparent", u8"АстроПрозрачность");
		_info.version = QStringLiteral("3.2");
		_info.author = QStringLiteral("@etopizdesblin");
		_info.description = Tr(
			_host,
			"Adds separate interface, message, and text transparency controls for Astrogram.",
			u8"Добавляет отдельные настройки прозрачности интерфейса, сообщений и текста в Astrogram.");
		_info.website = QStringLiteral("https://sosiskibot.ru");
		_windowOpacityPercent = readWindowOpacityPercent();
		_messageOpacityPercent = readMessageOpacityPercent();
		_textOpacityPercent = readTextOpacityPercent();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_windowOpacityPercent = readWindowOpacityPercent();
		_messageOpacityPercent = readMessageOpacityPercent();
		_textOpacityPercent = readTextOpacityPercent();

		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				if (setting.id == QStringLiteral("window_opacity")) {
					setWindowOpacityPercent(setting.intValue);
				} else if (setting.id == QStringLiteral("message_opacity")) {
					setMessageOpacityPercent(setting.intValue);
				} else if (setting.id == QStringLiteral("text_opacity")) {
					setTextOpacityPercent(setting.intValue);
				}
			});

		_host->onWindowWidgetCreated([this](QWidget *widget) {
			const auto guard = QPointer<QWidget>(widget);
			QTimer::singleShot(kAppearanceApplyDelayMs, this, [this, guard] {
				if (guard) {
					applyToWindow(guard.data());
				}
			});
		});

		ensureRefreshTimer();
		applyCurrentAppearance();
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}
		if (_refreshTimer) {
			_refreshTimer->stop();
			_refreshTimer->deleteLater();
			_refreshTimer = nullptr;
		}
		restoreDefaults();
	}

private:
	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto windowSlider = Plugins::SettingDescriptor();
		windowSlider.id = QStringLiteral("window_opacity");
		windowSlider.title = Tr(_host, "Interface opacity", u8"Прозрачность интерфейса");
		windowSlider.description = Tr(
			_host,
			"Controls the transparency of the whole window, including the chat list, headers and message area.",
			u8"Управляет прозрачностью всего окна: списка чатов, заголовков и области сообщений.");
		windowSlider.type = Plugins::SettingControl::IntSlider;
		windowSlider.intValue = _windowOpacityPercent;
		windowSlider.intMinimum = kMinOpacityPercent;
		windowSlider.intMaximum = kMaxOpacityPercent;
		windowSlider.intStep = 1;
		windowSlider.valueSuffix = QStringLiteral("%");

		auto messageSlider = Plugins::SettingDescriptor();
		messageSlider.id = QStringLiteral("message_opacity");
		messageSlider.title = Tr(_host, "Message opacity", u8"Прозрачность сообщений");
		messageSlider.description = Tr(
			_host,
			"Applies one uniform opacity to the message area on top of the interface transparency.",
			u8"Применяет дополнительную равномерную прозрачность к области сообщений поверх прозрачности интерфейса.");
		messageSlider.type = Plugins::SettingControl::IntSlider;
		messageSlider.intValue = _messageOpacityPercent;
		messageSlider.intMinimum = kMinOpacityPercent;
		messageSlider.intMaximum = kMaxOpacityPercent;
		messageSlider.intStep = 1;
		messageSlider.valueSuffix = QStringLiteral("%");

		auto textSlider = Plugins::SettingDescriptor();
		textSlider.id = QStringLiteral("text_opacity");
		textSlider.title = Tr(_host, "Text opacity", u8"Прозрачность текста");
		textSlider.description = Tr(
			_host,
			"Softens labels and text controls outside the message area.",
			u8"Смягчает прозрачность надписей и текстовых контролов вне области сообщений.");
		textSlider.type = Plugins::SettingControl::IntSlider;
		textSlider.intValue = _textOpacityPercent;
		textSlider.intMinimum = kMinOpacityPercent;
		textSlider.intMaximum = kMaxOpacityPercent;
		textSlider.intStep = 1;
		textSlider.valueSuffix = QStringLiteral("%");

		auto info = Plugins::SettingDescriptor();
		info.id = QStringLiteral("info");
		info.title = Tr(_host, "How it works", u8"Как это работает");
		info.description = Tr(
			_host,
			"Interface opacity controls the transparency of the whole window. Message and text opacity add extra dimming on top. Each slider is applied separately.",
			u8"Прозрачность интерфейса управляет прозрачностью всего окна. Прозрачность сообщений и текста добавляют дополнительное затемнение поверх. Каждый ползунок применяется отдельно.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("appearance");
		section.title = Tr(_host, "Appearance", u8"Оформление");
		section.settings = {
			windowSlider,
			messageSlider,
			textSlider,
			info,
		};

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("astro_transparent");
		page.title = Tr(_host, "AstroTransparent", u8"АстроПрозрачность");
		page.description = Tr(
			_host,
			"Separate transparency controls for interface, messages, and text.",
			u8"Раздельные настройки прозрачности для интерфейса, сообщений и текста.");
		page.sections = { section };
		return page;
	}

	void setWindowOpacityPercent(int value) {
		const auto clamped = std::clamp(value, kMinOpacityPercent, kMaxOpacityPercent);
		if (_windowOpacityPercent == clamped) {
			return;
		}
		_windowOpacityPercent = clamped;
		applyCurrentAppearance();
	}

	void setMessageOpacityPercent(int value) {
		const auto clamped = std::clamp(value, kMinOpacityPercent, kMaxOpacityPercent);
		if (_messageOpacityPercent == clamped) {
			return;
		}
		_messageOpacityPercent = clamped;
		applyCurrentAppearance();
	}

	void setTextOpacityPercent(int value) {
		const auto clamped = std::clamp(value, kMinOpacityPercent, kMaxOpacityPercent);
		if (_textOpacityPercent == clamped) {
			return;
		}
		_textOpacityPercent = clamped;
		applyCurrentAppearance();
	}

	void ensureRefreshTimer() {
		if (_refreshTimer) {
			return;
		}
		_refreshTimer = new QTimer(this);
		_refreshTimer->setInterval(kAppearanceRefreshMs);
		_refreshTimer->setSingleShot(false);
		QObject::connect(_refreshTimer, &QTimer::timeout, this, [this] {
			// Re-apply so lazily created widgets (message area, labels)
			// receive the effects after the UI is built or a chat opens.
			if (_messageOpacityPercent < kMaxOpacityPercent
				|| _textOpacityPercent < kMaxOpacityPercent) {
				applyCurrentAppearance();
			}
		});
		_refreshTimer->start();
	}

	void applyCurrentAppearance() {
		const auto windows = WindowRoots(_host);

		// Interface opacity drives the native window transparency.
		const auto windowOpacity = interfaceOpacityValue();
		for (auto *window : windows) {
			if (IsReadyWindowWidget(window)) {
				window->setWindowOpacity(windowOpacity);
			}
		}

		// Message and text opacity use per-widget effects on top.
		auto messageTargets = QSet<QWidget*>();
		auto textTargets = QSet<QWidget*>();
		for (auto *window : windows) {
			if (!IsReadyWindowWidget(window)) {
				continue;
			}
			if (_messageOpacityPercent < kMaxOpacityPercent) {
				if (const auto area = FindMessageArea(window)) {
					messageTargets.insert(area);
				}
			}
		}
		for (auto *window : windows) {
			if (!IsReadyWindowWidget(window)) {
				continue;
			}
			if (_textOpacityPercent < kMaxOpacityPercent) {
				collectTextTargets(window, messageTargets, textTargets);
			}
		}

		syncEffects(_messageEffects, messageTargets, messageOpacityValue());
		syncEffects(_textEffects, textTargets, textOpacityValue());
	}

	void applyToWindow(QWidget *widget) {
		if (!IsReadyWindowWidget(widget)) {
			return;
		}
		applyCurrentAppearance();
	}

	void collectTextTargets(
			QWidget *root,
			const QSet<QWidget*> &messageTargets,
			QSet<QWidget*> &targets) {
		const auto widgets = root->findChildren<QWidget*>();
		for (auto *widget : widgets) {
			if (!widget || widget->isWindow() || !IsTextWidget(widget)) {
				continue;
			}
			// Skip labels inside the message area: they are already dimmed
			// by the message effect, so a separate text effect would compound.
			if (HasAncestorInSet(widget, messageTargets)) {
				continue;
			}
			targets.insert(widget);
		}
	}

	void syncEffects(
			QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> &storage,
			const QSet<QWidget*> &targets,
			double opacity) {
		for (auto it = storage.begin(); it != storage.end();) {
			const auto widget = it.key();
			const auto effect = it.value();
			if (!widget || !targets.contains(widget) || opacity >= 0.999) {
				if (widget && effect && widget->graphicsEffect() == effect) {
					widget->setGraphicsEffect(nullptr);
				}
				if (effect) {
					effect->deleteLater();
				}
				it = storage.erase(it);
				continue;
			}
			if (effect) {
				effect->setOpacity(opacity);
			}
			++it;
		}
		if (opacity >= 0.999) {
			return;
		}
		for (auto *widget : targets) {
			if (!widget) {
				continue;
			}
			auto effect = storage.value(widget);
			if (!effect) {
				if (auto *foreign = widget->graphicsEffect();
					foreign && !storage.contains(widget)) {
					continue;
				}
				effect = new QGraphicsOpacityEffect(widget);
				effect->setOpacity(opacity);
				widget->setGraphicsEffect(effect);
				storage.insert(widget, effect);
				QObject::connect(
					widget,
					&QObject::destroyed,
					this,
					[this, widget](QObject *) {
						_messageEffects.remove(widget);
						_textEffects.remove(widget);
					});
			} else {
				effect->setOpacity(opacity);
				if (widget->graphicsEffect() != effect) {
					widget->setGraphicsEffect(effect);
				}
			}
		}
	}

	void restoreDefaults() {
		for (auto *widget : WindowRoots(_host)) {
			if (IsReadyWindowWidget(widget)) {
				widget->setWindowOpacity(1.0);
			}
		}
		clearEffects(_messageEffects);
		clearEffects(_textEffects);
	}

	void clearEffects(QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> &storage) {
		for (auto it = storage.begin(); it != storage.end(); ++it) {
			if (it.key() && it.value() && it.key()->graphicsEffect() == it.value()) {
				it.key()->setGraphicsEffect(nullptr);
			}
			if (it.value()) {
				it.value()->deleteLater();
			}
		}
		storage.clear();
	}

	double interfaceOpacityValue() const {
		return std::clamp(_windowOpacityPercent, kMinOpacityPercent, kMaxOpacityPercent) / 100.0;
	}

	double messageOpacityValue() const {
		return std::clamp(_messageOpacityPercent, kMinOpacityPercent, kMaxOpacityPercent) / 100.0;
	}

	double textOpacityValue() const {
		return std::clamp(_textOpacityPercent, kMinOpacityPercent, kMaxOpacityPercent) / 100.0;
	}

	int readWindowOpacityPercent() const {
		return readIntSetting(QStringLiteral("window_opacity"), kDefaultWindowOpacityPercent);
	}

	int readMessageOpacityPercent() const {
		return readIntSetting(QStringLiteral("message_opacity"), kDefaultMessageOpacityPercent);
	}

	int readTextOpacityPercent() const {
		return readIntSetting(QStringLiteral("text_opacity"), kDefaultTextOpacityPercent);
	}

	int readIntSetting(const QString &settingId, int fallback) const {
		const auto current = _host->settingIntValue(_info.id, settingId, fallback);
		if (current != fallback) {
			return std::clamp(current, kMinOpacityPercent, kMaxOpacityPercent);
		}
		const auto legacy = _host->settingIntValue(Latin1(kLegacyPluginId), settingId, fallback);
		return std::clamp(legacy, kMinOpacityPercent, kMaxOpacityPercent);
	}

	Plugins::Host *_host = nullptr;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	QTimer *_refreshTimer = nullptr;
	int _windowOpacityPercent = kDefaultWindowOpacityPercent;
	int _messageOpacityPercent = kDefaultMessageOpacityPercent;
	int _textOpacityPercent = kDefaultTextOpacityPercent;
	QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> _messageEffects;
	QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> _textEffects;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AstroTransparentPlugin(host);
}
