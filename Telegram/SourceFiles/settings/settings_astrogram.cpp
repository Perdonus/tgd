/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_astrogram.h"

#include "apiwrap.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/core_settings.h"
#include "core/launcher.h"
#include "core/update_checker.h"
#include "core/version.h"
#include "logs.h"
#include "lang/lang_instance.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "plugins/plugins_manager.h"
#include "settings/settings_common.h"
#include "settings/settings_experimental.h"
#include "settings/settings_plugins.h"
#include "settings.h"
#include "storage/localstorage.h"
#include "main/main_session.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/single_choice_box.h"
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
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>
#include <QTextOption>
#include <QUrl>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>
#include <vector>

namespace Settings {
namespace {

constexpr auto kHeaderHeight = 188;
constexpr auto kAvatarSize = 88;
constexpr auto kSecretChannelClickThreshold = 7;
constexpr auto kSecretChannelClickWindowMs = 1500;

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

[[nodiscard]] QString AstrogramCurrentBuildText() {
	return RuEn(
		"Текущая сборка: %1",
		"Current build: %1").arg(Core::FormatVersionWithBuild(AppVersion));
}

[[nodiscard]] QString AstrogramUpdateChannelText() {
	if (cAlphaVersion()) {
		return RuEn("Alpha", "Alpha");
	}
	return cInstallBetaVersion()
		? RuEn("Dev (beta)", "Dev (beta)")
		: RuEn("Stable", "Stable");
}

[[nodiscard]] QString AstrogramLastUpdateCheckText() {
	if (const auto value = cLastUpdateCheck(); value > 0) {
		const auto date = QDateTime::fromSecsSinceEpoch(value).toLocalTime();
		return RuEn(
			"Последняя проверка: %1",
			"Last checked: %1").arg(date.toString(Qt::DefaultLocaleShortDate));
	}
	return RuEn(
		"Проверка обновлений ещё не запускалась.",
		"Update check has not run yet.");
}

[[nodiscard]] QString AstrogramUpdateProgressText(int ready, int total) {
	if (total <= 0) {
		return RuEn(
			"Скачивание обновления Astrogram...",
			"Downloading Astrogram update...");
	}
	const auto percent = std::clamp(
		int(std::llround((double(ready) * 100.) / double(total))),
		0,
		100);
	return RuEn(
		"Скачивание обновления Astrogram: %1%",
		"Downloading Astrogram update: %1%").arg(percent);
}

[[nodiscard]] QString AstrogramUpdateHeadline(
		const Core::UpdateChecker &checker,
		const Core::UpdateReleaseInfo &info) {
	using State = Core::UpdateChecker::State;
	switch (checker.state()) {
	case State::Download:
		return AstrogramUpdateProgressText(
			checker.already(),
			checker.size());
	case State::Ready:
		return RuEn(
			"Обновление Astrogram готово к установке.",
			"Astrogram update is ready to install.");
	case State::None:
	default:
		break;
	}
	if (info.available) {
		const auto version = info.versionText.isEmpty()
			? Core::FormatVersionWithBuild(info.version)
			: info.versionText;
		return RuEn(
			"Доступно обновление Astrogram: %1",
			"Astrogram update available: %1").arg(version);
	}
	return cAutoUpdate()
		? RuEn(
			"Автообновления Astrogram включены.",
			"Astrogram auto-updates are enabled.")
		: RuEn(
			"Автообновления Astrogram выключены.",
			"Astrogram auto-updates are disabled.");
}

[[nodiscard]] QString AstrogramUpdateDetails(
		const Core::UpdateReleaseInfo &info) {
	if (!info.title.isEmpty()) {
		return info.title;
	}
	return AstrogramCurrentBuildText()
		+ u'\n'
		+ RuEn("Канал: %1", "Channel: %1").arg(AstrogramUpdateChannelText())
		+ u'\n'
		+ AstrogramLastUpdateCheckText();
}

[[nodiscard]] QString AstrogramUpdateChangelogText(
		const Core::UpdateReleaseInfo &info) {
	if (info.changelogLoading) {
		return RuEn(
			"Загружаю changelog из GitHub...",
			"Loading changelog from GitHub...");
	} else if (!info.changelog.isEmpty()) {
		return info.changelog;
	} else if (info.available && info.changelogFailed) {
		return RuEn(
			"Для этой сборки changelog пока пустой или недоступен.",
			"Changelog is empty or unavailable for this build.");
	}
	return QString();
}

struct SpeechModelSpec {
	QString label;
	QString folderName;
	QString url;
};

[[nodiscard]] const std::vector<SpeechModelSpec> &SpeechModelSpecs() {
	static const auto specs = std::vector<SpeechModelSpec>{
		{ u"Русский · Vosk"_q, u"vosk-model-small-ru-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ru-0.22.zip"_q },
		{ u"English (US) · Vosk"_q, u"vosk-model-small-en-us-0.15"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"_q },
		{ u"English (India) · Vosk"_q, u"vosk-model-small-en-in-0.4"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-en-in-0.4.zip"_q },
		{ u"中文 · Vosk"_q, u"vosk-model-small-cn-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip"_q },
		{ u"Українська · Vosk"_q, u"vosk-model-small-uk-v3-small"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-uk-v3-small.zip"_q },
		{ u"Deutsch · Vosk"_q, u"vosk-model-small-de-0.15"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-de-0.15.zip"_q },
		{ u"Français · Vosk"_q, u"vosk-model-small-fr-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-fr-0.22.zip"_q },
		{ u"Español · Vosk"_q, u"vosk-model-small-es-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-es-0.42.zip"_q },
		{ u"Português · Vosk"_q, u"vosk-model-small-pt-0.3"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-pt-0.3.zip"_q },
		{ u"Ελληνικά · Vosk"_q, u"vosk-model-el-gr-0.7"_q, u"https://alphacephei.com/vosk/models/vosk-model-el-gr-0.7.zip"_q },
		{ u"Türkçe · Vosk"_q, u"vosk-model-small-tr-0.3"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-tr-0.3.zip"_q },
		{ u"Tiếng Việt · Vosk"_q, u"vosk-model-small-vn-0.4"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-vn-0.4.zip"_q },
		{ u"Italiano · Vosk"_q, u"vosk-model-small-it-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-it-0.22.zip"_q },
		{ u"Nederlands · Vosk"_q, u"vosk-model-small-nl-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-nl-0.22.zip"_q },
		{ u"Català · Vosk"_q, u"vosk-model-small-ca-0.4"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ca-0.4.zip"_q },
		{ u"العربية · Vosk"_q, u"vosk-model-ar-mgb2-0.4"_q, u"https://alphacephei.com/vosk/models/vosk-model-ar-mgb2-0.4.zip"_q },
		{ u"العربية (تونس) · Vosk"_q, u"vosk-model-small-ar-tn-0.1-linto"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ar-tn-0.1-linto.zip"_q },
		{ u"فارسی · Vosk"_q, u"vosk-model-small-fa-0.5"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-fa-0.5.zip"_q },
		{ u"Filipino · Vosk"_q, u"vosk-model-tl-ph-generic-0.6"_q, u"https://alphacephei.com/vosk/models/vosk-model-tl-ph-generic-0.6.zip"_q },
		{ u"Қазақша · Vosk"_q, u"vosk-model-small-kz-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-kz-0.42.zip"_q },
		{ u"Svenska · Vosk"_q, u"vosk-model-small-sv-rhasspy-0.15"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-sv-rhasspy-0.15.zip"_q },
		{ u"日本語 · Vosk"_q, u"vosk-model-small-ja-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip"_q },
		{ u"Esperanto · Vosk"_q, u"vosk-model-small-eo-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-eo-0.42.zip"_q },
		{ u"हिन्दी · Vosk"_q, u"vosk-model-small-hi-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-hi-0.22.zip"_q },
		{ u"Čeština · Vosk"_q, u"vosk-model-small-cs-0.4-rhasspy"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-cs-0.4-rhasspy.zip"_q },
		{ u"Polski · Vosk"_q, u"vosk-model-small-pl-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-pl-0.22.zip"_q },
		{ u"O'zbekcha · Vosk"_q, u"vosk-model-small-uz-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-uz-0.22.zip"_q },
		{ u"한국어 · Vosk"_q, u"vosk-model-small-ko-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ko-0.22.zip"_q },
		{ u"Brezhoneg · Vosk"_q, u"vosk-model-br-0.8"_q, u"https://alphacephei.com/vosk/models/vosk-model-br-0.8.zip"_q },
		{ u"ગુજરાતી · Vosk"_q, u"vosk-model-small-gu-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-gu-0.42.zip"_q },
		{ u"Тоҷикӣ · Vosk"_q, u"vosk-model-small-tg-0.22"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-tg-0.22.zip"_q },
		{ u"తెలుగు · Vosk"_q, u"vosk-model-small-te-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-te-0.42.zip"_q },
		{ u"Кыргызча · Vosk"_q, u"vosk-model-small-ky-0.42"_q, u"https://alphacephei.com/vosk/models/vosk-model-small-ky-0.42.zip"_q },
	};
	return specs;
}

[[nodiscard]] QString SpeechModelsDirectory() {
	return QDir(cWorkingDir()).filePath(u"tdata/speech_models"_q);
}

[[nodiscard]] int CountInstalledSpeechModels(const QString &modelsDir) {
	auto result = 0;
	const auto root = QDir(modelsDir);
	for (const auto &spec : SpeechModelSpecs()) {
		if (QFileInfo(root.filePath(spec.folderName)).isDir()) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] int CountReadySpeechArchives(const QString &modelsDir) {
	auto result = 0;
	const auto root = QDir(modelsDir);
	for (const auto &spec : SpeechModelSpecs()) {
		const auto modelPath = root.filePath(spec.folderName);
		const auto archivePath = root.filePath(spec.folderName + u".zip"_q);
		if (!QFileInfo(modelPath).isDir() && QFileInfo::exists(archivePath)) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] QString SpeechModelsSettingsLabel() {
	const auto installed = CountInstalledSpeechModels(SpeechModelsDirectory());
	const auto total = int(SpeechModelSpecs().size());
	return RuEn(
		"Все языки · %1 из %2 установлено",
		"All languages · %1 of %2 installed").arg(installed).arg(total);
}

[[nodiscard]] QString SpeechModelsSummaryText(const QString &modelsDir) {
	const auto installed = CountInstalledSpeechModels(modelsDir);
	const auto ready = CountReadySpeechArchives(modelsDir);
	const auto total = int(SpeechModelSpecs().size());
	if ((installed <= 0) && (ready <= 0)) {
		return RuEn(
			"Пока ничего не скачано. Нажмите значок справа у нужного языка.",
			"Nothing is downloaded yet. Tap the icon on the right for the language you need.");
	} else if (installed >= total) {
		return RuEn(
			"Все модели установлены: %1 из %2.",
			"All models are installed: %1 of %2.").arg(installed).arg(total);
	} else if (ready > 0) {
		return RuEn(
			"Установлено моделей: %1 из %2. Архивов готово к установке: %3.",
			"Installed models: %1 of %2. Archives ready to install: %3.").arg(
				installed).arg(total).arg(ready);
	}
	return RuEn(
		"Установлено моделей: %1 из %2.",
		"Installed models: %1 of %2.").arg(installed).arg(total);
}

[[nodiscard]] QString SpeechModelArchivePath(
		const QString &modelsDir,
		const QString &folderName) {
	return QDir(modelsDir).filePath(folderName + u".zip"_q);
}

[[nodiscard]] QString SpeechModelPartialArchivePath(
		const QString &modelsDir,
		const QString &folderName) {
	return SpeechModelArchivePath(modelsDir, folderName) + u".part"_q;
}

[[nodiscard]] QString SpeechModelStatusIdle(
		bool installed,
		bool archiveReady) {
	if (installed) {
		return RuEn("Установлена", "Installed");
	} else if (archiveReady) {
		return RuEn(
			"Архив скачан, нажмите для установки",
			"Archive ready, tap to install");
	}
	return RuEn(
		"Нажмите значок загрузки справа",
		"Tap the download icon on the right");
}

[[nodiscard]] QString SpeechModelStatusDownloading(float64 progress) {
	return (progress > 0.)
		? RuEn("Скачивание: %1%", "Downloading: %1%").arg(
			int(std::round(progress * 100.)))
		: RuEn("Скачивание...", "Downloading...");
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

void ShowAstrogramUpdateChannelBox(
		not_null<Window::SessionController*> controller) {
	const auto applyChannel = [=](bool devChannel) {
		if (devChannel == cInstallBetaVersion()) {
			return;
		}
		cSetInstallBetaVersion(devChannel);
		Core::Launcher::Instance().writeInstallBetaVersionsSetting();
		cSetLastUpdateCheck(0);
		auto checker = Core::UpdateChecker();
		checker.stop();
		if (cAutoUpdate()) {
			checker.start();
		}
		controller->showToast(devChannel
			? RuEn(
				"Включён канал Dev (beta). Changelog будет браться из prerelease на GitHub.",
				"Dev (beta) channel enabled. Changelog will follow GitHub prereleases.")
			: RuEn(
				"Включён Stable канал обновлений Astrogram.",
				"Stable Astrogram update channel enabled."));
	};

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		SingleChoiceBox(box, {
			.title = rpl::single(RuEn(
				"Скрытый канал обновлений Astrogram",
				"Hidden Astrogram update channel")),
			.options = {
				RuEn("Stable", "Stable"),
				RuEn("Dev (beta)", "Dev (beta)"),
			},
			.initialSelection = cInstallBetaVersion() ? 1 : 0,
			.callback = [=](int index) {
				applyChannel(index == 1);
			},
		});
		const auto summary = Core::ActiveDevUpdateHooksSummary();
		const auto configPath = QDir::toNativeSeparators(
			Core::DevUpdateHooksConfigPath());
		if (cInstallBetaVersion() || (summary.isEmpty() == false)) {
			const auto info = summary.isEmpty()
				? RuEn(
					"Файл dev hook: %1",
					"Dev hook file: %1").arg(configPath)
				: RuEn(
					"Активный dev hook: %1\nФайл: %2",
					"Active dev hook: %1\nFile: %2").arg(summary, configPath);
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(info),
				st::boxLabel),
				style::margins(
					st::boxPadding.left(),
					0,
					st::boxPadding.right(),
					st::boxPadding.bottom() / 2),
				style::al_top);
		}
	}));
}

void AddAstrogramHeader(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto header = container->add(object_ptr<Ui::RpWidget>(container));
	const auto raw = header;
	raw->setMinimumHeight(kHeaderHeight);
	raw->setMaximumHeight(kHeaderHeight);
	const auto secretClicks = raw->lifetime().make_state<int>(0);
	const auto lastSecretClickMs = raw->lifetime().make_state<qint64>(0);

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
	raw->events() | rpl::on_next([=](not_null<QEvent*> event) {
		if (event->type() != QEvent::MouseButtonRelease) {
			return;
		}
		const auto mouse = static_cast<QMouseEvent*>(event.get());
		if (mouse->button() != Qt::LeftButton) {
			return;
		}
		const auto avatarRect = QRect(
			(raw->width() - kAvatarSize) / 2,
			14,
			kAvatarSize,
			kAvatarSize);
		if (!avatarRect.contains(mouse->pos())) {
			return;
		}
		const auto now = QDateTime::currentMSecsSinceEpoch();
		if ((now - *lastSecretClickMs) > kSecretChannelClickWindowMs) {
			*secretClicks = 0;
		}
		*lastSecretClickMs = now;
		++(*secretClicks);
		if (*secretClicks >= kSecretChannelClickThreshold) {
			*secretClicks = 0;
			ShowAstrogramUpdateChannelBox(controller);
		}
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
			rpl::single(placeholder),
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

void ShowNonNegativeIntEditBox(
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QString &placeholder,
		int current,
		Fn<void(int)> save) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(title));

		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::NoNewlines,
			rpl::single(placeholder),
			TextWithTags{ current > 0 ? QString::number(current) : QString(), {} }));

		box->addButton(tr::lng_settings_save(), [=] {
			const auto trimmed = field->getLastText().trimmed();
			auto ok = trimmed.isEmpty();
			const auto value = trimmed.isEmpty() ? 0 : trimmed.toInt(&ok);
			if (!ok || (value < 0)) {
				controller->showToast(RuEn(
					"Введите 0 или положительное число.",
					"Enter 0 or a positive number."));
				return;
			}
			box->closeBox();
			save(value);
		});
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

void RefreshLocalForwardingLimits(
		not_null<Window::SessionController*> controller,
		bool savedGifs,
		bool favedStickers,
		bool recentStickers) {
	auto &session = controller->session();
	session.data().stickers().reapplyLocalLimitOverrides();

	auto &api = session.api();
	if (savedGifs) {
		api.updateSavedGifs();
	}
	if (favedStickers) {
		api.requestSpecialStickersForce(true, false, false);
	}
	if (recentStickers) {
		api.requestSpecialStickersForce(false, true, false);
	}
}

void SyncScheduledEditPersistence(
		not_null<Window::SessionController*> controller) {
	controller->session().api().syncScheduledMessageEditStorage();
}

[[nodiscard]] QString LimitOverrideLabel(int value) {
	return (value > 0)
		? RuEn("До %1 локально", "Up to %1 locally").arg(value)
		: RuEn("Авто (лимит Telegram)", "Auto (Telegram limit)");
}

[[nodiscard]] QString DefaultEditedMarkText() {
	return RuEn("Изменено", "Edited");
}

[[nodiscard]] QString DefaultDeletedMarkText() {
	return RuEn("Удалено", "Deleted");
}

[[nodiscard]] QString DefaultEditedMarkIcon() {
	return QString::fromUtf8("\xE2\x9C\x8E");
}

[[nodiscard]] QString DefaultDeletedMarkIcon() {
	return QString::fromUtf8("\xE2\x9C\x95");
}

[[nodiscard]] QString MarkFieldSummary(
		const QString &value,
		const QString &fallback) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty()
		? RuEn("%1 (по умолчанию)", "%1 (default)").arg(fallback)
		: trimmed;
}

[[nodiscard]] QString MarkPreviewSummary(
		bool showIcon,
		bool showText,
		const QString &icon,
		const QString &text,
		const QString &fallbackIcon,
		const QString &fallbackText) {
	const auto resolvedIcon = icon.trimmed().isEmpty()
		? fallbackIcon
		: icon.trimmed();
	const auto resolvedText = text.trimmed().isEmpty()
		? fallbackText
		: text.trimmed();
	auto preview = QString();
	if (showIcon && !resolvedIcon.isEmpty()) {
		preview += resolvedIcon;
	}
	if (showText && !resolvedText.isEmpty()) {
		if (!preview.isEmpty()) {
			preview += u' ';
		}
		preview += resolvedText;
	}
	return preview.isEmpty()
		? RuEn("Превью скрыто", "Preview hidden")
		: RuEn("Превью: %1", "Preview: %1").arg(preview);
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
			QString archivePath;
			QString partialPath;
			bool installed = false;
			bool downloading = false;
			bool extracting = false;
			float64 progress = 0.;
			QString status;
			QString lastError;
			QPointer<QNetworkReply> reply;
			std::unique_ptr<QFile> output;
			QPointer<Ui::RpWidget> row;
		};

		const auto manager = box->lifetime().make_state<QNetworkAccessManager>();
		auto models = std::vector<std::shared_ptr<ModelRowState>>();
		const auto modelsDir = SpeechModelsDirectory();
		QDir().mkpath(modelsDir);
		const auto summaryText = box->lifetime().make_state<rpl::variable<QString>>(
			SpeechModelsSummaryText(modelsDir));
		const auto refreshSummary = [=] {
			summaryText->force_assign(SpeechModelsSummaryText(modelsDir));
		};


		const auto addModel = [&](const SpeechModelSpec &spec) {
			auto state = std::make_shared<ModelRowState>();
			state->label = spec.label;
			state->folderName = spec.folderName;
			state->url = QUrl(spec.url);
			state->archivePath = SpeechModelArchivePath(modelsDir, state->folderName);
			state->partialPath = SpeechModelPartialArchivePath(modelsDir, state->folderName);
			state->status = SpeechModelStatusIdle(false, false);
			models.push_back(std::move(state));
		};
		for (const auto &spec : SpeechModelSpecs()) {
			addModel(spec);
		}
		Logs::writeClient(QString::fromLatin1(
			"[speech-models] open local speech box: %1 models dir=%2")
			.arg(QString::number(models.size()), QDir::toNativeSeparators(modelsDir)));

		const auto isInstalled = [modelsDir](const std::shared_ptr<ModelRowState> &state) {
			const auto dir = QDir(modelsDir).filePath(state->folderName);
			return QFileInfo(dir).isDir();
		};
		const auto hasArchive = [](const std::shared_ptr<ModelRowState> &state) {
			return QFileInfo::exists(state->archivePath);
		};
		const auto updateRowState = [=](const std::shared_ptr<ModelRowState> &state) {
			state->installed = isInstalled(state);
			if (state->installed) {
				state->downloading = false;
				state->extracting = false;
				state->progress = 1.;
				state->lastError = QString();
				state->status = SpeechModelStatusIdle(true, false);
			} else if (state->extracting) {
				state->status = RuEn("Установка...", "Installing...");
			} else if (state->downloading) {
				state->status = SpeechModelStatusDownloading(state->progress);
			} else if (!state->lastError.isEmpty()) {
				state->status = state->lastError;
			} else {
				state->progress = 0.;
				state->status = SpeechModelStatusIdle(false, hasArchive(state));
			}
			if (state->row) {
				state->row->setCursor(
					(state->installed || state->downloading || state->extracting)
						? style::cur_default
						: style::cur_pointer);
				state->row->update();
			}
			refreshSummary();
		};
		for (const auto &state : models) {
			if (QFileInfo::exists(state->partialPath)) {
				QFile::remove(state->partialPath);
				Logs::writeClient(QString::fromLatin1(
					"[speech-models] removed stale partial archive: %1")
					.arg(state->partialPath));
			}
			updateRowState(state);
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
				"Все языковые модели показаны сразу. У не скачанных моделей справа есть кнопка загрузки, повторная загрузка скрыта после установки, а прогресс идёт тонкой полосой по нижнему торцу карточки. На Windows для языка может дополнительно понадобиться системный speech pack.",
				"All language models are shown in one list. Not-downloaded models keep a download action on the right, re-download is hidden after install, and progress is shown as a thin bar on the bottom edge of the row. Windows may also require the matching system speech pack.")),
			st::boxDividerLabel),
			st::boxRowPadding);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				summaryText->value(),
				st::defaultFlatLabel),
			style::margins(14, 0, 14, 0),
			style::al_top);
		Ui::AddSkip(container);

		for (const auto &state : models) {
			const auto row = container->add(object_ptr<Ui::RpWidget>(container));
			state->row = row;
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
					const auto width = std::max(
						6,
						int(rect.width() * std::clamp(state->progress, 0., 1.)));
					auto progressRect = QRect(0, rect.height() - 3, width, 3);
					p.setBrush(QColor(0x21, 0xc7, 0x6a));
					p.drawRoundedRect(progressRect, 2, 2);
				}
				p.setPen(st::windowFg);
				p.setFont(st::semiboldFont->f);
				p.drawText(QRect(14, 8, rect.width() - 90, 18), Qt::AlignLeft | Qt::AlignVCenter, state->label);
				p.setPen(state->lastError.isEmpty()
					? st::windowSubTextFg->c
					: QColor(0xe1, 0x6b, 0x6b));
				p.setFont(st::defaultFlatLabel.style.font->f);
				p.drawText(QRect(14, 28, rect.width() - 90, 16), Qt::AlignLeft | Qt::AlignVCenter, state->status);
				if (!state->installed && !state->downloading && !state->extracting && !hasArchive(state)) {
					const auto actionSize = 28;
					const auto actionRect = QRect(
						rect.width() - 14 - actionSize,
						(rect.height() - actionSize) / 2,
						actionSize,
						actionSize);
					p.setBrush(QColor(0x21, 0xc7, 0x6a, 26));
					p.drawEllipse(actionRect.adjusted(0, 0, -1, -1));
					st::menuIconDownload.paint(
						p,
						actionRect.x() + (actionSize - st::menuIconDownload.width()) / 2,
						actionRect.y() + (actionSize - st::menuIconDownload.height()) / 2,
						rect.width(),
						QColor(0x21, 0xc7, 0x6a));
				} else if (state->installed) {
					p.setPen(QColor(0x21, 0xc7, 0x6a));
					p.setFont(st::semiboldFont->f);
					p.drawText(QRect(rect.width() - 48, 0, 34, rect.height()), Qt::AlignCenter, QString::fromUtf8("✓"));
				}
			}, row->lifetime());
			updateRowState(state);
			row->events() | rpl::on_next([=](not_null<QEvent*> e) {
				if (e->type() != QEvent::MouseButtonRelease) {
					return;
				}
				const auto mouse = static_cast<QMouseEvent*>(e.get());
				if (mouse->button() != Qt::LeftButton) {
					return;
				}
				if (state->installed || state->downloading || state->extracting) {
					return;
				}
				for (const auto &other : models) {
					if (other->downloading || other->extracting) {
						controller->showToast(RuEn(
							"Дождитесь завершения текущей установки модели.",
							"Wait for the current model installation to finish."));
						return;
					}
				}
				const auto failState = [=](const QString &status, const QString &log) {
					state->downloading = false;
					state->extracting = false;
					state->progress = 0.;
					state->lastError = status;
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] %1: %2")
						.arg(log, state->folderName));
					updateRowState(state);
				};
				const auto extractModel = [=] {
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] extract requested: %1")
						.arg(state->folderName));
					state->lastError = QString();
					state->extracting = true;
					updateRowState(state);
					auto error = QString();
					if (!ExtractSpeechModelArchive(state->archivePath, modelsDir, &error)) {
						QFile::remove(state->archivePath);
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] extract failed: %1 reason=%2")
							.arg(state->folderName, error));
						failState(
							error.isEmpty()
								? RuEn("Ошибка установки", "Install failed")
								: error,
							u"extract failed"_q);
						return;
					}
					if (!isInstalled(state)) {
						QFile::remove(state->archivePath);
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] extract finished without model dir: %1")
							.arg(state->folderName));
						failState(
							RuEn(
								"Модель не найдена после распаковки",
								"Model folder not found after extraction"),
							u"extract missing dir"_q);
						return;
					}
					QFile::remove(state->archivePath);
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] extract finished: %1")
						.arg(state->folderName));
					updateRowState(state);
					controller->showToast(RuEn(
						"Модель установлена: %1",
						"Installed model: %1").arg(state->label));
					return;
				};
				if (hasArchive(state)) {
					extractModel();
					return;
				}
				QDir().mkpath(modelsDir);
				QFile::remove(state->partialPath);
				state->output = std::make_unique<QFile>(state->partialPath);
				if (!state->output->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					failState(
						RuEn("Не удалось создать файл", "Could not create file"),
						u"create file failed"_q);
					return;
				}
				state->lastError = QString();
				state->progress = 0.;
				state->downloading = true;
				Logs::writeClient(QString::fromLatin1(
					"[speech-models] download started: %1 url=%2")
					.arg(state->folderName, state->url.toString()));
				updateRowState(state);
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
					updateRowState(state);
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
						failState(
							RuEn("Неизвестная ошибка", "Unknown error"),
							u"download finished with null reply"_q);
						return;
					}
					if (reply->error() != QNetworkReply::NoError) {
						if (!outputPath.isEmpty()) {
							QFile::remove(outputPath);
						}
						Logs::writeClient(QString::fromLatin1(
							"[speech-models] download failed: %1 reason=%2")
							.arg(state->folderName, reply->errorString()));
						reply->deleteLater();
						failState(
							RuEn("Ошибка загрузки", "Download failed"),
							u"download failed"_q);
						return;
					}
					auto archive = QFile(outputPath);
					if (!archive.rename(state->archivePath)) {
						QFile::remove(outputPath);
						reply->deleteLater();
						failState(
							RuEn(
								"Не удалось сохранить архив модели",
								"Could not finalize model archive"),
							u"rename archive failed"_q);
						return;
					}
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] download complete, extracting: %1")
						.arg(state->folderName));
					Logs::writeClient(QString::fromLatin1(
						"[speech-models] download finished: %1")
						.arg(state->folderName));
					reply->deleteLater();
					extractModel();
				});
			}, row->lifetime());
			container->add(object_ptr<Ui::FixedHeightWidget>(container, st::settingsCheckboxesSkip / 3));
		}

		box->addLeftButton(rpl::single(RuEn(
			"Открыть папку моделей",
			"Open models folder")), [=] {
			QDir().mkpath(modelsDir);
			File::ShowInFolder(modelsDir);
		});
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

[[nodiscard]] not_null<Ui::VerticalLayout*> AddSettingsCard(
		not_null<Ui::VerticalLayout*> container,
		int topMargin = 8) {
	auto card = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		style::margins(6, topMargin, 6, 0),
		style::al_top);
	card->resizeToWidth(container->width());
	Ui::AddDivider(card);
	Ui::AddSkip(card, st::settingsCheckboxesSkip / 2);
	return card;
}

void FinishSettingsCard(not_null<Ui::VerticalLayout*> card) {
	Ui::AddSkip(card, st::settingsCheckboxesSkip / 2);
}

void AddAstrogramUpdateSection(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	if (Core::UpdaterDisabled()) {
		return;
	}

	AddSectionGroupTitle(container, RuEn(
		"Обновления Astrogram",
		"Astrogram updates"));
	const auto card = AddSettingsCard(container);
	const auto checker = card->lifetime().make_state<Core::UpdateChecker>();
	const auto statusText = card->lifetime().make_state<rpl::variable<QString>>(
		QString());
	const auto detailsText = card->lifetime().make_state<rpl::variable<QString>>(
		QString());
	const auto hookText = card->lifetime().make_state<rpl::variable<QString>>(
		QString());
	const auto changelogText = card->lifetime().make_state<rpl::variable<QString>>(
		QString());
	const auto releaseUrl = card->lifetime().make_state<QString>(
		Core::CurrentUpdateFeedPageUrl());

	AddToggle(
		card,
		rpl::single(cAutoUpdate()),
		RuEn("Автообновления Astrogram", "Astrogram auto-updates"),
		[=](bool toggled) {
			cSetAutoUpdate(toggled);
			Local::writeSettings();
			if (toggled) {
				cSetLastUpdateCheck(0);
				checker->start();
			} else {
				checker->stop();
			}
		});
	Ui::AddSkip(card, st::settingsCheckboxesSkip / 4);

	card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			statusText->value(),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	const auto detailsLabel = card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			detailsText->value(),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	const auto hookLabel = card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			hookText->value(),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	const auto changelogLabel = card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			changelogText->value(),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	Ui::AddSkip(card, st::settingsCheckboxesSkip / 4);

	AddActionButton(
		card,
		RuEn("Проверить обновления сейчас", "Check for updates now"),
		[=] {
			if (!cAutoUpdate()) {
				controller->showToast(RuEn(
					"Сначала включите автообновления Astrogram.",
					"Enable Astrogram auto-updates first."));
				return;
			}
			cSetLastUpdateCheck(0);
			Local::writeSettings();
			checker->start();
		},
		{ &st::menuIconRestore });
	const auto openRelease = AddButtonWithIcon(
		card,
		rpl::single(RuEn(
			"Открыть страницу релизов",
			"Open releases page")),
		st::settingsButton,
		{ &st::menuIconIpAddress });
	openRelease->addClickHandler([=] {
		if (!releaseUrl->isEmpty()) {
			QDesktopServices::openUrl(QUrl(*releaseUrl));
		}
	});
	const auto installUpdate = AddButtonWithIcon(
		card,
		rpl::single(RuEn(
			"Установить обновление",
			"Install update")),
		st::settingsButton,
		{ &st::menuIconDownload });
	installUpdate->addClickHandler([=] {
		if (!Core::UpdaterDisabled()) {
			Core::checkReadyUpdate();
		}
		Core::Restart();
	});

	const auto refresh = [=] {
		const auto info = checker->releaseInfo();
		*releaseUrl = info.url.isEmpty()
			? Core::CurrentUpdateFeedPageUrl()
			: info.url;
		statusText->force_assign(AstrogramUpdateHeadline(*checker, info));
		detailsText->force_assign(AstrogramUpdateDetails(info));
		hookText->force_assign(Core::DescribeDevUpdateHooksState());
		changelogText->force_assign(AstrogramUpdateChangelogText(info));
		detailsLabel->setVisible(detailsText->current().isEmpty() == false);
		hookLabel->setVisible(hookText->current().isEmpty() == false);
		changelogLabel->setVisible(changelogText->current().isEmpty() == false);
		openRelease->setVisible(releaseUrl->isEmpty() == false);
		installUpdate->setVisible(
			checker->state() == Core::UpdateChecker::State::Ready);
	};

	refresh();
	checker->checking() | rpl::on_next(refresh, card->lifetime());
	checker->progress() | rpl::on_next([=](auto) {
		refresh();
	}, card->lifetime());
	checker->isLatest() | rpl::on_next(refresh, card->lifetime());
	checker->failed() | rpl::on_next(refresh, card->lifetime());
	checker->ready() | rpl::on_next(refresh, card->lifetime());
	checker->releaseInfoChanged() | rpl::on_next(refresh, card->lifetime());
}

void SetupAstrogramHome(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	AddAstrogramHeader(controller, container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddAstrogramUpdateSection(controller, container);
	Ui::AddSkip(container);
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
	AddSectionGroup(
		controller,
		container,
		RuEn("Эксперименты", "Experiments"),
		{
			{ RuEn("Экспериментальные", "Experimental"), Experimental::Id(), { &st::menuIconExperimental } },
		});
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Ссылки", "Links"));
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddLinksSection(controller, container);
}

void SetupAstrogramCore(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Аккаунт и возможности", "Account & features"));
	const auto accountCard = AddSettingsCard(container);
	AddToggle(
		accountCard,
		settings.localPremiumValue(),
		RuEn("Локальный премиум", "Local Premium"),
		[&](bool toggled) { settings.setLocalPremium(toggled); });
	AddToggle(
		accountCard,
		settings.disableAdsValue(),
		RuEn("Скрывать рекламу и спонсорские блоки", "Hide ads and sponsored"),
		[&](bool toggled) { settings.setDisableAds(toggled); });
	FinishSettingsCard(accountCard);
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Пересылка и ограничения", "Forwarding & limits"));
	const auto forwardingCard = AddSettingsCard(container);
	AddToggle(
		forwardingCard,
		settings.unlockForwardSelectionLimitValue(),
		RuEn(
			"Снять локальный лимит 100 сообщений при пересылке",
			"Remove local 100-message forward limit"),
		[&](bool toggled) {
			settings.setUnlockForwardSelectionLimit(toggled);
		});
	AddToggle(
		forwardingCard,
		settings.persistLocalScheduledEditsValue(),
		RuEn(
			"Сохранять локальные отложенные правки после перезапуска",
			"Keep local scheduled edits after restart"),
		[=](bool toggled) {
			auto &settings = Core::App().settings();
			settings.setPersistLocalScheduledEdits(toggled);
			SyncScheduledEditPersistence(controller);
			Core::App().saveSettings();
		});
	AddButtonWithLabel(
		forwardingCard,
		rpl::single(RuEn("Локальный лимит сохранённых GIF", "Local saved GIF limit")),
		settings.localSavedGifsLimitOverrideValue() | rpl::map([](int value) {
			return LimitOverrideLabel(value);
		}),
		st::settingsButton,
		{ &st::menuIconGif }
	)->addClickHandler([=] {
		ShowNonNegativeIntEditBox(
			controller,
			RuEn("Локальный лимит сохранённых GIF", "Local saved GIF limit"),
			RuEn("0 = авто, число = локальный лимит", "0 = auto, number = local limit"),
			Core::App().settings().localSavedGifsLimitOverride(),
			[=](int value) {
				auto &settings = Core::App().settings();
				settings.setLocalSavedGifsLimitOverride(value);
				Core::App().saveSettings();
				RefreshLocalForwardingLimits(controller, true, false, false);
			});
	});
	AddButtonWithLabel(
		forwardingCard,
		rpl::single(RuEn("Локальный лимит избранных стикеров", "Local favourite stickers limit")),
		settings.localFavedStickersLimitOverrideValue() | rpl::map([](int value) {
			return LimitOverrideLabel(value);
		}),
		st::settingsButton,
		{ &st::menuIconStickers }
	)->addClickHandler([=] {
		ShowNonNegativeIntEditBox(
			controller,
			RuEn("Локальный лимит избранных стикеров", "Local favourite stickers limit"),
			RuEn("0 = авто, число = локальный лимит", "0 = auto, number = local limit"),
			Core::App().settings().localFavedStickersLimitOverride(),
			[=](int value) {
				auto &settings = Core::App().settings();
				settings.setLocalFavedStickersLimitOverride(value);
				Core::App().saveSettings();
				RefreshLocalForwardingLimits(controller, false, true, false);
			});
	});
	AddButtonWithLabel(
		forwardingCard,
		rpl::single(RuEn("Локальный лимит недавних стикеров", "Local recent stickers limit")),
		settings.localRecentStickersLimitOverrideValue() | rpl::map([](int value) {
			return LimitOverrideLabel(value);
		}),
		st::settingsButton,
		{ &st::menuIconStickers }
	)->addClickHandler([=] {
		ShowNonNegativeIntEditBox(
			controller,
			RuEn("Локальный лимит недавних стикеров", "Local recent stickers limit"),
			RuEn("0 = авто, число = локальный лимит", "0 = auto, number = local limit"),
			Core::App().settings().localRecentStickersLimitOverride(),
			[=](int value) {
				auto &settings = Core::App().settings();
				settings.setLocalRecentStickersLimitOverride(value);
				Core::App().saveSettings();
				RefreshLocalForwardingLimits(controller, false, false, true);
			});
	});
	Ui::AddSkip(forwardingCard, st::settingsCheckboxesSkip / 4);
	forwardingCard->add(
		object_ptr<Ui::FlatLabel>(
			forwardingCard,
			rpl::single(RuEn(
				"Пересылка снимает только клиентский лимит выделения. GIF и стикерные override работают локально: Astrogram удерживает расширенный хвост на этом устройстве, но серверные лимиты Telegram и синхронизация всё равно могут быть ниже.",
				"Forwarding removes only the client-side selection cap. GIF and sticker overrides are local: Astrogram keeps an extended tail on this device, but Telegram server limits and sync may still stay lower.")),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	FinishSettingsCard(forwardingCard);
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Боковое меню", "Side menu"));
	const auto sideMenuCard = AddSettingsCard(container);
	AddToggle(
		sideMenuCard,
		settings.mainMenuAccountsShownValue(),
		RuEn("Показывать аккаунты в боковом меню", "Show accounts in side menu"),
		[&](bool toggled) { settings.setMainMenuAccountsShown(toggled); });
	FinishSettingsCard(sideMenuCard);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramPrivacy(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Режим призрака", "Ghost mode"));
	const auto ghostCard = AddSettingsCard(container);
	AddToggle(
		ghostCard,
		settings.ghostModeValue(),
		RuEn("Режим призрака", "Ghost mode"),
		[&](bool toggled) { settings.setGhostMode(toggled); });
	AddToggle(
		ghostCard,
		settings.ghostHideReadMessagesValue(),
		RuEn("Не читать сообщения", "Don't read messages"),
		[&](bool toggled) { settings.setGhostHideReadMessages(toggled); });
	AddToggle(
		ghostCard,
		settings.ghostHideOnlineStatusValue(),
		RuEn("Не отправлять статус «в сети»", "Don't send online packets"),
		[&](bool toggled) { settings.setGhostHideOnlineStatus(toggled); });
	AddToggle(
		ghostCard,
		settings.ghostHideTypingProgressValue(),
		RuEn("Не отправлять набор текста и ход загрузки", "Don't send typing/upload progress"),
		[&](bool toggled) { settings.setGhostHideTypingProgress(toggled); });
	FinishSettingsCard(ghostCard);
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("История и защита", "History & protection"));
	const auto historyCard = AddSettingsCard(container);
	AddToggle(
		historyCard,
		settings.saveDeletedMessagesValue(),
		RuEn("Сохранять удалённые сообщения", "Keep deleted messages"),
		[&](bool toggled) { settings.setSaveDeletedMessages(toggled); });
	AddToggle(
		historyCard,
		settings.saveMessagesHistoryValue(),
		RuEn("Сохранять историю правок", "Keep edit history"),
		[&](bool toggled) { settings.setSaveMessagesHistory(toggled); });
	AddToggle(
		historyCard,
		settings.semiTransparentDeletedMessagesValue(),
		RuEn("Полупрозрачные удалённые сообщения", "Semi-transparent deleted messages"),
		[&](bool toggled) { settings.setSemiTransparentDeletedMessages(toggled); });
	FinishSettingsCard(historyCard);
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Отметки изменений и удаления", "Edited & deleted tags"));
	const auto editedTagCard = AddSettingsCard(container);
	AddSectionGroupTitle(
		editedTagCard,
		RuEn("Изменённое сообщение", "Edited message"));
	AddToggle(
		editedTagCard,
		settings.editedMarkShowTextValue(),
		RuEn("Показывать текст у отметки изменения", "Show text on edited tag"),
		[&](bool toggled) { settings.setEditedMarkShowText(toggled); });
	AddToggle(
		editedTagCard,
		settings.editedMarkShowIconValue(),
		RuEn("Показывать значок у отметки изменения", "Show icon on edited tag"),
		[&](bool toggled) { settings.setEditedMarkShowIcon(toggled); });
	AddButtonWithLabel(
		editedTagCard,
		rpl::single(RuEn("Текст отметки изменения", "Edited tag text")),
		settings.editedMarkTextValue() | rpl::map([](const QString &value) {
			return MarkFieldSummary(value, DefaultEditedMarkText());
		}),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		ShowSingleLineTextEditBox(
			controller,
			RuEn("Текст отметки изменения", "Edited tag text"),
			DefaultEditedMarkText(),
			Core::App().settings().editedMarkText(),
			[](QString value) {
				auto &settings = Core::App().settings();
				settings.setEditedMarkText(std::move(value));
				Core::App().saveSettings();
			});
	});
	AddButtonWithLabel(
		editedTagCard,
		rpl::single(RuEn("Значок отметки изменения", "Edited tag icon")),
		settings.editedMarkIconValue() | rpl::map([](const QString &value) {
			return MarkFieldSummary(value, DefaultEditedMarkIcon());
		}),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		ShowSingleLineTextEditBox(
			controller,
			RuEn("Значок отметки изменения", "Edited tag icon"),
			DefaultEditedMarkIcon(),
			Core::App().settings().editedMarkIcon(),
			[](QString value) {
				auto &settings = Core::App().settings();
				settings.setEditedMarkIcon(std::move(value));
				Core::App().saveSettings();
			});
	});
	editedTagCard->add(
		object_ptr<Ui::FlatLabel>(
			editedTagCard,
			rpl::combine(
				settings.editedMarkShowIconValue(),
				settings.editedMarkShowTextValue(),
				settings.editedMarkIconValue(),
				settings.editedMarkTextValue()) | rpl::map([](
						bool showIcon,
						bool showText,
						const QString &icon,
						const QString &text) {
					return MarkPreviewSummary(
						showIcon,
						showText,
						icon,
						text,
						DefaultEditedMarkIcon(),
						DefaultEditedMarkText());
				}),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	AddActionButton(
		editedTagCard,
		RuEn("Сбросить отметку изменения", "Reset edited tag"),
		[] {
			auto &settings = Core::App().settings();
			settings.setEditedMarkShowText(true);
			settings.setEditedMarkShowIcon(true);
			settings.setEditedMarkText(QString());
			settings.setEditedMarkIcon(DefaultEditedMarkIcon());
			Core::App().saveSettings();
		},
		{ &st::menuIconRestore });
	FinishSettingsCard(editedTagCard);
	Ui::AddSkip(container);
	const auto deletedTagCard = AddSettingsCard(container);
	AddSectionGroupTitle(
		deletedTagCard,
		RuEn("Удалённое сообщение", "Deleted message"));
	AddToggle(
		deletedTagCard,
		settings.deletedMarkShowTextValue(),
		RuEn("Показывать текст у отметки удаления", "Show text on deleted tag"),
		[&](bool toggled) { settings.setDeletedMarkShowText(toggled); });
	AddToggle(
		deletedTagCard,
		settings.deletedMarkShowIconValue(),
		RuEn("Показывать значок у отметки удаления", "Show icon on deleted tag"),
		[&](bool toggled) { settings.setDeletedMarkShowIcon(toggled); });
	AddButtonWithLabel(
		deletedTagCard,
		rpl::single(RuEn("Текст отметки удаления", "Deleted tag text")),
		settings.deletedMarkTextValue() | rpl::map([](const QString &value) {
			return MarkFieldSummary(value, DefaultDeletedMarkText());
		}),
		st::settingsButton,
		{ &st::menuIconDelete }
	)->addClickHandler([=] {
		ShowSingleLineTextEditBox(
			controller,
			RuEn("Текст отметки удаления", "Deleted tag text"),
			DefaultDeletedMarkText(),
			Core::App().settings().deletedMarkText(),
			[](QString value) {
				auto &settings = Core::App().settings();
				settings.setDeletedMarkText(std::move(value));
				Core::App().saveSettings();
			});
	});
	AddButtonWithLabel(
		deletedTagCard,
		rpl::single(RuEn("Значок отметки удаления", "Deleted tag icon")),
		settings.deletedMarkIconValue() | rpl::map([](const QString &value) {
			return MarkFieldSummary(value, DefaultDeletedMarkIcon());
		}),
		st::settingsButton,
		{ &st::menuIconDelete }
	)->addClickHandler([=] {
		ShowSingleLineTextEditBox(
			controller,
			RuEn("Значок отметки удаления", "Deleted tag icon"),
			DefaultDeletedMarkIcon(),
			Core::App().settings().deletedMarkIcon(),
			[](QString value) {
				auto &settings = Core::App().settings();
				settings.setDeletedMarkIcon(std::move(value));
				Core::App().saveSettings();
			});
	});
	deletedTagCard->add(
		object_ptr<Ui::FlatLabel>(
			deletedTagCard,
			rpl::combine(
				settings.deletedMarkShowIconValue(),
				settings.deletedMarkShowTextValue(),
				settings.deletedMarkIconValue(),
				settings.deletedMarkTextValue()) | rpl::map([](
						bool showIcon,
						bool showText,
						const QString &icon,
						const QString &text) {
					return MarkPreviewSummary(
						showIcon,
						showText,
						icon,
						text,
						DefaultDeletedMarkIcon(),
						DefaultDeletedMarkText());
				}),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	AddActionButton(
		deletedTagCard,
		RuEn("Сбросить отметку удаления", "Reset deleted tag"),
		[] {
			auto &settings = Core::App().settings();
			settings.setDeletedMarkShowText(true);
			settings.setDeletedMarkShowIcon(true);
			settings.setDeletedMarkText(QString());
			settings.setDeletedMarkIcon(DefaultDeletedMarkIcon());
			Core::App().saveSettings();
		},
		{ &st::menuIconRestore });
	FinishSettingsCard(deletedTagCard);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramInterface(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Окно и навигация", "Window & navigation"));
	const auto windowCard = AddSettingsCard(container);
	AddToggle(
		windowCard,
		settings.adaptiveForWideValue(),
		RuEn("Адаптивный широкий макет", "Adaptive wide layout"),
		[&](bool toggled) { settings.setAdaptiveForWide(toggled); });
	AddToggle(
		windowCard,
		settings.systemDarkModeEnabledValue(),
		RuEn("Автоматическая тёмная тема по системе", "Auto dark mode from system"),
		[&](bool toggled) { settings.setSystemDarkModeEnabled(toggled); });
	AddToggle(
		windowCard,
		settings.disableStoriesValue(),
		RuEn("Скрыть истории", "Hide stories"),
		[&](bool toggled) { settings.setDisableStories(toggled); });
	AddToggle(
		windowCard,
		settings.disableOpenLinkWarningValue(),
		RuEn("Не спрашивать перед открытием ссылок", "Skip link warning"),
		[&](bool toggled) { settings.setDisableOpenLinkWarning(toggled); });
	AddToggle(
		windowCard,
		settings.showMessageSecondsValue(),
		RuEn("Показывать секунды во времени", "Show message seconds"),
		[&](bool toggled) { settings.setShowMessageSeconds(toggled); });
	FinishSettingsCard(windowCard);
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Перевод и речь", "Translation & speech"));
	const auto speechCard = AddSettingsCard(container);
	AddToggle(
		speechCard,
		rpl::single(settings.translateButtonEnabled()),
		RuEn("Показывать кнопку перевода", "Show translate button"),
		[&](bool toggled) { settings.setTranslateButtonEnabled(toggled); });
	AddToggle(
		speechCard,
		settings.translateChatEnabledValue(),
		RuEn("Переводить чат целиком", "Translate whole chat"),
		[&](bool toggled) { settings.setTranslateChatEnabled(toggled); });
	AddButtonWithLabel(
		speechCard,
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
	AddButtonWithLabel(
		speechCard,
		rpl::single(RuEn("Локальное распознавание речи", "Local speech recognition")),
		rpl::single(SpeechModelsSettingsLabel()),
		st::settingsButton,
		{ &st::menuIconDownload }
	)->addClickHandler([=] {
		ShowSpeechModelDownloadBox(controller);
	});
	Ui::AddSkip(speechCard, st::settingsCheckboxesSkip / 4);
	speechCard->add(
		object_ptr<Ui::FlatLabel>(
			speechCard,
			rpl::single(RuEn(
				"Открывается отдельное окно со всеми языками сразу: у не скачанных моделей справа есть значок загрузки, прогресс идёт тонкой полосой снизу, после установки повторная загрузка скрывается.",
				"A separate box shows all languages at once: not-downloaded models keep a download icon on the right, progress is shown as a thin bar at the bottom, and re-download is hidden after install.")),
			st::defaultFlatLabel),
		style::margins(14, 0, 14, 0),
		style::al_top);
	FinishSettingsCard(speechCard);
	Ui::AddSkip(container);
	AddSectionGroupTitle(container, RuEn("Текст и ввод", "Text & input"));
	const auto inputCard = AddSettingsCard(container);
	AddToggle(
		inputCard,
		settings.localOnlyDraftsValue(),
		RuEn("Локальные черновики (без облака)", "Local drafts only (no cloud sync)"),
		[&](bool toggled) { settings.setLocalOnlyDrafts(toggled); });
	AddToggle(
		inputCard,
		settings.collapseSimilarChannelsValue(),
		RuEn("Сворачивать похожие каналы", "Collapse similar channels"),
		[&](bool toggled) { settings.setCollapseSimilarChannels(toggled); });
	AddToggle(
		inputCard,
		settings.hideSimilarChannelsValue(),
		RuEn("Скрыть похожие каналы", "Hide similar channels"),
		[&](bool toggled) { settings.setHideSimilarChannels(toggled); });
	AddToggle(
		inputCard,
		settings.largeEmojiValue(),
		RuEn("Крупные эмодзи", "Large emoji"),
		[&](bool toggled) { settings.setLargeEmoji(toggled); });
	AddToggle(
		inputCard,
		settings.replaceEmojiValue(),
		RuEn("Автозамена эмодзи", "Auto replace emoji"),
		[&](bool toggled) { settings.setReplaceEmoji(toggled); });
	AddToggle(
		inputCard,
		settings.cornerReactionValue(),
		RuEn("Быстрая реакция в углу", "Corner quick reaction"),
		[&](bool toggled) { settings.setCornerReaction(toggled); });
	AddToggle(
		inputCard,
		settings.spellcheckerEnabledValue(),
		RuEn("Проверка орфографии", "Spell checker"),
		[&](bool toggled) { settings.setSpellcheckerEnabled(toggled); });
	AddToggle(
		inputCard,
		settings.autoDownloadDictionariesValue(),
		RuEn("Автозагрузка словарей", "Auto download dictionaries"),
		[&](bool toggled) { settings.setAutoDownloadDictionaries(toggled); });
	FinishSettingsCard(inputCard);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void SetupAstrogramAntiRecall(not_null<Ui::VerticalLayout*> container) {
	auto &settings = Core::App().settings();
	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	AddSectionGroupTitle(container, RuEn("Журнал защиты от удаления", "Anti-recall log"));
	const auto card = AddSettingsCard(container);
	AddToggle(
		card,
		settings.saveDeletedMessagesValue(),
		RuEn("Сохранять удалённые сообщения", "Keep deleted messages"),
		[&](bool toggled) { settings.setSaveDeletedMessages(toggled); });
	AddToggle(
		card,
		settings.saveMessagesHistoryValue(),
		RuEn("Сохранять историю правок", "Keep edit history"),
		[&](bool toggled) { settings.setSaveMessagesHistory(toggled); });
	AddToggle(
		card,
		settings.semiTransparentDeletedMessagesValue(),
		RuEn("Полупрозрачные удалённые сообщения", "Semi-transparent deleted messages"),
		[&](bool toggled) { settings.setSemiTransparentDeletedMessages(toggled); });
	AddActionButton(
		card,
		RuEn("Показать журнал", "Show local log"),
		[] { File::ShowInFolder(u"./tdata/astro_recall_log.jsonl"_q); },
		{ &st::menuIconShowInFolder });
	FinishSettingsCard(card);
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
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	SetupAstrogramCore(controller, content);
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
