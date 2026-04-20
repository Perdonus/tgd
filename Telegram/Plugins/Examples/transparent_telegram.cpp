/*
Astrogram transparency plugin.
Adds separate host-managed sliders for interface, message, and text opacity.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtWidgets/QAbstractButton>
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
	"3.1",
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
constexpr int kAppearanceApplyDelayMs = 16;

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
	return widget->windowType() == Qt::Window;
}

bool IsReadyWindowWidget(QWidget *widget) {
	return IsSupportedWindowWidget(widget) && IsReadyWidget(widget);
}

QByteArray WidgetClassName(QWidget *widget) {
	return widget
		? QByteArray(widget->metaObject()->className()).toLower()
		: QByteArray();
}

QString WidgetObjectName(QWidget *widget) {
	return widget ? widget->objectName().toLower() : QString();
}

bool NameContains(QWidget *widget, const char *needle) {
	return WidgetObjectName(widget).contains(QString::fromLatin1(needle))
		|| WidgetClassName(widget).contains(needle);
}

bool HasAnyNameToken(QWidget *widget, std::initializer_list<const char*> needles) {
	for (const auto needle : needles) {
		if (NameContains(widget, needle)) {
			return true;
		}
	}
	return false;
}

int WidgetDepth(QWidget *widget);

bool IsTextWidgetClass(QWidget *widget) {
	return widget
		&& (widget->inherits("QLabel")
			|| widget->inherits("QLineEdit")
			|| widget->inherits("QTextEdit")
			|| widget->inherits("QPlainTextEdit"));
}

bool LooksLikeMessageContainer(QWidget *widget, QWidget *root = nullptr) {
	if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
		return false;
	}
	if (widget->width() < 36 || widget->height() < 20) {
		return false;
	}
	if (HasAnyNameToken(widget, {
			"tooltip",
			"menu",
			"button",
			"checkbox",
			"radio",
			"slider",
			"scrollbar",
			"lineedit",
			"input",
			"editfield",
			"settings",
				"controls",
				"title",
				"subtitle",
			"header",
			"sidebar",
			"panel",
		})) {
		return false;
	}
	if (IsTextWidgetClass(widget) || widget->inherits("QAbstractButton")) {
		return false;
	}
	if (HasAnyNameToken(widget, {
			"message",
			"bubble",
			"media",
			"photo",
			"video",
			"sticker",
			"gif",
			"album",
			"reply",
			"webpage",
			"attachment",
		})) {
		return true;
	}
	const auto parentWidth = widget->parentWidget()
		? widget->parentWidget()->width()
		: 0;
	const auto rootWidth = root ? root->width() : 0;
	const auto relativeToParent = (parentWidth > 0)
		? (double(widget->width()) / double(parentWidth))
		: 1.0;
	const auto relativeToRoot = (rootWidth > 0)
		? (double(widget->width()) / double(rootWidth))
		: 1.0;
	return WidgetDepth(widget) >= 6
		&& widget->width() >= 120
		&& widget->height() >= 18
		&& relativeToParent < 0.97
		&& relativeToRoot < 0.90
		&& HasAnyNameToken(widget, {
			"history",
			"element",
			"item",
			"entry",
		});
}

bool LooksLikeTextWidget(QWidget *widget) {
	if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
		return false;
	}
	if (widget->inherits("QAbstractButton")) {
		return false;
	}
	return IsTextWidgetClass(widget)
		|| widget->metaObject()->indexOfProperty("text") >= 0
		|| widget->metaObject()->indexOfProperty("placeholderText") >= 0
		|| HasAnyNameToken(widget, {
			"label",
			"text",
			"caption",
			"title",
			"subtitle",
			"name",
			"status",
			"hint",
			"value",
			"lineedit",
			"input",
		});
}

bool LooksLikeInterfaceWidget(QWidget *widget) {
	if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
		return false;
	}
	if (widget->width() < 20 || widget->height() < 14) {
		return false;
	}
	if (LooksLikeMessageContainer(widget) || LooksLikeTextWidget(widget)) {
		return false;
	}
	if (HasAnyNameToken(widget, {
			"tooltip",
			"menu",
			"popup",
			"separator",
			"shadow",
			"ripple",
			"cursor",
		})) {
		return false;
	}
	return HasAnyNameToken(widget, {
		"wrap",
		"container",
		"column",
		"panel",
		"background",
		"sidebar",
		"history",
		"chat",
		"list",
		"footer",
		"header",
		"toolbar",
		"controls",
		"search",
		"compose",
		"reply",
		"tabs",
		"navigation",
		"info",
		"settings",
		});
}

bool LooksLikeInterfaceContainer(QWidget *widget) {
	if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
		return false;
	}
	if (widget->width() < 48 || widget->height() < 24) {
		return false;
	}
	if (widget->inherits("QLabel")
		|| widget->inherits("QAbstractButton")
		|| widget->inherits("QLineEdit")
		|| widget->inherits("QTextEdit")
		|| widget->inherits("QPlainTextEdit")) {
		return false;
	}
	if (HasAnyNameToken(widget, {
			"tooltip",
			"popup",
			"menu",
			"message",
			"bubble",
			"media",
			"photo",
			"video",
			"sticker",
			"reply",
			"attachment",
			"button",
			"checkbox",
			"radio",
			"slider",
			"scrollbar",
			"lineedit",
			"input",
			"text",
			"label",
		})) {
		return false;
	}
	return HasAnyNameToken(widget, {
		"sidebar",
		"header",
		"panel",
		"controls",
		"background",
		"column",
		"chatlist",
		"history",
		"compose",
		"content",
		"wrap",
		"container",
		"body",
		"main",
		"stack",
		"settings",
	});
}

bool LooksLikeInterfaceTarget(QWidget *widget, QWidget *root) {
	if (!widget || widget->isWindow() || !IsReadyWidget(widget)) {
		return false;
	}
	if (LooksLikeMessageContainer(widget, root) || LooksLikeTextWidget(widget)) {
		return false;
	}
	if (!(LooksLikeInterfaceWidget(widget) || LooksLikeInterfaceContainer(widget))) {
		return false;
	}
	if (!root) {
		return true;
	}
	const auto rootWidth = root->width();
	const auto rootHeight = root->height();
	if (rootWidth <= 0 || rootHeight <= 0) {
		return true;
	}
	const auto almostWholeWindow
		= (widget->width() >= (rootWidth - 24))
		&& (widget->height() >= (rootHeight - 24));
	const auto oversizedPanel
		= (widget->width() >= int(rootWidth * 0.96))
		&& (widget->height() >= int(rootHeight * 0.65));
	return !almostWholeWindow && !oversizedPanel;
}

bool HasTrackedAncestor(
		QWidget *widget,
		const QSet<QWidget*> &tracked) {
	for (auto *parent = widget ? widget->parentWidget() : nullptr;
		parent;
		parent = parent->parentWidget()) {
		if (tracked.contains(parent)) {
			return true;
		}
	}
	return false;
}

bool HasTrackedDescendant(
		QWidget *widget,
		const QSet<QWidget*> &tracked) {
	if (!widget) {
		return false;
	}
	for (auto *trackedWidget : tracked) {
		if (trackedWidget
			&& trackedWidget != widget
			&& widget->isAncestorOf(trackedWidget)) {
			return true;
		}
	}
	return false;
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
		_info.version = QStringLiteral("3.1");
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

		scheduleAppearanceApply();
	}

	void onUnload() override {
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
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
			"Targets interface chrome such as sidebars, headers, list panels and compose areas without dimming the whole window.",
			u8"Влияет на оболочку интерфейса: боковые панели, заголовки, списки и область ввода, не затемняя окно целиком.");
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
			"Applies one uniform opacity to message bubbles and media blocks without per-child gradients.",
			u8"Применяет одну равномерную прозрачность к пузырям сообщений и медиа-блокам без градиентов по дочерним элементам.");
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
			"Softens labels and text controls outside message bubbles.",
			u8"Смягчает прозрачность надписей и текстовых контролов вне пузырей сообщений.");
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
			"Interface opacity targets Astrogram chrome only. Message opacity targets message and media containers. Text opacity targets labels and inputs outside message bubbles. Each slider is applied separately.",
			u8"Прозрачность интерфейса влияет только на оболочку Astrogram. Прозрачность сообщений нацелена на контейнеры сообщений и медиа. Прозрачность текста нацелена на надписи и поля ввода вне пузырей сообщений. Каждый ползунок применяется отдельно.");
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

	void scheduleAppearanceApply() {
		if (_appearanceApplyScheduled) {
			return;
		}
		_appearanceApplyScheduled = true;
		QTimer::singleShot(kAppearanceApplyDelayMs, this, [this] {
			_appearanceApplyScheduled = false;
			applyCurrentAppearance();
		});
	}

	void applyCurrentAppearance() {
		auto interfaceTargets = QSet<QWidget*>();
		auto messageTargets = QSet<QWidget*>();
		auto textTargets = QSet<QWidget*>();
		const auto windows = WindowRoots(_host);
		for (auto *window : windows) {
			if (IsReadyWindowWidget(window)) {
				window->setWindowOpacity(1.0);
			}
			collectTargets(window, interfaceTargets, messageTargets, textTargets);
		}

		syncEffects(_interfaceEffects, interfaceTargets, interfaceOpacityValue());
		syncEffects(_messageEffects, messageTargets, messageOpacityValue());
		syncEffects(_textEffects, textTargets, textOpacityValue());
	}

	void applyToWindow(QWidget *widget) {
		if (!IsReadyWindowWidget(widget)) {
			return;
		}
		widget->setWindowOpacity(1.0);
		scheduleAppearanceApply();
	}

	void collectTargets(
			QWidget *root,
			QSet<QWidget*> &interfaceTargets,
			QSet<QWidget*> &messageTargets,
			QSet<QWidget*> &textTargets) {
		if (!IsReadyWindowWidget(root)) {
			return;
		}
		auto widgets = root->findChildren<QWidget*>();
		std::sort(
			widgets.begin(),
			widgets.end(),
			[](QWidget *a, QWidget *b) {
				return WidgetDepth(a) > WidgetDepth(b);
			});

		for (auto *widget : widgets) {
			if (!IsReadyWidget(widget) || widget->isWindow()) {
				continue;
			}
			if (_messageOpacityPercent < kMaxOpacityPercent
				&& LooksLikeMessageContainer(widget, root)
				&& !HasTrackedAncestor(widget, messageTargets)
				&& !HasTrackedDescendant(widget, messageTargets)) {
				messageTargets.insert(widget);
			}
		}

		for (auto *widget : widgets) {
			if (!IsReadyWidget(widget) || widget->isWindow()) {
				continue;
			}
			if (_textOpacityPercent < kMaxOpacityPercent
				&& LooksLikeTextWidget(widget)
				&& !HasTrackedAncestor(widget, messageTargets)
				&& !HasTrackedAncestor(widget, textTargets)
				&& !HasTrackedDescendant(widget, textTargets)) {
				textTargets.insert(widget);
			}
		}

		for (auto *widget : widgets) {
			if (!IsReadyWidget(widget) || widget->isWindow()) {
				continue;
			}
			if (_windowOpacityPercent < kMaxOpacityPercent
				&& LooksLikeInterfaceTarget(widget, root)
				&& !HasTrackedAncestor(widget, messageTargets)
				&& !HasTrackedDescendant(widget, messageTargets)
				&& !HasTrackedAncestor(widget, textTargets)
				&& !HasTrackedDescendant(widget, textTargets)
				&& !HasTrackedAncestor(widget, interfaceTargets)
				&& !HasTrackedDescendant(widget, interfaceTargets)) {
				interfaceTargets.insert(widget);
			}
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
						_interfaceEffects.remove(widget);
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
		clearEffects(_interfaceEffects);
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
	int _windowOpacityPercent = kDefaultWindowOpacityPercent;
	int _messageOpacityPercent = kDefaultMessageOpacityPercent;
	int _textOpacityPercent = kDefaultTextOpacityPercent;
	QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> _interfaceEffects;
	QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> _messageEffects;
	QHash<QWidget*, QPointer<QGraphicsOpacityEffect>> _textEffects;
	bool _appearanceApplyScheduled = false;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AstroTransparentPlugin(host);
}
