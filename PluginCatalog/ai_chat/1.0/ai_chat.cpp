/*
AI chat plugin for Telegram Desktop.
Registers /ai, exposes host-managed settings for an API key, and opens a
per-window chat dialog backed by sosiskibot.ru.
*/
#include "plugins/plugins_api.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeySequence>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QShortcut>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <optional>
#include <unordered_map>

TGD_PLUGIN_PREVIEW(
	"sosiskibot.ai_chat",
	"AI Chat",
	"1.0",
	"Codex",
	"Opens a per-window AI chat dialog backed by sosiskibot.ru.",
	"https://sosiskibot.ru",
	"")

namespace {

constexpr auto kPluginId = "sosiskibot.ai_chat";
constexpr auto kPluginName = "AI Chat";
constexpr auto kPluginVersion = "1.0";
constexpr auto kPluginAuthor = "Codex";
constexpr auto kPluginDescription =
	"Opens a per-window AI chat dialog backed by sosiskibot.ru.";
constexpr auto kSiteUrl = "https://sosiskibot.ru";
constexpr auto kApiUrl = "https://sosiskibot.ru/api/v1/chat/completions";
constexpr auto kModelName = "gpt-4o-mini";

constexpr auto kApiKeySettingId = "api_key";
constexpr auto kOpenSiteSettingId = "open_site";
constexpr auto kInfoSettingId = "usage_info";

constexpr auto kUserLabel = "You";
constexpr auto kAssistantLabel = "AI";
constexpr auto kSystemLabel = "System";

constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 640;
constexpr int kTranscriptMinimumHeight = 320;
constexpr int kInputHeight = 110;

template <typename Enum>
concept HasTextInputControl = requires {
	Enum::TextInput;
};

template <typename Descriptor>
concept HasTextInputFields = requires(Descriptor value) {
	value.textValue;
	value.placeholderText;
};

template <typename Descriptor>
std::optional<QString> DescriptorTextValue(const Descriptor &value) {
	if constexpr (requires {
		value.textValue;
	}) {
		return value.textValue;
	} else {
		return std::nullopt;
	}
}

template <typename HostType>
QString ReadSettingString(
		HostType *host,
		const QString &pluginId,
		const QString &settingId,
		const QString &fallback = QString()) {
	if constexpr (requires(
			HostType *valueHost,
			const QString &valuePluginId,
			const QString &valueSettingId,
			const QString &valueFallback) {
		valueHost->settingStringValue(
			valuePluginId,
			valueSettingId,
			valueFallback);
	}) {
		return host->settingStringValue(pluginId, settingId, fallback);
	} else {
		const auto stored = host->storedSettingValue(pluginId, settingId);
		return stored.isString() ? stored.toString() : fallback;
	}
}

QString NormalizeText(const QString &text) {
	auto normalized = text;
	normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	normalized.replace(QChar::fromLatin1('\r'), QChar::fromLatin1('\n'));
	return normalized.trimmed();
}

QString Latin1(const char *value) {
	return QString::fromLatin1(value);
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
	const auto choices = payload.value(QStringLiteral("choices")).toArray();
	for (const auto &choiceValue : choices) {
		const auto choice = choiceValue.toObject();
		if (const auto messageText = ExtractTextFromValue(
				choice.value(QStringLiteral("message")));
			!messageText.isEmpty()) {
			return messageText;
		}
		if (const auto contentText = ExtractTextFromValue(
				choice.value(QStringLiteral("content")));
			!contentText.isEmpty()) {
			return contentText;
		}
		if (const auto text = ExtractTextFromValue(choice.value(QStringLiteral("text")));
			!text.isEmpty()) {
			return text;
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
	const auto message = ExtractTextFromValue(payload.value(QStringLiteral("message")));
	return message;
}

bool ApplyTextInputSetting(
		Plugins::SettingDescriptor &setting,
		const QString &value,
		const QString &placeholder) {
	if constexpr (
			HasTextInputControl<Plugins::SettingControl>
		&& HasTextInputFields<Plugins::SettingDescriptor>) {
		setting.type = Plugins::SettingControl::TextInput;
		setting.textValue = value;
		setting.placeholderText = placeholder;
		return true;
	} else {
		Q_UNUSED(value);
		Q_UNUSED(placeholder);
		setting.type = Plugins::SettingControl::InfoText;
		return false;
	}
}

} // namespace

class AiChatPlugin final
	: public QObject
	, public Plugins::Plugin {
public:
	explicit AiChatPlugin(Plugins::Host *host)
	: _host(host)
	, _network(new QNetworkAccessManager(this)) {
		_info.id = Latin1(kPluginId);
		_info.name = Latin1(kPluginName);
		_info.version = Latin1(kPluginVersion);
		_info.author = Latin1(kPluginAuthor);
		_info.description = Latin1(kPluginDescription);
		_info.website = Latin1(kSiteUrl);
		_apiKey = readApiKey();
	}

	Plugins::PluginInfo info() const override {
		return _info;
	}

	void onLoad() override {
		_apiKey = readApiKey();

		_settingsPageId = _host->registerSettingsPage(
			_info.id,
			makeSettingsPage(),
			[this](const Plugins::SettingDescriptor &setting) {
				handleSettingChanged(setting);
			});

		_commandId = _host->registerCommand(
			_info.id,
			{
				QStringLiteral("ai"),
				QStringLiteral("Open the AI chat dialog."),
				QStringLiteral("/ai")
			},
			[this](const Plugins::CommandContext &context) {
				openChatDialog(_host->activeWindowWidget(), context.args);
				auto result = Plugins::CommandResult();
				result.action = Plugins::CommandResult::Action::Handled;
				return result;
			});
	}

	void onUnload() override {
		_isUnloading = true;

		if (_commandId) {
			_host->unregisterCommand(_commandId);
			_commandId = 0;
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

	Plugins::SettingsPageDescriptor makeSettingsPage() const {
		auto apiKey = Plugins::SettingDescriptor();
		apiKey.id = Latin1(kApiKeySettingId);
		apiKey.title = QStringLiteral("API key");
		apiKey.description = QStringLiteral(
			"Used as the Bearer token for requests to sosiskibot.ru.");
		if (!ApplyTextInputSetting(
				apiKey,
				_apiKey,
				QStringLiteral("Paste your sosiskibot.ru API key"))) {
			apiKey.description += QStringLiteral(
				" This build expects host support for a TextInput settings control.");
		}

		auto openSite = Plugins::SettingDescriptor();
		openSite.id = Latin1(kOpenSiteSettingId);
		openSite.title = QStringLiteral("Open sosiskibot.ru");
		openSite.description = QStringLiteral(
			"Open the website to create or manage your API key.");
		openSite.type = Plugins::SettingControl::ActionButton;
		openSite.buttonText = QStringLiteral("Open sosiskibot.ru");

		auto info = Plugins::SettingDescriptor();
		info.id = Latin1(kInfoSettingId);
		info.title = QStringLiteral("Usage");
		info.description = QStringLiteral(
			"Use /ai to open the chat dialog. History is kept in memory per "
			"Telegram window until that window is closed. Requests use model "
			"gpt-4o-mini.");
		info.type = Plugins::SettingControl::InfoText;

		auto section = Plugins::SettingsSectionDescriptor();
		section.id = QStringLiteral("connection");
		section.title = QStringLiteral("Connection");
		section.settings.push_back(apiKey);
		section.settings.push_back(openSite);
		section.settings.push_back(info);

		auto page = Plugins::SettingsPageDescriptor();
		page.id = QStringLiteral("ai_chat");
		page.title = QStringLiteral("AI Chat");
		page.description = QStringLiteral(
			"Configure the API key used by the /ai chat dialog.");
		page.sections.push_back(section);
		return page;
	}

	QString readApiKey() const {
		return NormalizeText(ReadSettingString(
			_host,
			_info.id,
			Latin1(kApiKeySettingId),
			QString()));
	}

	QString defaultStatusText() const {
		return _apiKey.isEmpty()
			? QStringLiteral(
				"Add your sosiskibot.ru API key in Settings > Plugins > AI Chat.")
			: QStringLiteral("Ready. Model: %1").arg(Latin1(kModelName));
	}

	void handleSettingChanged(const Plugins::SettingDescriptor &setting) {
		if (setting.id == Latin1(kApiKeySettingId)) {
			if (const auto textValue = DescriptorTextValue(setting)) {
				_apiKey = NormalizeText(*textValue);
			} else {
				_apiKey = readApiKey();
			}
			refreshIdleWindows();
			return;
		}

		if (setting.id == Latin1(kOpenSiteSettingId)) {
			if (!QDesktopServices::openUrl(QUrl(Latin1(kSiteUrl)))) {
				_host->showToast(QStringLiteral("Could not open sosiskibot.ru."));
			}
		}
	}

	WindowState *findWindowState(QWidget *windowKey) {
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
					_windowStates.erase(windowKey);
				});
		}
		return state;
	}

	void openChatDialog(QWidget *parentWindow, const QString &prefill) {
		if (!parentWindow) {
			_host->showToast(QStringLiteral("Open a Telegram window before using /ai."));
			return;
		}

		auto &state = ensureWindowState(parentWindow);
		if (!state.dialog) {
			createDialog(parentWindow, state);
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
	}

	void createDialog(QWidget *parentWindow, WindowState &state) {
		auto dialog = new QDialog(parentWindow);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(QStringLiteral("AI Chat"));
		dialog->resize(kDialogWidth, kDialogHeight);

		auto layout = new QVBoxLayout(dialog);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(10);

		auto intro = new QLabel(
			QStringLiteral(
				"Chat with sosiskibot.ru from inside Telegram. Each Telegram "
				"window keeps its own in-memory conversation."),
			dialog);
		intro->setWordWrap(true);
		layout->addWidget(intro);

		auto transcript = new QPlainTextEdit(dialog);
		transcript->setReadOnly(true);
		transcript->setMinimumHeight(kTranscriptMinimumHeight);
		transcript->setPlaceholderText(
			QStringLiteral("Conversation messages will appear here."));
		layout->addWidget(transcript, 1);

		auto status = new QLabel(dialog);
		status->setWordWrap(true);
		status->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(status);

		auto input = new QPlainTextEdit(dialog);
		input->setPlaceholderText(QStringLiteral("Ask something..."));
		input->setFixedHeight(kInputHeight);
		layout->addWidget(input);

		auto buttonsLayout = new QHBoxLayout();
		buttonsLayout->setContentsMargins(0, 0, 0, 0);
		buttonsLayout->setSpacing(8);

		auto clearButton = new QPushButton(QStringLiteral("Clear history"), dialog);
		auto sendButton = new QPushButton(QStringLiteral("Send"), dialog);
		sendButton->setDefault(true);

		buttonsLayout->addWidget(clearButton);
		buttonsLayout->addStretch(1);
		buttonsLayout->addWidget(sendButton);
		layout->addLayout(buttonsLayout);

		auto sendShortcut = new QShortcut(
			QKeySequence(Qt::CTRL | Qt::Key_Return),
			dialog);
		auto keypadSendShortcut = new QShortcut(
			QKeySequence(Qt::CTRL | Qt::Key_Enter),
			dialog);

		state.dialog = dialog;
		state.transcriptWidget = transcript;
		state.inputWidget = input;
		state.statusLabel = status;
		state.sendButton = sendButton;
		state.clearButton = clearButton;

		QObject::connect(sendButton, &QPushButton::clicked, this, [this, parentWindow] {
			sendCurrentPrompt(parentWindow);
		});
		QObject::connect(clearButton, &QPushButton::clicked, this, [this, parentWindow] {
			clearHistory(parentWindow);
		});
		QObject::connect(sendShortcut, &QShortcut::activated, this, [this, parentWindow] {
			sendCurrentPrompt(parentWindow);
		});
		QObject::connect(
			keypadSendShortcut,
			&QShortcut::activated,
			this,
			[this, parentWindow] {
				sendCurrentPrompt(parentWindow);
			});
		QObject::connect(dialog, &QDialog::finished, this, [this, parentWindow](int) {
			if (auto *existing = findWindowState(parentWindow)) {
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

		dialog->show();
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
			state->statusText = QStringLiteral(
				"Configure your sosiskibot.ru API key in Settings > Plugins > AI Chat.");
			applyWindowState(*state);
			return;
		}

		const auto prompt = NormalizeText(state->inputWidget->toPlainText());
		if (prompt.isEmpty()) {
			state->statusText = QStringLiteral("Type a prompt before sending.");
			applyWindowState(*state);
			return;
		}

		addTranscriptEntry(*state, Latin1(kUserLabel), prompt);
		state->pendingUserPrompt = prompt;
		state->inputWidget->clear();
		state->statusText = QStringLiteral("Waiting for response...");
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
			return QStringLiteral("Request failed with HTTP %1.")
				.arg(statusCode);
		}
		return QStringLiteral("Request failed.");
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
				Latin1(kSystemLabel),
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
				Latin1(kSystemLabel),
				QStringLiteral("Received an invalid JSON response from the API."));
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
				Latin1(kSystemLabel),
				QStringLiteral("The API response did not contain assistant text."));
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
		addTranscriptEntry(*state, Latin1(kAssistantLabel), assistantText);
		state->statusText = defaultStatusText();
		applyWindowState(*state);

		reply->deleteLater();
	}

	Plugins::Host *_host = nullptr;
	QNetworkAccessManager *_network = nullptr;
	Plugins::CommandId _commandId = 0;
	Plugins::SettingsPageId _settingsPageId = 0;
	Plugins::PluginInfo _info;
	QString _apiKey;
	bool _isUnloading = false;
	std::unordered_map<QWidget*, WindowState> _windowStates;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) {
		return nullptr;
	}
	return new AiChatPlugin(host);
}
