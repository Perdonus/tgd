/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_astrogram.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "core/core_settings.h"
#include "core/version.h"
#include "lang/lang_instance.h"
#include "plugins/plugins_manager.h"
#include "settings/settings_common.h"
#include "settings/settings_plugins.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QPainterPath>
#include <QStandardPaths>
#include <QTextOption>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace Settings {
namespace {

constexpr auto kHeaderHeight = 188;
constexpr auto kAvatarSize = 76;

[[nodiscard]] bool IsRussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return IsRussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString TranslateProviderLabel(Core::TranslateProvider provider) {
	switch (provider) {
	case Core::TranslateProvider::Telegram:
		return RuEn("Telegram (стандартный)", "Telegram (default)");
	case Core::TranslateProvider::Google:
		return u"Google"_q;
	case Core::TranslateProvider::DeepL:
		return u"DeepL"_q;
	}
	return RuEn("Telegram (стандартный)", "Telegram (default)");
}

[[nodiscard]] QImage AstrogramHeaderImage() {
	static const auto image = [] {
		auto result = QImage(u":/gui/art/astrogram/settings_avatar.png"_q);
		if (result.isNull()) {
			result = QImage(u":/gui/art/logo_256_no_margin.png"_q);
		}
		return result;
	}();
	return image;
}

[[nodiscard]] QRectF CenterCropSourceRect(const QImage &image) {
	if (image.isNull()) {
		return {};
	}
	if (image.width() == image.height()) {
		return QRectF(0, 0, image.width(), image.height());
	}
	if (image.width() > image.height()) {
		const auto side = image.height();
		const auto left = (image.width() - side) / 2.0;
		return QRectF(left, 0, side, side);
	}
	const auto side = image.width();
	const auto top = (image.height() - side) / 2.0;
	return QRectF(0, top, side, side);
}

[[nodiscard]] QString AstrogramVersionText() {
	return QString::fromLatin1("Astrogram Desktop %1").arg(
		QString::fromLatin1(AppVersionStr));
}

void AddAstrogramHeader(not_null<Ui::VerticalLayout*> container) {
	const auto header = container->add(object_ptr<Ui::RpWidget>(container));
	const auto raw = header;
	raw->setMinimumHeight(kHeaderHeight);
	raw->setMaximumHeight(kHeaderHeight);

	raw->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(raw);
		const auto width = raw->width();
			const auto avatarRect = QRect(
				(width - kAvatarSize) / 2,
				14,
				kAvatarSize,
				kAvatarSize);
		const auto avatar = AstrogramHeaderImage();

		if (!avatar.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			QPainterPath path;
			path.addEllipse(QRectF(avatarRect));
			p.save();
			p.setClipPath(path);
			p.drawImage(avatarRect, avatar, CenterCropSourceRect(avatar));
			p.restore();
		}

	auto titleFont = st::semiboldFont->f;
	titleFont.setPixelSize(titleFont.pixelSize() + 3);
		titleFont.setBold(true);
		const auto titleMetrics = QFontMetrics(titleFont);
		const auto titleTop = avatarRect.bottom() + 12;

			p.setPen(st::windowFg);
			p.setFont(titleFont);
			p.drawText(
				QRect(24, titleTop, width - 48, titleMetrics.height() + 6),
				Qt::AlignHCenter | Qt::TextSingleLine,
				u"Astrogram"_q);

		auto versionFont = st::defaultFlatLabel.style.font->f;
		const auto versionMetrics = QFontMetrics(versionFont);
		const auto versionTop = titleTop + titleMetrics.height() + 6;

			p.setPen(st::windowSubTextFg);
			p.setFont(versionFont);
			p.drawText(
				QRect(24, versionTop, width - 48, versionMetrics.height() + 4),
				Qt::AlignHCenter | Qt::TextSingleLine,
				AstrogramVersionText());
		}, raw->lifetime());
}

template <typename Producer, typename Callback>
void AddToggle(
		not_null<Ui::VerticalLayout*> container,
		Producer value,
		const QString &label,
		Callback callback) {
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(label),
		st::settingsButtonNoIcon));
	button->toggleOn(std::move(value));
	button->toggledChanges(
	) | rpl::on_next([=](bool toggled) {
		callback(toggled);
		Core::App().saveSettings();
	}, button->lifetime());
}

template <typename Callback>
void AddActionButtonWithLabel(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &label,
		Callback callback,
		IconDescriptor descriptor = {}) {
	AddButtonWithLabel(
		container,
		rpl::single(title),
		rpl::single(label),
		st::settingsButton,
		std::move(descriptor)
	)->addClickHandler(std::move(callback));
}

template <typename Callback>
void AddActionButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		Callback callback,
		IconDescriptor descriptor = {}) {
	AddButtonWithIcon(
		container,
		rpl::single(title),
		st::settingsButton,
		std::move(descriptor)
	)->addClickHandler(std::move(callback));
}

void AddSectionButton(
			not_null<Window::SessionController*> controller,
			not_null<Ui::VerticalLayout*> container,
			const QString &title,
			Type type,
			IconDescriptor descriptor) {
	AddButtonWithIcon(
		container,
		rpl::single(title),
		st::settingsButton,
		std::move(descriptor)
	)->addClickHandler([=] { controller->showSettings(type); });
}

void ShowSpeechModelDownloadBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		struct DownloadState {
			QPointer<QNetworkReply> reply;
			std::unique_ptr<QFile> output;
			qint64 ready = 0;
			qint64 total = 0;
			QString modelName;
			bool active = false;
		};
		const auto state = box->lifetime().make_state<DownloadState>();
		const auto manager = box->lifetime().make_state<QNetworkAccessManager>();
		const auto selectedModelIndex = box->lifetime().make_state<int>(0);
		const auto modelOptions = std::vector<std::pair<QString, QUrl>>{
			{
				u"Vosk RU small (vosk-model-small-ru-0.22)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-ru-0.22.zip"_q),
			},
			{
				u"Vosk EN small (vosk-model-small-en-us-0.15)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"_q),
			},
			{
				u"Vosk UK small (vosk-model-small-uk-v3-small)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-uk-v3-small.zip"_q),
			},
			{
				u"Vosk DE small (vosk-model-small-de-0.15)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-de-0.15.zip"_q),
			},
			{
				u"Vosk FR small (vosk-model-small-fr-0.22)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-fr-0.22.zip"_q),
			},
			{
				u"Vosk ES small (vosk-model-small-es-0.42)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-es-0.42.zip"_q),
			},
			{
				u"Vosk IT small (vosk-model-small-it-0.22)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-it-0.22.zip"_q),
			},
			{
				u"Vosk PT small (vosk-model-small-pt-0.3)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-pt-0.3.zip"_q),
			},
			{
				u"Vosk TR small (vosk-model-small-tr-0.3)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-tr-0.3.zip"_q),
			},
			{
				u"Vosk PL small (vosk-model-small-pl-0.22)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-pl-0.22.zip"_q),
			},
			{
				u"Vosk JA small (vosk-model-small-ja-0.22)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip"_q),
			},
			{
				u"Vosk KZ small (vosk-model-small-kz-0.42)"_q,
				QUrl(u"https://alphacephei.com/vosk/models/vosk-model-small-kz-0.42.zip"_q),
			},
		};
		auto modelLabel = box->lifetime().make_state<rpl::variable<QString>>(
			modelOptions.front().first);
		auto progressText = box->lifetime().make_state<rpl::variable<QString>>(
			RuEn("Ожидание запуска загрузки", "Waiting to start download"));
		auto progressRatio = box->lifetime().make_state<rpl::variable<float64>>(0.);

		box->setTitle(rpl::single(RuEn(
			"Локальное распознавание речи",
			"Local speech recognition")));
		box->setWidth(st::boxWideWidth);
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

		const auto container = box->verticalLayout();
		Ui::AddSkip(container);
		container->add(object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(RuEn(
				"Скачайте модель распознавания речи на устройство. "
				"Модель хранится локально и используется без отправки аудио в облако.",
				"Download a speech recognition model to your device. "
				"The model is stored locally and used without uploading audio to cloud.")),
			st::boxDividerLabel),
			st::boxRowPadding);
		Ui::AddSkip(container);

		AddButtonWithLabel(
			container,
			rpl::single(RuEn("Модель", "Model")),
			modelLabel->value(),
			st::settingsButton,
			{ &st::menuIconInfo }
		)->addClickHandler([=] {
			*selectedModelIndex = (*selectedModelIndex + 1)
				% int(modelOptions.size());
			*modelLabel = modelOptions[*selectedModelIndex].first;
		});

		const auto progressWidget = container->add(object_ptr<Ui::RpWidget>(container));
		progressWidget->resizeToWidth(st::boxWideWidth - st::boxRowPadding.left() - st::boxRowPadding.right());
		progressWidget->setMinimumHeight(26);
		progressWidget->setMaximumHeight(26);
		progressWidget->paintRequest() | rpl::on_next([=](const QRect &) {
			const auto ratio = std::clamp(progressRatio->current(), 0., 1.);
			QPainter p(progressWidget);
			PainterHighQualityEnabler hq(p);
			const auto outer = progressWidget->rect().marginsRemoved(QMargins(0, 8, 0, 8));
			const auto radius = outer.height() / 2.;
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver->c);
			p.drawRoundedRect(outer, radius, radius);
			if (ratio > 0.) {
				auto filled = outer;
				filled.setWidth(std::max(1, int(std::round(outer.width() * ratio))));
				p.setBrush(QColor(0x21, 0xc7, 0x6a));
				p.drawRoundedRect(filled, radius, radius);
			}
		}, progressWidget->lifetime());

		container->add(object_ptr<Ui::FlatLabel>(
			container,
			progressText->value(),
			st::defaultFlatLabel),
			st::boxRowPadding);

		Ui::AddSkip(container);
		box->addButton(
			rpl::single(RuEn("Скачать", "Download")),
			[=] {
				if (state->active) {
					return;
				}
				const auto option = modelOptions[*selectedModelIndex];
				const auto downloads = QStandardPaths::writableLocation(
					QStandardPaths::DownloadLocation);
				if (downloads.isEmpty()) {
					*progressText = RuEn(
						"Не удалось определить папку Загрузки.",
						"Failed to resolve Downloads folder.");
					return;
				}
				QDir().mkpath(downloads);
				const auto fileName = option.second.fileName().isEmpty()
					? QString(u"speech-model.zip"_q)
					: option.second.fileName();
				const auto filePath = QDir(downloads).filePath(fileName);
				state->output = std::make_unique<QFile>(filePath);
				if (!state->output->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					*progressText = RuEn(
						"Не удалось создать файл в папке Загрузки.",
						"Failed to create output file in Downloads folder.");
					state->output.reset();
					return;
				}

				state->modelName = option.first;
				state->ready = 0;
				state->total = 0;
				state->active = true;
				*progressRatio = 0.;
				*progressText = RuEn("Подключение к серверу...", "Connecting to server...");

				QNetworkRequest request(option.second);
				request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
				state->reply = manager->get(request);

				QObject::connect(
					state->reply,
					&QNetworkReply::readyRead,
					box,
					[=] {
						if (state->output && state->reply) {
							state->output->write(state->reply->readAll());
						}
					});

				QObject::connect(
					state->reply,
					&QNetworkReply::downloadProgress,
					box,
					[=](qint64 ready, qint64 total) {
						state->ready = std::max<qint64>(ready, 0);
						state->total = std::max<qint64>(total, 0);
						const auto ratio = (state->total > 0)
							? std::clamp(double(state->ready) / double(state->total), 0., 1.)
							: 0.;
						*progressRatio = ratio;
						const auto mbReady = double(state->ready) / (1024. * 1024.);
						const auto mbTotal = (state->total > 0)
							? (double(state->total) / (1024. * 1024.))
							: 0.;
						*progressText = (state->total > 0)
							? RuEn(
								"Скачивание: %1 / %2 МБ (%3%)",
								"Downloading: %1 / %2 MB (%3%)"
							).arg(QString::number(mbReady, 'f', 1))
							 .arg(QString::number(mbTotal, 'f', 1))
							 .arg(int(std::round(ratio * 100.)))
							: RuEn(
								"Скачивание: %1 МБ",
								"Downloading: %1 MB"
							).arg(QString::number(mbReady, 'f', 1));
						progressWidget->update();
					});

				QObject::connect(
					state->reply,
					&QNetworkReply::finished,
					box,
					[=] {
						const auto reply = state->reply;
						state->reply = nullptr;
						state->active = false;

						if (state->output) {
							state->output->flush();
							state->output->close();
						}
						const auto outputPath = state->output
							? state->output->fileName()
							: QString();
						state->output.reset();

						if (!reply) {
							*progressText = RuEn(
								"Загрузка завершилась с неизвестной ошибкой.",
								"Download finished with unknown error.");
							return;
						}
						if (reply->error() != QNetworkReply::NoError) {
							if (!outputPath.isEmpty()) {
								QFile::remove(outputPath);
							}
							*progressText = RuEn(
								"Ошибка загрузки: %1",
								"Download error: %1"
							).arg(reply->errorString());
							reply->deleteLater();
							return;
						}
						*progressRatio = 1.;
						progressWidget->update();
						*progressText = RuEn(
							"Модель скачана: %1",
							"Model downloaded: %1"
						).arg(outputPath);
						reply->deleteLater();
					});
			});
		box->addButton(rpl::single(RuEn("Отмена", "Cancel")), [=] {
			if (state->reply) {
				state->reply->abort();
			}
			box->closeBox();
		});
	});
}

void AddLinksSection(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	AddActionButtonWithLabel(
		container,
		RuEn("Основной канал", "Main channel"),
		u"@astrogramchannel"_q,
		[=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QStringLiteral("astrogramchannel"),
			});
		},
		{ &st::menuIconChannel });
	AddActionButtonWithLabel(
		container,
		RuEn("Чат сообщества", "Community chat"),
		u"@astrogram_chat"_q,
		[=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QStringLiteral("astrogram_chat"),
			});
		},
		{ &st::menuIconChats });
	AddActionButtonWithLabel(
		container,
		RuEn("Документация", "Documentation"),
		u"docs.astrogram.su"_q,
		[] {
			QDesktopServices::openUrl(QUrl(u"https://docs.astrogram.su"_q));
		},
		{ &st::menuIconIpAddress });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramHome(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	AddAstrogramHeader(container);
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddSectionButton(
		controller,
		container,
		RuEn("Основные", "General"),
		AstrogramCore::Id(),
		{ &st::menuIconPremium });
	AddSectionButton(
		controller,
		container,
		RuEn("Приватность", "Privacy"),
		AstrogramPrivacy::Id(),
		{ &st::menuIconLock });
	AddSectionButton(
		controller,
		container,
		RuEn("Интерфейс", "Interface"),
		AstrogramInterface::Id(),
		{ &st::menuIconPalette });
	AddSectionButton(
		controller,
		container,
		RuEn("Защита от удаления", "Anti-recall"),
		AstrogramAntiRecall::Id(),
		{ &st::menuIconRestore });
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddSectionButton(
		controller,
		container,
		RuEn("Плагины", "Plugins"),
		Plugins::Id(),
		{ &st::menuIconCustomize });
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddLinksSection(controller, container);
}

void SetupAstrogramCore(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddToggle(
		container,
		settings.localPremiumValue(),
		RuEn("Локальный премиум", "Local Premium"),
		[&](bool toggled) { settings.setLocalPremium(toggled); });
	AddToggle(
		container,
		settings.disableAdsValue(),
		RuEn("Скрывать рекламу и спонсорские блоки", "Hide ads and sponsored"),
		[&](bool toggled) { settings.setDisableAds(toggled); });
	AddToggle(
		container,
		settings.mainMenuAccountsShownValue(),
		RuEn("Показывать аккаунты в боковом меню", "Show accounts in side menu"),
		[&](bool toggled) { settings.setMainMenuAccountsShown(toggled); });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramPrivacy(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddToggle(
		container,
		settings.ghostModeValue(),
		RuEn("Режим призрака", "Ghost mode"),
		[&](bool toggled) { settings.setGhostMode(toggled); });
	AddToggle(
		container,
		settings.ghostHideReadMessagesValue(),
		RuEn("Не читать сообщения", "Don't read messages"),
		[&](bool toggled) { settings.setGhostHideReadMessages(toggled); });
	AddToggle(
		container,
		settings.ghostHideOnlineStatusValue(),
		RuEn("Не отправлять статус «в сети»", "Don't send online packets"),
		[&](bool toggled) { settings.setGhostHideOnlineStatus(toggled); });
	AddToggle(
		container,
		settings.ghostHideTypingProgressValue(),
		RuEn("Не отправлять набор текста и ход загрузки", "Don't send typing/upload progress"),
		[&](bool toggled) { settings.setGhostHideTypingProgress(toggled); });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramInterface(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddToggle(
		container,
		settings.adaptiveForWideValue(),
		RuEn("Адаптивный широкий макет", "Adaptive wide layout"),
		[&](bool toggled) { settings.setAdaptiveForWide(toggled); });
	AddToggle(
		container,
		settings.systemDarkModeEnabledValue(),
		RuEn("Автоматическая тёмная тема по системе", "Auto dark mode from system"),
		[&](bool toggled) { settings.setSystemDarkModeEnabled(toggled); });
	AddToggle(
		container,
		settings.disableStoriesValue(),
		RuEn("Скрыть истории", "Hide stories"),
		[&](bool toggled) { settings.setDisableStories(toggled); });
	AddToggle(
		container,
		settings.disableOpenLinkWarningValue(),
		RuEn("Не спрашивать перед открытием ссылок", "Skip link warning"),
		[&](bool toggled) { settings.setDisableOpenLinkWarning(toggled); });
	AddToggle(
		container,
		settings.showMessageSecondsValue(),
		RuEn("Показывать секунды во времени", "Show message seconds"),
		[&](bool toggled) { settings.setShowMessageSeconds(toggled); });
	AddToggle(
		container,
		rpl::single(settings.translateButtonEnabled()),
		RuEn("Показывать кнопку перевода", "Show translate button"),
		[&](bool toggled) { settings.setTranslateButtonEnabled(toggled); });
	AddToggle(
		container,
		settings.translateChatEnabledValue(),
		RuEn("Переводить чат целиком", "Translate whole chat"),
		[&](bool toggled) { settings.setTranslateChatEnabled(toggled); });
	AddButtonWithLabel(
		container,
		rpl::single(RuEn("Провайдер перевода", "Translation provider")),
		settings.translateProviderValue() | rpl::map([](Core::TranslateProvider provider) {
			return TranslateProviderLabel(provider);
		}),
		st::settingsButton,
		{ &st::menuIconTranslate }
	)->addClickHandler([&settings] {
		const auto next = [&] {
			switch (settings.translateProvider()) {
			case Core::TranslateProvider::Telegram:
				return Core::TranslateProvider::Google;
			case Core::TranslateProvider::Google:
				return Core::TranslateProvider::DeepL;
			case Core::TranslateProvider::DeepL:
				return Core::TranslateProvider::Telegram;
			}
			return Core::TranslateProvider::Telegram;
		}();
		settings.setTranslateProvider(next);
		Core::App().saveSettings();
	});
	AddActionButtonWithLabel(
		container,
		RuEn("Локальное распознавание речи", "Local speech recognition"),
		RuEn("Скачать модель", "Download model"),
		[=] {
			ShowSpeechModelDownloadBox(controller);
		},
		{ &st::menuIconDownload });
	AddToggle(
		container,
		settings.localOnlyDraftsValue(),
		RuEn("Локальные черновики (без облака)", "Local drafts only (no cloud sync)"),
		[&](bool toggled) { settings.setLocalOnlyDrafts(toggled); });
	AddToggle(
		container,
		settings.collapseSimilarChannelsValue(),
		RuEn("Сворачивать похожие каналы", "Collapse similar channels"),
		[&](bool toggled) { settings.setCollapseSimilarChannels(toggled); });
	AddToggle(
		container,
		settings.hideSimilarChannelsValue(),
		RuEn("Скрыть похожие каналы", "Hide similar channels"),
		[&](bool toggled) { settings.setHideSimilarChannels(toggled); });
	AddToggle(
		container,
		settings.largeEmojiValue(),
		RuEn("Крупные эмодзи", "Large emoji"),
		[&](bool toggled) { settings.setLargeEmoji(toggled); });
	AddToggle(
		container,
		settings.replaceEmojiValue(),
		RuEn("Автозамена эмодзи", "Auto replace emoji"),
		[&](bool toggled) { settings.setReplaceEmoji(toggled); });
	AddToggle(
		container,
		settings.cornerReactionValue(),
		RuEn("Быстрая реакция в углу", "Corner quick reaction"),
		[&](bool toggled) { settings.setCornerReaction(toggled); });
	AddToggle(
		container,
		settings.spellcheckerEnabledValue(),
		RuEn("Проверка орфографии", "Spell checker"),
		[&](bool toggled) { settings.setSpellcheckerEnabled(toggled); });
	AddToggle(
		container,
		settings.autoDownloadDictionariesValue(),
		RuEn("Автозагрузка словарей", "Auto download dictionaries"),
		[&](bool toggled) { settings.setAutoDownloadDictionaries(toggled); });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramAntiRecall(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddToggle(
		container,
		settings.saveDeletedMessagesValue(),
		RuEn("Сохранять удалённые сообщения", "Keep deleted messages"),
		[&](bool toggled) { settings.setSaveDeletedMessages(toggled); });
	AddToggle(
		container,
		settings.saveMessagesHistoryValue(),
		RuEn("Сохранять историю правок", "Keep edit history"),
		[&](bool toggled) { settings.setSaveMessagesHistory(toggled); });
	AddToggle(
		container,
		settings.semiTransparentDeletedMessagesValue(),
		RuEn("Полупрозрачные удалённые сообщения", "Semi-transparent deleted messages"),
		[&](bool toggled) { settings.setSemiTransparentDeletedMessages(toggled); });
	AddActionButton(
		container,
		RuEn("Показать журнал", "Show local log"),
		[] { File::ShowInFolder(u"./tdata/astro_recall_log.jsonl"_q); },
		{ &st::menuIconShowInFolder });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramLinks(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	AddLinksSection(controller, container);
}

} // namespace

Astrogram::Astrogram(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> Astrogram::title() {
	return rpl::single(QString());
}

void Astrogram::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramHome(controller, content);
	Ui::ResizeFitChild(this, content);
}

AstrogramCore::AstrogramCore(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramCore::title() {
	return rpl::single(u"Astrogram"_q);
}

void AstrogramCore::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramCore(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramPrivacy::AstrogramPrivacy(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramPrivacy::title() {
	return rpl::single(RuEn("Приватность", "Privacy"));
}

void AstrogramPrivacy::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramPrivacy(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramInterface::AstrogramInterface(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramInterface::title() {
	return rpl::single(RuEn("Интерфейс", "Interface"));
}

void AstrogramInterface::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramInterface(controller, content);
	Ui::ResizeFitChild(this, content);
}

AstrogramAntiRecall::AstrogramAntiRecall(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramAntiRecall::title() {
	return rpl::single(RuEn("Защита от удаления", "Anti-recall"));
}

void AstrogramAntiRecall::setupContent(
		not_null<Window::SessionController*> controller) {
	Q_UNUSED(controller);
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramAntiRecall(content);
	Ui::ResizeFitChild(this, content);
}

AstrogramLinks::AstrogramLinks(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent) {
	setupContent(controller);
}

rpl::producer<QString> AstrogramLinks::title() {
	return rpl::single(RuEn("Ссылки", "Links"));
}

void AstrogramLinks::setupContent(
		not_null<Window::SessionController*> controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramLinks(controller, content);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
