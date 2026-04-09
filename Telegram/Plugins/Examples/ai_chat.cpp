/*
AI chat plugin for Astrogram.
Intercepts /ai, keeps a per-window dialog, and talks to sosiskibot.ru/api.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeySequence>
#include <QtGui/QScreen>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QShortcut>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <utility>
#include <memory>
#include <unordered_map>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

TGD_PLUGIN_PREVIEW(
	"sosiskibot.ai_chat",
	"AI Chat",
	"2.0",
	"@etopizdesblin",
	"Intercepts /ai, opens the built-in Astrogram AI chat, and talks to sosiskibot.ru/api.",
	"https://sosiskibot.ru",
	"")

namespace {

constexpr auto kPluginId = "sosiskibot.ai_chat";
constexpr auto kPluginVersion = "2.0";
constexpr auto kPluginAuthor = "@etopizdesblin";
constexpr auto kSiteUrl = "https://sosiskibot.ru";
constexpr auto kApiUrl = "https://sosiskibot.ru/api/v1/chat/completions";
constexpr auto kModelName = "gpt-4o-mini";

constexpr auto kApiKeySettingId = "api_key";
constexpr auto kOpenChatSettingId = "open_chat";
constexpr auto kOpenSiteSettingId = "open_site";
constexpr auto kInfoSettingId = "usage_info";

constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 640;
constexpr int kTranscriptMinimumHeight = 320;
constexpr int kInputHeight = 110;
constexpr int kOpenDelayMs = 100;
constexpr int kRequestTimeoutMs = 45000;

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
}

QString Utf8(const char *value) {
	return QString::fromUtf8(value);
}

QString Utf8(const char8_t *value) {
	return QString::fromUtf8(reinterpret_cast<const char*>(value));
}

QString NormalizeText(const QString &text) {
	auto normalized = text;
	normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	normalized.replace(QChar::fromLatin1('\r'), QChar::fromLatin1('\n'));
	return normalized.trimmed();
}

QString ExtractTextFromValue(const QJsonValue &value) {
	if (value.isString()) {
		return NormalizeText(value.toString());
	} else if (value.isArray()) {
		QStringList parts;
		for (const auto &entry : value.toArray()) {
			const auto part = ExtractTextFromValue(entry);
			if (!part.isEmpty()) {
				parts.push_back(part);
			}
		}
		return parts.join(QStringLiteral("\n"));
	} else if (value.isObject()) {
		const auto object = value.toObject();
		if (const auto text = ExtractTextFromValue(object.value(QStringLiteral("text")));
			!text.isEmpty()) {
			return text;
		}
		if (const auto content = ExtractTextFromValue(
				object.value(QStringLiteral("content")));
			!content.isEmpty()) {
			return content;
		}
	}
	return QString();
}

QString ExtractAssistantText(const QJsonObject &payload) {
	for (const auto field : {
			QStringLiteral("reply"),
			QStringLiteral("response"),
			QStringLiteral("answer"),
			QStringLiteral("assistant"),
			QStringLiteral("content"),
			QStringLiteral("message"),
			QStringLiteral("text"),
		}) {
		if (const auto text = ExtractTextFromValue(payload.value(field));
			!text.isEmpty()) {
			return text;
		}
	}
	const auto choices = payload.value(QStringLiteral("choices")).toArray();
	for (const auto &choiceValue : choices) {
		const auto choice = choiceValue.toObject();
		for (const auto field : {
				QStringLiteral("message"),
				QStringLiteral("delta"),
				QStringLiteral("content"),
				QStringLiteral("response"),
				QStringLiteral("answer"),
				QStringLiteral("text"),
			}) {
			if (const auto text = ExtractTextFromValue(choice.value(field));
				!text.isEmpty()) {
				return text;
			}
		}
	}
	return QString();
}

QString ExtractErrorText(const QJsonObject &payload) {
	const auto errorValue = payload.value(QStringLiteral("error"));
	if (errorValue.isObject()) {
		const auto error = errorValue.toObject();
		const auto message = ExtractTextFromValue(error.value(QStringLiteral("message")));
		if (!message.isEmpty()) {
			return message;
		}
		const auto type = ExtractTextFromValue(error.value(QStringLiteral("type")));
		if (!type.isEmpty()) {
			return type;
		}
	} else if (errorValue.isString()) {
		return NormalizeText(errorValue.toString());
	}
	return ExtractTextFromValue(payload.value(QStringLiteral("message")));
}

} // namespace

class AiChatPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit AiChatPlugin(Plugins::Host *host)
	: _host(host)
	, _network(new QNetworkAccessManager(this)) {
		refreshInfo();
		_apiKey = readApiKey();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		refreshInfo();
		_apiKey = readApiKey();

		_outgoingInterceptorId = _host->registerOutgoingTextInterceptor(
			_info.id,
			[this](const Plugins::OutgoingTextContext &context) {
				QString args;
				if (!extractAiCommandArgs(context.text, &args)) {
					return Plugins::CommandResult{
						.action = Plugins::CommandResult::Action::Continue
					};
				}
				scheduleOpenChat(
					args,
					nullptr,
					!NormalizeText(args).isEmpty());
				return Plugins::CommandResult{
					.action = Plugins::CommandResult::Action::Handled
				};
			},
			-1000);

		_commandId = _host->registerCommand(
			_info.id,
			{
				QStringLiteral("ai"),
					tr(
						QStringLiteral("Open the AI chat dialog."),
						u8"Открыть окно ИИ-чата."),
						QStringLiteral("/ai")
				},
				[this](const Plugins::CommandContext &context) {
					scheduleOpenChat(
						context.args,
						nullptr,
						!NormalizeText(context.args).isEmpty());
					auto result = Plugins::CommandResult();
					result.action = Plugins::CommandResult::Action::Handled;
					return result;
				});
		_actionId = _host->registerActionWithContext(
			_info.id,
			tr(
				QStringLiteral("Open AI Chat"),
				u8"Открыть ИИ-чат"),
				tr(
					QStringLiteral("Open the built-in sosiskibot.ru AI dialog."),
					u8"Открыть встроенный ИИ-диалог sosiskibot.ru."),
				[this](const Plugins::ActionContext &context) {
					Q_UNUSED(context);
					scheduleOpenChat(QString(), nullptr, false);
				});
		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSettingChanged(setting);
			});
	}

	void onUnload() override {
		_isUnloading = true;

		if (_commandId) {
			_host->unregisterCommand(_commandId);
			_commandId = 0;
		}
		if (_actionId) {
			_host->unregisterAction(_actionId);
			_actionId = 0;
		}
		if (_outgoingInterceptorId) {
			_host->unregisterOutgoingTextInterceptor(_outgoingInterceptorId);
			_outgoingInterceptorId = 0;
		}
		if (_settingsPageId) {
			_host->unregisterSettingsPage(_settingsPageId);
			_settingsPageId = 0;
		}

		for (auto &[windowKey, state] : _windowStates) {
			Q_UNUSED(windowKey);
			if (state.pendingReply) {
				state.pendingReply->abort();
				state.pendingReply->deleteLater();
				state.pendingReply = nullptr;
			}
			if (state.dialog) {
				state.dialog->close();
				state.dialog = nullptr;
			}
		}
		_windowStates.clear();
		if (_standaloneState) {
			if (_standaloneState->pendingReply) {
				_standaloneState->pendingReply->abort();
				_standaloneState->pendingReply->deleteLater();
				_standaloneState->pendingReply = nullptr;
			}
			if (_standaloneState->dialog) {
				_standaloneState->dialog->close();
				_standaloneState->dialog = nullptr;
			}
			_standaloneState.reset();
		}
	}

private:
	struct ChatMessage {
		QString role;
		QString content;
	};

	struct TranscriptEntry {
		QString label;
		QString text;
	};

	struct WindowState {
		QPointer<QWidget> parentWindow;
		QPointer<QDialog> dialog;
		QPointer<QPlainTextEdit> transcriptWidget;
		QPointer<QPlainTextEdit> inputWidget;
		QPointer<QLabel> statusLabel;
		QPointer<QPushButton> sendButton;
		QPointer<QPushButton> clearButton;
		QPointer<QNetworkReply> pendingReply;
		QString pendingUserPrompt;
		QString statusText;
		QVector<ChatMessage> history;
		QVector<TranscriptEntry> transcript;
	};

	bool useRussian() const {
		auto language = _host->hostInfo().appUiLanguage.trimmed();
		if (language.isEmpty()) {
			language = _host->systemInfo().uiLanguage.trimmed();
		}
		return language.startsWith(QStringLiteral("ru"), Qt::CaseInsensitive);
	}

	QString tr(QString en, const char *ru) const {
		return useRussian() ? Utf8(ru) : std::move(en);
	}

	QString tr(QString en, const char8_t *ru) const {
		return useRussian() ? Utf8(ru) : std::move(en);
	}

	void refreshInfo() {
		_info.id = Latin1(kPluginId);
		_info.name = tr(
			QStringLiteral("AI Chat"),
			u8"ИИ-чат");
		_info.version = Latin1(kPluginVersion);
		_info.author = Latin1(kPluginAuthor);
		_info.description = tr(
			QStringLiteral("Intercepts /ai, opens the built-in Astrogram AI chat, and uses sosiskibot.ru/api."),
			u8"Перехватывает /ai, открывает встроенный ИИ-чат Astrogram и использует sosiskibot.ru/api.");
		_info.website = Latin1(kSiteUrl);
	}

	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto apiKey = Plugins::SettingDescriptor();
		apiKey.id = Latin1(kApiKeySettingId);
		apiKey.title = tr(
			QStringLiteral("API key"),
			u8"API-ключ");
		apiKey.description = tr(
			QStringLiteral("Access token for requests to sosiskibot.ru/api."),
			u8"Токен доступа для запросов к sosiskibot.ru/api.");
		apiKey.type = Plugins::SettingControl::TextInput;
		apiKey.textValue = _apiKey;
		apiKey.placeholderText = tr(
			QStringLiteral("Paste your sosiskibot.ru API key"),
			u8"Вставьте API-ключ от sosiskibot.ru");
		apiKey.secret = true;

		auto openSite = Plugins::SettingDescriptor();
		openSite.id = Latin1(kOpenSiteSettingId);
		openSite.title = tr(
			QStringLiteral("Get API key"),
			u8"Получить API-ключ");
		openSite.description = tr(
			QStringLiteral("Open sosiskibot.ru to get or manage your API key."),
			u8"Открыть sosiskibot.ru, чтобы получить или настроить API-ключ.");
		openSite.type = Plugins::SettingControl::ActionButton;
		openSite.buttonText = QStringLiteral("sosiskibot.ru");

		auto openChat = Plugins::SettingDescriptor();
		openChat.id = Latin1(kOpenChatSettingId);
		openChat.title = tr(
			QStringLiteral("Open AI chat"),
			u8"Открыть ИИ-чат");
		openChat.description = tr(
			QStringLiteral("Open the built-in AI chat window without sending /ai into any chat."),
			u8"Открывает встроенное окно ИИ-чата без отправки /ai в какой-либо чат.");
		openChat.type = Plugins::SettingControl::ActionButton;
		openChat.buttonText = tr(
			QStringLiteral("Open"),
			u8"Открыть");

		auto info = Plugins::SettingDescriptor();
		info.id = Latin1(kInfoSettingId);
		info.title = tr(
			QStringLiteral("How it works"),
			u8"Как это работает");
		info.description = _outgoingInterceptorId
			? tr(
				QStringLiteral("Use /ai to open the AI chat. The plugin intercepts the command before sending, so it is not posted into the current chat. API keys are available at sosiskibot.ru."),
				u8"Используйте /ai, чтобы открыть ИИ-чат. Плагин перехватывает команду до отправки, поэтому она не попадает в текущий чат. API-ключ можно получить на sosiskibot.ru.")
			: tr(
				QStringLiteral("Use /ai to open the AI chat. In this build, outgoing slash-command interception is unavailable, so sending /ai may still reach the chat. You can still open the dialog from the plugin action. API keys are available at sosiskibot.ru."),
				u8"Используйте /ai, чтобы открыть ИИ-чат. В этой сборке недоступен перехват исходящих slash-команд, поэтому /ai может уйти в чат. Окно по-прежнему можно открыть через действие плагина. API-ключ можно получить на sosiskibot.ru.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("connection");
		section.title = tr(
			QStringLiteral("Connection"),
			u8"Подключение");
		section.settings.push_back(apiKey);
		section.settings.push_back(openChat);
		section.settings.push_back(openSite);
		section.settings.push_back(info);

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("ai_chat");
		page.title = tr(
			QStringLiteral("AI Chat"),
			u8"ИИ-чат");
		page.description = tr(
			QStringLiteral("Configure the built-in Astrogram AI chat and access to sosiskibot.ru."),
			u8"Настройте встроенный ИИ-чат Astrogram и доступ к sosiskibot.ru.");
		page.sections.push_back(section);
		return page;
	}

	QString readApiKey() const {
		return NormalizeText(_host->settingStringValue(
			_info.id,
			Latin1(kApiKeySettingId),
			QString()));
	}

	QString defaultStatusText() const {
		return _apiKey.isEmpty()
			? tr(
				QStringLiteral("Add your sosiskibot.ru API key in Settings > Plugins > AI Chat."),
				u8"Добавьте API-ключ от sosiskibot.ru: Настройки > Плагины > ИИ-чат.")
			: (_outgoingInterceptorId
				? tr(
					QStringLiteral("Ready. Model: %1. Use Ctrl+Enter to send.").arg(Latin1(kModelName)),
					u8"Готово. Модель: %1. Для отправки используй Ctrl+Enter.").arg(Latin1(kModelName))
				: tr(
					QStringLiteral("Ready, but outgoing /ai interception is unavailable in this build. Model: %1. Use Ctrl+Enter to send.").arg(Latin1(kModelName)),
					u8"Готово, но перехват исходящих /ai в этой сборке недоступен. Модель: %1. Для отправки используй Ctrl+Enter.").arg(Latin1(kModelName)));
	}

	void handleSettingChanged(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kApiKeySettingId)) {
			_apiKey = NormalizeText(setting.textValue);
			refreshIdleWindows();
			return;
		}
		if (setting.id == Latin1(kOpenChatSettingId)) {
			QTimer::singleShot(120, this, [this] {
				if (!_isUnloading) {
					scheduleOpenChat(QString(), nullptr, false);
				}
			});
			return;
		}
		if (setting.id == Latin1(kOpenSiteSettingId)) {
			QTimer::singleShot(120, this, [this] {
				if (!_isUnloading) {
					openSite();
				}
			});
			return;
		}
	}

	void openSite() const {
		const auto url = QUrl(Latin1(kSiteUrl));
		const auto urlText = url.toString();
		auto launched = false;
#ifdef Q_OS_WIN
		const auto shellResult = reinterpret_cast<quintptr>(ShellExecuteW(
			nullptr,
			L"open",
			reinterpret_cast<LPCWSTR>(urlText.utf16()),
			nullptr,
			nullptr,
			SW_SHOWNORMAL));
		launched = (shellResult > 32);
		if (!launched) {
			launched = QProcess::startDetached(
				QStringLiteral("cmd"),
				{
					QStringLiteral("/c"),
					QStringLiteral("start"),
					QStringLiteral(""),
					urlText,
				});
		}
#elif defined(Q_OS_MACOS)
		launched = QProcess::startDetached(
			QStringLiteral("open"),
			{ urlText });
#else
		launched = QProcess::startDetached(
			QStringLiteral("xdg-open"),
			{ urlText });
#endif // Q_OS_WIN
		if (!launched) {
			launched = url.isValid() && QDesktopServices::openUrl(url);
		}
		if (!launched) {
			_host->showToast(tr(
				QStringLiteral("Could not open sosiskibot.ru."),
				u8"Не удалось открыть sosiskibot.ru."));
		}
	}

	bool extractAiCommandArgs(const QString &text, QString *args) const {
		const auto normalized = NormalizeText(text);
		if (normalized.isEmpty() || !normalized.startsWith(QChar::fromLatin1('/'))) {
			return false;
		}
		auto end = normalized.size();
		for (auto i = 1; i < normalized.size(); ++i) {
			if (normalized.at(i).isSpace()) {
				end = i;
				break;
			}
		}
		const auto token = normalized.mid(1, end - 1);
		const auto command = token.section(QChar::fromLatin1('@'), 0, 0);
		if (command.compare(QStringLiteral("ai"), Qt::CaseInsensitive) != 0) {
			return false;
		}
		if (args) {
			*args = normalized.mid(end).trimmed();
		}
		return true;
	}

	WindowState *findWindowState(QWidget *windowKey) {
		if (!windowKey) {
			return _standaloneState.get();
		}
		const auto it = _windowStates.find(windowKey);
		return (it == _windowStates.end()) ? nullptr : &it->second;
	}

	WindowState &ensureWindowState(QWidget *windowKey) {
		const auto [it, inserted] = _windowStates.try_emplace(windowKey);
		auto &state = it->second;
		if (inserted) {
			state.parentWindow = windowKey;
			state.statusText = defaultStatusText();
			QObject::connect(
				windowKey,
				&QObject::destroyed,
				this,
				[this, windowKey](QObject *) {
					if (auto it = _windowStates.find(windowKey);
						it != _windowStates.end()
						&& it->second.pendingReply) {
						it->second.pendingReply->abort();
						it->second.pendingReply->deleteLater();
						it->second.pendingReply = nullptr;
					}
					if (auto it = _windowStates.find(windowKey);
						it != _windowStates.end()
						&& it->second.dialog) {
						it->second.dialog->close();
					}
					_windowStates.erase(windowKey);
				});
		}
		return state;
	}

	WindowState &ensureStandaloneState() {
		if (!_standaloneState) {
			_standaloneState = std::make_unique<WindowState>();
			_standaloneState->statusText = defaultStatusText();
		}
		return *_standaloneState;
	}

	QWidget *stableParentWindow(QWidget *candidate) const {
		if (!candidate) {
			return nullptr;
		}
		auto *top = candidate->window();
		if (!top || !top->isWindow() || top->parentWidget()) {
			return nullptr;
		}
		const auto type = top->windowType();
		if (type == Qt::Dialog
			|| type == Qt::Popup
			|| type == Qt::Tool
			|| type == Qt::ToolTip
			|| type == Qt::Sheet
			|| type == Qt::Drawer
			|| type == Qt::SplashScreen
			|| type == Qt::SubWindow
			|| qobject_cast<QDialog*>(top)) {
			return nullptr;
		}
		if (!top->testAttribute(Qt::WA_WState_Created)
			|| top->testAttribute(Qt::WA_DontShowOnScreen)
			|| !top->isVisible()) {
			return nullptr;
		}
		return top;
	}

	QWidget *resolveChatWindow(QWidget *preferred = nullptr) const {
		if (auto *stable = stableParentWindow(preferred)) {
			return stable;
		}
		if (auto *stable = stableParentWindow(_host->activeWindowWidget())) {
			return stable;
		}
		auto *hostFallback = static_cast<QWidget*>(nullptr);
		_host->forEachWindowWidget([&](QWidget *widget) {
			if (hostFallback || !widget) {
				return;
			}
			if (auto *stable = stableParentWindow(widget)) {
				hostFallback = stable;
			}
		});
		if (hostFallback) {
			return hostFallback;
		}
		if (auto *stable = stableParentWindow(QApplication::activeWindow())) {
			return stable;
		}
		for (auto *widget : QApplication::topLevelWidgets()) {
			if (auto *stable = stableParentWindow(widget)) {
				return stable;
			}
		}
		return nullptr;
	}

	void scheduleOpenChat(
			const QString &prefill,
			QWidget *preferredWindow = nullptr,
			bool autoSend = false) {
		const auto normalizedPrefill = NormalizeText(prefill);
		const auto preferredGuard = QPointer<QWidget>(preferredWindow);
		QTimer::singleShot(kOpenDelayMs, this, [this, preferredGuard, normalizedPrefill, autoSend] {
			if (_isUnloading) {
				return;
			}
			auto *windowKey = resolveChatWindow(preferredGuard.data());
			openChatDialog(windowKey, normalizedPrefill, autoSend);
		});
	}

	void openChatDialog(QWidget *parentWindow, const QString &prefill, bool autoSend) {
		auto &state = parentWindow
			? ensureWindowState(parentWindow)
			: ensureStandaloneState();
		if (!state.dialog) {
			createDialog(parentWindow, state, parentWindow);
		} else {
			state.dialog->show();
			state.dialog->raise();
			state.dialog->activateWindow();
		}

		if (!NormalizeText(prefill).isEmpty() && state.inputWidget) {
			state.inputWidget->setPlainText(prefill);
		}
		if (state.inputWidget) {
			state.inputWidget->setFocus();
		}
		applyWindowState(state);
		if (autoSend
			&& !NormalizeText(prefill).isEmpty()
			&& !state.pendingReply) {
			QTimer::singleShot(0, this, [this, parentWindow] {
				if (!_isUnloading) {
					sendCurrentPrompt(parentWindow);
				}
			});
		}
	}

	void createDialog(QWidget *parentWindow, WindowState &state, QWidget *stateKey) {
		auto *dialogParent = stableParentWindow(parentWindow);
		auto dialog = new QDialog(
			dialogParent,
			Qt::Dialog
				| Qt::Tool
				| Qt::WindowTitleHint
				| Qt::WindowCloseButtonHint
				| Qt::CustomizeWindowHint);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setAttribute(Qt::WA_QuitOnClose, false);
		dialog->setModal(false);
		dialog->setWindowModality(Qt::NonModal);
		dialog->setWindowTitle(tr(
			QStringLiteral("AI Chat"),
			u8"ИИ-чат"));
		dialog->resize(kDialogWidth, kDialogHeight);
		if (dialogParent) {
			const auto center = dialogParent->frameGeometry().center();
			dialog->move(center - QPoint(dialog->width() / 2, dialog->height() / 2));
		} else if (auto *screen = QApplication::primaryScreen()) {
			const auto geometry = screen->availableGeometry();
			dialog->move(
				geometry.center().x() - (dialog->width() / 2),
				geometry.center().y() - (dialog->height() / 2));
		}

		auto layout = new QVBoxLayout(dialog);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(10);

		auto intro = new QLabel(
			tr(
				QStringLiteral("Chat with sosiskibot.ru directly inside Astrogram. Each Astrogram window keeps its own in-memory conversation."),
				u8"Общайся с sosiskibot.ru прямо внутри Astrogram. У каждого окна Astrogram будет своя отдельная история диалога в памяти."),
			dialog);
		intro->setWordWrap(true);
		layout->addWidget(intro);

		auto transcript = new QPlainTextEdit(dialog);
		transcript->setReadOnly(true);
		transcript->setMinimumHeight(kTranscriptMinimumHeight);
		transcript->setPlaceholderText(tr(
			QStringLiteral("Conversation messages will appear here."),
			u8"Здесь будут появляться сообщения диалога."));
		layout->addWidget(transcript, 1);

		auto status = new QLabel(dialog);
		status->setWordWrap(true);
		status->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(status);

		auto input = new QPlainTextEdit(dialog);
		input->setPlaceholderText(tr(
			QStringLiteral("Ask something..."),
			u8"Спроси что-нибудь..."));
		input->setFixedHeight(kInputHeight);
		layout->addWidget(input);

		auto buttonsLayout = new QHBoxLayout();
		buttonsLayout->setContentsMargins(0, 0, 0, 0);
		buttonsLayout->setSpacing(8);

		auto clearButton = new QPushButton(tr(
			QStringLiteral("Clear history"),
			u8"Очистить историю"), dialog);
		auto sendButton = new QPushButton(tr(
			QStringLiteral("Send"),
			u8"Отправить"), dialog);
		sendButton->setDefault(true);

		buttonsLayout->addWidget(clearButton);
		buttonsLayout->addStretch(1);
		buttonsLayout->addWidget(sendButton);
		layout->addLayout(buttonsLayout);

		auto sendShortcut = new QShortcut(
			QKeySequence(int(Qt::CTRL) | int(Qt::Key_Return)),
			dialog);
		auto keypadSendShortcut = new QShortcut(
			QKeySequence(int(Qt::CTRL) | int(Qt::Key_Enter)),
			dialog);

		state.dialog = dialog;
		state.transcriptWidget = transcript;
		state.inputWidget = input;
		state.statusLabel = status;
		state.sendButton = sendButton;
		state.clearButton = clearButton;

		QObject::connect(sendButton, &QPushButton::clicked, this, [this, stateKey] {
			sendCurrentPrompt(stateKey);
		});
		QObject::connect(clearButton, &QPushButton::clicked, this, [this, stateKey] {
			clearHistory(stateKey);
		});
		QObject::connect(sendShortcut, &QShortcut::activated, this, [this, stateKey] {
			sendCurrentPrompt(stateKey);
		});
		QObject::connect(
			keypadSendShortcut,
			&QShortcut::activated,
			this,
			[this, stateKey] {
				sendCurrentPrompt(stateKey);
			});
		QObject::connect(dialog, &QObject::destroyed, this, [this, stateKey](QObject *) {
			if (auto *existing = findWindowState(stateKey)) {
				existing->dialog = nullptr;
				existing->transcriptWidget = nullptr;
				existing->inputWidget = nullptr;
				existing->statusLabel = nullptr;
				existing->sendButton = nullptr;
				existing->clearButton = nullptr;
			}
		});

		rebuildTranscript(state);
		applyWindowState(state);

		dialog->showNormal();
		dialog->raise();
		dialog->activateWindow();
	}

	void appendTranscriptEntry(
			QPlainTextEdit *widget,
			const TranscriptEntry &entry) const {
		if (!widget) {
			return;
		}
		if (!widget->toPlainText().isEmpty()) {
			widget->appendPlainText(QString());
		}
		widget->appendPlainText(entry.label + QStringLiteral(": ") + entry.text);
		if (const auto bar = widget->verticalScrollBar()) {
			bar->setValue(bar->maximum());
		}
	}

	void addTranscriptEntry(
			WindowState &state,
			const QString &label,
			const QString &text) {
		const auto normalized = NormalizeText(text);
		if (normalized.isEmpty()) {
			return;
		}
		auto entry = TranscriptEntry{ label, normalized };
		state.transcript.push_back(entry);
		appendTranscriptEntry(state.transcriptWidget, entry);
	}

	void rebuildTranscript(WindowState &state) const {
		if (!state.transcriptWidget) {
			return;
		}
		state.transcriptWidget->clear();
		for (const auto &entry : state.transcript) {
			appendTranscriptEntry(state.transcriptWidget, entry);
		}
	}

	void applyWindowState(WindowState &state) const {
		const auto busy = (state.pendingReply != nullptr);
		if (state.statusLabel) {
			state.statusLabel->setText(state.statusText);
		}
		if (state.inputWidget) {
			state.inputWidget->setEnabled(!busy);
		}
		if (state.sendButton) {
			state.sendButton->setEnabled(!busy);
		}
		if (state.clearButton) {
			state.clearButton->setEnabled(!busy && !state.transcript.isEmpty());
		}
	}

	void refreshIdleWindows() {
		for (auto &[windowKey, state] : _windowStates) {
			Q_UNUSED(windowKey);
			if (!state.pendingReply) {
				state.statusText = defaultStatusText();
				applyWindowState(state);
			}
		}
		if (_standaloneState && !_standaloneState->pendingReply) {
			_standaloneState->statusText = defaultStatusText();
			applyWindowState(*_standaloneState);
		}
	}

	void clearHistory(QWidget *windowKey) {
		auto *state = findWindowState(windowKey);
		if (!state || state->pendingReply) {
			return;
		}
		state->history.clear();
		state->transcript.clear();
		state->pendingUserPrompt.clear();
		state->statusText = defaultStatusText();
		if (state->transcriptWidget) {
			state->transcriptWidget->clear();
		}
		applyWindowState(*state);
	}

	void sendCurrentPrompt(QWidget *windowKey) {
		auto *state = findWindowState(windowKey);
		if (!state || state->pendingReply || !state->inputWidget) {
			return;
		}

		if (_apiKey.isEmpty()) {
			state->statusText = tr(
				QStringLiteral("Configure your sosiskibot.ru API key in Settings > Plugins > AI Chat."),
				u8"Укажите API-ключ от sosiskibot.ru: Настройки > Плагины > ИИ-чат.");
			applyWindowState(*state);
			return;
		}

		const auto prompt = NormalizeText(state->inputWidget->toPlainText());
		if (prompt.isEmpty()) {
			state->statusText = tr(
				QStringLiteral("Type a prompt before sending."),
				u8"Напиши запрос перед отправкой.");
			applyWindowState(*state);
			return;
		}

		addTranscriptEntry(
			*state,
			tr(QStringLiteral("You"), u8"Ты"),
			prompt);
		state->pendingUserPrompt = prompt;
		state->inputWidget->clear();
		state->statusText = tr(
			QStringLiteral("Waiting for response..."),
			u8"Жду ответ...");
		applyWindowState(*state);

		auto messages = QJsonArray();
		for (const auto &message : state->history) {
			messages.push_back(QJsonObject{
				{ QStringLiteral("role"), message.role },
				{ QStringLiteral("content"), message.content },
			});
		}
		messages.push_back(QJsonObject{
			{ QStringLiteral("role"), QStringLiteral("user") },
			{ QStringLiteral("content"), prompt },
		});

		const auto payload = QJsonObject{
			{ QStringLiteral("model"), Latin1(kModelName) },
			{ QStringLiteral("messages"), messages },
		};

		QNetworkRequest request{ QUrl(Latin1(kApiUrl)) };
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			QByteArrayLiteral("application/json"));
		request.setRawHeader(
			QByteArrayLiteral("Accept"),
			QByteArrayLiteral("application/json"));
		request.setRawHeader(
			QByteArrayLiteral("Authorization"),
			QByteArrayLiteral("Bearer ") + _apiKey.toUtf8());
		request.setTransferTimeout(kRequestTimeoutMs);

		auto *reply = _network->post(
			request,
			QJsonDocument(payload).toJson(QJsonDocument::Compact));
		state->pendingReply = reply;

		QObject::connect(reply, &QNetworkReply::finished, this, [this, windowKey, reply] {
			finishReply(windowKey, reply);
		});
	}

	QString requestErrorText(
			QNetworkReply *reply,
			const QByteArray &body,
			int statusCode) const {
		QJsonParseError parseError;
		const auto parsed = QJsonDocument::fromJson(body, &parseError);
		if (parseError.error == QJsonParseError::NoError && parsed.isObject()) {
			if (const auto payloadError = ExtractErrorText(parsed.object());
				!payloadError.isEmpty()) {
				return payloadError;
			}
		}
		if (reply && reply->error() != QNetworkReply::NoError) {
			return NormalizeText(reply->errorString());
		}
		if (statusCode > 0) {
			return tr(
				QStringLiteral("Request failed with HTTP %1.").arg(statusCode),
				u8"Запрос завершился ошибкой HTTP %1.").arg(statusCode);
		}
		return tr(
			QStringLiteral("Request failed."),
			u8"Запрос завершился ошибкой.");
	}

	void finishReply(QWidget *windowKey, QNetworkReply *reply) {
		const auto body = reply->readAll();
		const auto statusCode = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();

		auto *state = findWindowState(windowKey);
		if (!state) {
			reply->deleteLater();
			return;
		}

		if (state->pendingReply == reply) {
			state->pendingReply = nullptr;
		}

		if (_isUnloading) {
			reply->deleteLater();
			return;
		}

		if (reply->error() != QNetworkReply::NoError || statusCode >= 400) {
			addTranscriptEntry(
				*state,
				tr(QStringLiteral("System"), u8"Система"),
				requestErrorText(reply, body, statusCode));
			state->pendingUserPrompt.clear();
			state->statusText = defaultStatusText();
			applyWindowState(*state);
			reply->deleteLater();
			return;
		}

		QJsonParseError parseError;
		const auto parsed = QJsonDocument::fromJson(body, &parseError);
		if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
			addTranscriptEntry(
				*state,
				tr(QStringLiteral("System"), u8"Система"),
				tr(
					QStringLiteral("Received an invalid JSON response from the API."),
					u8"API вернул некорректный JSON-ответ."));
			state->pendingUserPrompt.clear();
			state->statusText = defaultStatusText();
			applyWindowState(*state);
			reply->deleteLater();
			return;
		}

		const auto assistantText = ExtractAssistantText(parsed.object());
		if (assistantText.isEmpty()) {
			addTranscriptEntry(
				*state,
				tr(QStringLiteral("System"), u8"Система"),
				tr(
					QStringLiteral("The API response did not contain assistant text."),
					u8"В ответе API не оказалось текста ассистента."));
			state->pendingUserPrompt.clear();
			state->statusText = defaultStatusText();
			applyWindowState(*state);
			reply->deleteLater();
			return;
		}

		const auto prompt = state->pendingUserPrompt;
		state->pendingUserPrompt.clear();
		state->history.push_back({
			QStringLiteral("user"),
			prompt,
		});
		state->history.push_back({
			QStringLiteral("assistant"),
			assistantText,
		});
		addTranscriptEntry(
			*state,
			tr(QStringLiteral("AI"), u8"ИИ"),
			assistantText);
		state->statusText = defaultStatusText();
		applyWindowState(*state);

		reply->deleteLater();
	}

	Plugins::Host *_host = nullptr;
	QNetworkAccessManager *_network = nullptr;
	Plugins::CommandId _commandId = 0;
	Plugins::ActionId _actionId = 0;
	Plugins::OutgoingInterceptorId _outgoingInterceptorId = 0;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	QString _apiKey;
	bool _isUnloading = false;
	std::unordered_map<QWidget*, WindowState> _windowStates;
	std::unique_ptr<WindowState> _standaloneState;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion || !host) {
		return nullptr;
	}
	return new AiChatPlugin(host);
}
