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
#include "logs.h"
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
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>
#include <QTextOption>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>
#include <vector>

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
	return QString::fromLatin1("Astrogram Desktop %1 (%2)").arg(
		QString::fromLatin1(AppVersionStr),
		QString::number(AppVersion));
}

[[nodiscard]] QString SpeechModelArchivePath(
		const QString &modelsDir,
		const QString &folderName) {
	return QDir(modelsDir).filePath(folderName + u".zip"_q);
}

[[nodiscard]] bool ExtractSpeechModelArchive(
		const QString &archivePath,
		const QString &modelsDir,
		QString *error) {
	if (!QFileInfo::exists(archivePath)) {
		if (error) {
			*error = RuEn("Архив модели не найден", "Model archive was not found");
		}
		return false;
	}
	QDir().mkpath(modelsDir);
#ifdef Q_OS_WIN
	auto escapedArchive = archivePath;
	auto escapedModelsDir = modelsDir;
	escapedArchive.replace(u"'"_q, u"''"_q);
	escapedModelsDir.replace(u"'"_q, u"''"_q);
	const auto command = QString(
		u"Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force"_q
	).arg(escapedArchive, escapedModelsDir);
	const auto result = QProcess::execute(
		u"powershell"_q,
		{
			u"-NoProfile"_q,
			u"-NonInteractive"_q,
			u"-Command"_q,
			command,
		});
#else // Q_OS_WIN
	const auto result = QProcess::execute(
		u"unzip"_q,
		{
			u"-oq"_q,
			archivePath,
			u"-d"_q,
			modelsDir,
		});
#endif // Q_OS_WIN
	if (result != 0) {
		if (error) {
			*error = RuEn("Не удалось распаковать модель", "Could not extract the model");
		}
		return false;
	}
	return true;
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

void ShowSingleLineTextEditBox(
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QString &placeholder,
		const QString &current,
		Fn<void(QString)> save) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(title));

		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::NoNewlines,
			placeholder,
			TextWithTags{ current, {} }));

		box->addButton(tr::lng_settings_save(), [=] {
			box->closeBox();
			save(field->getLastText().trimmed());
		});
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
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
		struct ModelRowState {
			QString label;
			QString folderName;
			QUrl url;
			bool installed = false;
			bool downloading = false;
			float64 progress = 0.;
			QString status;
			QPointer<QNetworkReply> reply;
			std::unique_ptr<QFile> output;
		};

		const auto manager = box->lifetime().make_state<QNetworkAccessManager>();
		auto models = std::vector<std::shared_ptr<ModelRowState>>();
		auto modelsDir = QDir(cWorkingDir()).filePath(u"tdata/speech_models"_q);
		QDir().mkpath(modelsDir);

		const auto addModel = [&](QString label, QString folder, QString url) {
			auto state = std::make_shared<ModelRowState>();
			state->label = std::move(label);
			state->folderName = std::move(folder);
			state->url = QUrl(std::move(url));
			state->status = RuEn("Не скачана", "Not downloaded");
			models.push_back(std::move(state));
		};
		addModel(u"Русский · Vosk small"_q, u"vosk-model-small-ru-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ru-0.22.zip"_q);
		addModel(u"English · Vosk small"_q, u"vosk-model-small-en-us-0.15"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"_q);
		addModel(u"Українська · Vosk small"_q, u"vosk-model-small-uk-v3-small"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-uk-v3-small.zip"_q);
		addModel(u"Deutsch · Vosk small"_q, u"vosk-model-small-de-0.15"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-de-0.15.zip"_q);
		addModel(u"Français · Vosk small"_q, u"vosk-model-small-fr-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-fr-0.22.zip"_q);
		addModel(u"Español · Vosk small"_q, u"vosk-model-small-es-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-es-0.42.zip"_q);
		addModel(u"Italiano · Vosk small"_q, u"vosk-model-small-it-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-it-0.22.zip"_q);
		addModel(u"Português · Vosk small"_q, u"vosk-model-small-pt-0.3"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-pt-0.3.zip"_q);
		addModel(u"Türkçe · Vosk small"_q, u"vosk-model-small-tr-0.3"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-tr-0.3.zip"_q);
		addModel(u"Polski · Vosk small"_q, u"vosk-model-small-pl-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-pl-0.22.zip"_q);
		addModel(u"日本語 · Vosk small"_q, u"vosk-model-small-ja-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip"_q);
		addModel(u"Қазақша · Vosk small"_q, u"vosk-model-small-kz-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-kz-0.42.zip"_q);

		const auto isInstalled = [&](const std::shared_ptr<ModelRowState> &state) {
			const auto dir = QDir(modelsDir).filePath(state->folderName);
			return QFileInfo(dir).isDir();
		};
		for (const auto &state : models) {
			state->installed = isInstalled(state);
			const auto archivePath = SpeechModelArchivePath(modelsDir, state->folderName);
			state->status = state->installed
				? RuEn("Скачана", "Downloaded")
				: QFileInfo::exists(archivePath)
				? RuEn("Архив готов к распаковке", "Archive ready to extract")
				: RuEn("Не скачана", "Not downloaded");
		}

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
				"Скачайте языковые модели локально. Повторная загрузка скрыта после установки, а прогресс показывается прямо на нижней границе строки.",
				"Download language models locally. Re-download is hidden after install, and progress is shown on the bottom edge of each row.")),
			st::boxDividerLabel),
			st::boxRowPadding);
		Ui::AddSkip(container);

		for (const auto &state : models) {
			const auto row = container->add(object_ptr<Ui::RpWidget>(container));
			row->setMinimumHeight(54);
			row->setMaximumHeight(54);
			row->paintRequest() | rpl::on_next([=] {
				auto p = Painter(row);
				const auto rect = row->rect();
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawRoundedRect(rect.adjusted(0, 0, 0, -1), 10, 10);
				if (state->downloading) {
					auto progressRect = QRect(0, rect.height() - 3, int(rect.width() * std::clamp(state->progress, 0., 1.)), 3);
					p.setBrush(QColor(0x21, 0xc7, 0x6a));
					p.drawRoundedRect(progressRect, 2, 2);
				}
				p.setPen(st::windowFg);
				p.setFont(st::semiboldFont->f);
				p.drawText(QRect(14, 8, rect.width() - 90, 18), Qt::AlignLeft | Qt::AlignVCenter, state->label);
				p.setPen(st::windowSubTextFg);
				p.setFont(st::defaultFlatLabel.style.font->f);
				p.drawText(QRect(14, 28, rect.width() - 90, 16), Qt::AlignLeft | Qt::AlignVCenter, state->status);
				if (!state->installed && !state->downloading) {
					st::menuIconDownload.paint(p, rect.width() - 14 - st::menuIconDownload.width(), (rect.height() - st::menuIconDownload.height()) / 2, rect.width(), st::windowSubTextFg->c);
				} else if (state->installed) {
					p.setPen(QColor(0x21, 0xc7, 0x6a));
					p.setFont(st::semiboldFont->f);
					p.drawText(QRect(rect.width() - 48, 0, 34, rect.height()), Qt::AlignCenter, QString::fromUtf8("✓"));
				}
			}, row->lifetime());
			row->setCursor(style::cur_pointer);
			row->events() | rpl::start_with_next([=](not_null<QEvent*> e) {
				if (e->type() != QEvent::MouseButtonRelease) {
					return;
				}
				if (state->installed || state->downloading) {
					return;
				}
				for (const auto &other : models) {
					if (other->downloading) {
						return;
					}
				}
				const auto archivePath = SpeechModelArchivePath(modelsDir, state->folderName);
				if (QFileInfo::exists(archivePath)) {
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] extract requested: %1")
						.arg(state->folderName));
					state->status = RuEn("Распаковка...", "Extracting...");
					row->update();
					auto error = QString();
					if (!ExtractSpeechModelArchive(archivePath, modelsDir, &error)) {
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] extract failed: %1 reason=%2")
							.arg(state->folderName, error));
						state->status = error;
						row->update();
						return;
					}
					if (!isInstalled(state)) {
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] extract finished without model dir: %1")
							.arg(state->folderName));
						state->status = RuEn("Модель не найдена после распаковки", "Model folder not found after extraction");
						row->update();
						return;
					}
					QFile::remove(archivePath);
					state->progress = 1.;
					state->installed = true;
					state->status = RuEn("Скачана", "Downloaded");
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] extract finished: %1")
						.arg(state->folderName));
					row->update();
					return;
				}
				QDir().mkpath(modelsDir);
				const auto targetPath = archivePath;
				state->output = std::make_unique<QFile>(targetPath);
				if (!state->output->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					state->status = RuEn("Не удалось создать файл", "Could not create file");
					row->update();
					return;
				}
				state->progress = 0.;
				state->downloading = true;
				state->status = RuEn("Скачивание...", "Downloading...");
				Logs::writeClient(QString::fromLatin1(
					"[speech-models] download started: %1 url=%2")
					.arg(state->folderName, state->url.toString()));
				row->update();
				QNetworkRequest request(state->url);
				request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
				state->reply = manager->get(request);
				QObject::connect(state->reply, &QNetworkReply::readyRead, box, [=] {
					if (state->output && state->reply) {
						state->output->write(state->reply->readAll());
					}
				});
				QObject::connect(state->reply, &QNetworkReply::downloadProgress, box, [=](qint64 ready, qint64 total) {
					state->progress = (total > 0) ? std::clamp(double(ready) / double(total), 0., 1.) : 0.;
					state->status = (total > 0)
						? RuEn("Скачивание: %1%", "Downloading: %1%").arg(int(std::round(state->progress * 100.)))
						: RuEn("Скачивание...", "Downloading...");
					row->update();
				});
				QObject::connect(state->reply, &QNetworkReply::finished, box, [=] {
					const auto reply = state->reply;
					state->reply = nullptr;
					state->downloading = false;
					if (state->output) {
						state->output->flush();
						state->output->close();
					}
					const auto outputPath = state->output ? state->output->fileName() : QString();
					state->output.reset();
					if (!reply) {
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] download finished with null reply: %1")
							.arg(state->folderName));
						state->status = RuEn("Неизвестная ошибка", "Unknown error");
						row->update();
						return;
					}
					if (reply->error() != QNetworkReply::NoError) {
						if (!outputPath.isEmpty()) {
							QFile::remove(outputPath);
						}
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] download failed: %1 reason=%2")
							.arg(state->folderName, reply->errorString()));
						state->status = RuEn("Ошибка загрузки", "Download failed");
						reply->deleteLater();
						row->update();
						return;
					}
					state->status = RuEn("Распаковка...", "Extracting...");
					row->update();
					auto error = QString();
					if (!ExtractSpeechModelArchive(outputPath, modelsDir, &error)) {
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] post-download extract failed: %1 reason=%2")
							.arg(state->folderName, error));
						state->status = error;
						reply->deleteLater();
						row->update();
						return;
					}
					if (!isInstalled(state)) {
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] post-download extract missing dir: %1")
							.arg(state->folderName));
						state->status = RuEn("Модель не найдена после распаковки", "Model folder not found after extraction");
						reply->deleteLater();
						row->update();
						return;
					}
					QFile::remove(outputPath);
					state->progress = 1.;
					state->installed = true;
					state->status = RuEn("Скачана", "Downloaded");
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] download finished: %1")
						.arg(state->folderName));
					reply->deleteLater();
					row->update();
				});
			}, row->lifetime());
			container->add(object_ptr<Ui::FixedHeightWidget>(container, st::settingsCheckboxesSkip / 3));
		}

		box->addButton(rpl::single(RuEn("Готово", "Done")), [=] { box->closeBox(); });
	}));
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

void AddSectionGroupTitle(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(text),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
}

void AddSectionGroup(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		std::initializer_list<std::tuple<QString, Type, IconDescriptor>> entries) {
	AddSectionGroupTitle(container, title);
	auto card = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		style::margins(6, 8, 6, 0),
		style::al_top);
	card->resizeToWidth(container->width());
	Ui::AddDivider(card);
	Ui::AddSkip(card, st::settingsCheckboxesSkip / 2);
	for (const auto &[entryTitle, type, descriptor] : entries) {
		AddSectionButton(controller, card, entryTitle, type, descriptor);
	}
	Ui::AddSkip(card, st::settingsCheckboxesSkip / 2);
}

void SetupAstrogramHome(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	AddAstrogramHeader(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroup(
		controller,
		container,
		RuEn("Astrogram", "Astrogram"),
		{
			{ RuEn("Основные", "General"), AstrogramCore::Id(), { &st::menuIconPremium } },
			{ RuEn("Приватность", "Privacy"), AstrogramPrivacy::Id(), { &st::menuIconLock } },
			{ RuEn("Интерфейс", "Interface"), AstrogramInterface::Id(), { &st::menuIconPalette } },
		});
	Ui::AddSkip(container);
	AddSectionGroup(
		controller,
		container,
		RuEn("Модули", "Modules"),
		{
			{ RuEn("Плагины", "Plugins"), Plugins::Id(), { &st::menuIconCustomize } },
			{ RuEn("Защита от удаления", "Anti-recall"), AstrogramAntiRecall::Id(), { &st::menuIconRestore } },
		});
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Ссылки", "Links"));
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddLinksSection(controller, container);
}

void SetupAstrogramCore(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Основные возможности", "Core features"));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
	AddSectionGroupTitle(container, RuEn("Аккаунт и Premium", "Account & Premium"));
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
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Боковое меню", "Side menu"));
	AddToggle(
		container,
		settings.mainMenuAccountsShownValue(),
		RuEn("Показывать аккаунты в боковом меню", "Show accounts in side menu"),
		[&](bool toggled) { settings.setMainMenuAccountsShown(toggled); });
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramPrivacy(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Приватность и следы", "Privacy & traces"));
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
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Удаление и правки", "Deleted & edited messages"));
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
	AddToggle(
		container,
		settings.editedMarkShowTextValue(),
		RuEn("Показывать текст у отметки изменения", "Show text on edited tag"),
		[&](bool toggled) { settings.setEditedMarkShowText(toggled); });
	AddToggle(
		container,
		settings.editedMarkShowIconValue(),
		RuEn("Показывать значок у отметки изменения", "Show icon on edited tag"),
		[&](bool toggled) { settings.setEditedMarkShowIcon(toggled); });
	AddButtonWithLabel(
		container,
		rpl::single(RuEn("Текст отметки изменения", "Edited tag text")),
		settings.editedMarkTextValue() | rpl::map([](const QString &value) {
			return value.trimmed().isEmpty()
				? RuEn("Изменено (по умолчанию)", "Edited (default)")
				: value.trimmed();
		}),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		ShowSingleLineTextEditBox(
			controller,
			RuEn("Текст отметки изменения", "Edited tag text"),
			RuEn("Изменено", "Edited"),
			Core::App().settings().editedMarkText(),
			[](QString value) {
				auto &settings = Core::App().settings();
				settings.setEditedMarkText(std::move(value));
				Core::App().saveSettings();
			});
	});
	AddToggle(
		container,
		settings.deletedMarkShowTextValue(),
		RuEn("Показывать текст у отметки удаления", "Show text on deleted tag"),
		[&](bool toggled) { settings.setDeletedMarkShowText(toggled); });
	AddToggle(
		container,
		settings.deletedMarkShowIconValue(),
		RuEn("Показывать значок у отметки удаления", "Show icon on deleted tag"),
		[&](bool toggled) { settings.setDeletedMarkShowIcon(toggled); });
	AddButtonWithLabel(
		container,
		rpl::single(RuEn("Текст отметки удаления", "Deleted tag text")),
		settings.deletedMarkTextValue() | rpl::map([](const QString &value) {
			return value.trimmed().isEmpty()
				? RuEn("Удалено (по умолчанию)", "Deleted (default)")
				: value.trimmed();
		}),
		st::settingsButton,
		{ &st::menuIconDelete }
	)->addClickHandler([=] {
		ShowSingleLineTextEditBox(
			controller,
			RuEn("Текст отметки удаления", "Deleted tag text"),
			RuEn("Удалено", "Deleted"),
			Core::App().settings().deletedMarkText(),
			[](QString value) {
				auto &settings = Core::App().settings();
				settings.setDeletedMarkText(std::move(value));
				Core::App().saveSettings();
			});
	});
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramInterface(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Интерфейс и поведение", "Interface & behavior"));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
	AddSectionGroupTitle(container, RuEn("Окно и навигация", "Window & navigation"));
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
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Перевод и речь", "Translation & speech"));
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
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Текст и ввод", "Text & input"));
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
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Журнал защиты от удаления", "Anti-recall log"));
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
	return rpl::single(RuEn("Основные", "General"));
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
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramPrivacy(controller, content);
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
