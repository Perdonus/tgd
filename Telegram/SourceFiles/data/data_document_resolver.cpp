/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_document_resolver.h"

#include "base/options.h"
#include "base/platform/base_platform_info.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "chat_helpers/ttl_media_layer_widget.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/media/history_view_gif.h"
#include "lang/lang_keys.h"
#include "lang/lang_text_entity.h"
#include "main/main_session.h"
#include "media/player/media_player_instance.h"
#include "platform/platform_file_utilities.h"
#include "plugins/plugins_manager.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_theme.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeType>
#include <QtCore/QMimeDatabase>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

#include <utility>

namespace Data {
namespace {

base::options::toggle OptionExternalVideoPlayer({
	.id = kOptionExternalVideoPlayer,
	.name = "External video player",
	.description = "Use system video player instead of the internal one. "
		"This disabes video playback in messages.",
});

constexpr auto kPluginPackageExtension = ".tgd";
constexpr auto kPluginIncomingFolder = "tdata/plugins/.incoming";
constexpr auto kPluginIconSize = 72;
constexpr auto kPluginIconRowHeight = 88;

struct PluginIconSpec {
	QString packShortName;
	int stickerIndex = -1;

	[[nodiscard]] bool valid() const {
		return !packShortName.isEmpty() && stickerIndex >= 0;
	}
};

PluginIconSpec ParsePluginIconSpec(QString value) {
	value = value.trimmed();
	const auto slash = value.indexOf('/');
	if (slash <= 0 || slash + 1 >= value.size()) {
		return {};
	}
	auto result = PluginIconSpec();
	result.packShortName = value.mid(0, slash).trimmed();
	result.stickerIndex = value.mid(slash + 1).trimmed().toInt();
	if (!result.valid()) {
		return {};
	}
	return result;
}

QString PluginPackageDisplayName(not_null<DocumentData*> document) {
	const auto filename = document->filename().trimmed();
	if (!filename.isEmpty()) {
		return filename;
	}
	const auto filepath = document->filepath(true);
	if (!filepath.isEmpty()) {
		return QFileInfo(filepath).fileName();
	}
	return QString::number(document->id) + QString::fromLatin1(kPluginPackageExtension);
}

bool IsPluginPackage(not_null<DocumentData*> document) {
	return PluginPackageDisplayName(document).endsWith(
		QString::fromLatin1(kPluginPackageExtension),
		Qt::CaseInsensitive);
}

QString PluginIncomingPath(not_null<DocumentData*> document) {
	auto filename = QFileInfo(PluginPackageDisplayName(document)).fileName();
	if (!filename.endsWith(QString::fromLatin1(kPluginPackageExtension), Qt::CaseInsensitive)) {
		filename += QString::fromLatin1(kPluginPackageExtension);
	}
	const auto dir = QDir(cWorkingDir() + QString::fromLatin1(kPluginIncomingFolder));
	QDir().mkpath(dir.path());
	return dir.filePath(QString::number(document->id) + u"-"_q + filename);
}

QString LocalPluginPackagePath(not_null<DocumentData*> document) {
	const auto filepath = document->filepath(true);
	if (!filepath.isEmpty() && QFileInfo::exists(filepath)) {
		return filepath;
	}
	const auto media = document->createMediaView();
	const auto bytes = media->bytes();
	if (bytes.isEmpty()) {
		return QString();
	}
	const auto path = PluginIncomingPath(document);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return QString();
	}
	if (file.write(bytes) != bytes.size()) {
		file.close();
		QFile::remove(path);
		return QString();
	}
	file.close();
	return path;
}

[[nodiscard]] bool UseRussianPluginUi();
[[nodiscard]] QString PluginUiText(QString en, QString ru);

TextWithEntities PluginVersionText(const Plugins::PackagePreviewState &preview) {
	auto result = TextWithEntities();
	result.append(UseRussianPluginUi() ? u"Версия: "_q : u"Version: "_q);
	const auto oldVersion = preview.installedVersion.trimmed();
	const auto newVersion = preview.info.version.trimmed();
	if (preview.update && !oldVersion.isEmpty()) {
		result.append(tr::strikeout(oldVersion));
		if (!newVersion.isEmpty()) {
			result.append(u"  "_q);
		}
	}
	result.append(newVersion.isEmpty()
		? (UseRussianPluginUi() ? u"неизвестно"_q : u"unknown"_q)
		: newVersion);
	return result;
}

QString PluginPackageButtonText(const Plugins::PackagePreviewState &preview) {
	return preview.update
		? (UseRussianPluginUi() ? u"Обновить"_q : u"Update"_q)
		: (UseRussianPluginUi() ? u"Установить"_q : u"Install"_q);
}

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

[[nodiscard]] bool IsTelegramHandleChar(QChar ch) {
	return ch.isLetterOrNumber() || (ch == QChar::fromLatin1('_'));
}

[[nodiscard]] QString NormalizedTelegramHandle(QString value) {
	value = value.trimmed();
	if (value.startsWith(QChar::fromLatin1('@'))) {
		value.remove(0, 1);
	}
	if (value.isEmpty()) {
		return QString();
	}
	for (const auto ch : value) {
		if (!IsTelegramHandleChar(ch)) {
			return QString();
		}
	}
	return value;
}

[[nodiscard]] TextWithEntities PluginAuthorText(const QString &author) {
	const auto trimmed = author.trimmed();
	auto result = TextWithEntities{
		PluginUiText(u"Author: "_q, u"Автор: "_q) + trimmed
	};
	const auto offset = result.text.size() - trimmed.size();
	if (const auto handle = NormalizedTelegramHandle(trimmed); !handle.isEmpty()) {
		result.entities.push_back({
			EntityType::CustomUrl,
			offset,
			trimmed.size(),
			u"https://t.me/"_q + handle,
		});
		return result;
	}
	for (auto i = 0; i < trimmed.size();) {
		if (trimmed[i] != QChar::fromLatin1('@')) {
			++i;
			continue;
		}
		auto j = i + 1;
		while (j < trimmed.size() && IsTelegramHandleChar(trimmed[j])) {
			++j;
		}
		if (j > (i + 1)) {
			result.entities.push_back({
				EntityType::CustomUrl,
				offset + i,
				j - i,
				u"https://t.me/"_q + trimmed.mid(i + 1, j - i - 1),
			});
		}
		i = std::max(j, i + 1);
	}
	return result;
}

void WireExternalLinks(not_null<Ui::FlatLabel*> label) {
	label->setClickHandlerFilter([=](const auto &handler, auto) {
		const auto entity = handler->getTextEntity();
		if (entity.type != EntityType::CustomUrl) {
			return true;
		}
		QDesktopServices::openUrl(QUrl(entity.data));
		return false;
	});
}

class PluginPackageIcon final : public Ui::RpWidget {
public:
	PluginPackageIcon(
		QWidget *parent,
		not_null<Main::Session*> session,
		QString title,
		QString iconSpec)
	: Ui::RpWidget(parent)
	, _session(session)
	, _title(std::move(title))
	, _iconSpec(ParsePluginIconSpec(std::move(iconSpec))) {
		setMinimumHeight(kPluginIconRowHeight);
		setMaximumHeight(kPluginIconRowHeight);

		paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(this);
			const auto image = currentImage();
			const auto target = QRect(
				(width() - kPluginIconSize) / 2,
				(height() - kPluginIconSize) / 2,
				kPluginIconSize,
				kPluginIconSize);
			p.setRenderHint(QPainter::Antialiasing);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 22));
			p.drawRoundedRect(target, 18, 18);
			if (image) {
				const auto pixmap = image->pix(kPluginIconSize, kPluginIconSize);
				p.drawPixmap(target.topLeft(), pixmap);
				return;
			}
			auto font = this->font();
			font.setPointSize(font.pointSize() + 8);
			font.setBold(true);
			p.setFont(font);
			p.setPen(palette().color(QPalette::WindowText));
			const auto letter = !_title.trimmed().isEmpty()
				? _title.trimmed().left(1).toUpper()
				: u"P"_q;
			p.drawText(target, Qt::AlignCenter, letter);
		}, lifetime());

		_session->downloaderTaskFinished(
		) | rpl::on_next([=] {
			update();
		}, lifetime());

		requestIcon();
	}

	~PluginPackageIcon() override {
		if (_requestId) {
			const auto requestId = _requestId;
			_requestId = 0;
			_session->api().request(requestId).cancel();
		}
	}

private:
	void requestIcon() {
		if (!_iconSpec.valid()) {
			return;
		}
		_requestId = _session->api().request(MTPmessages_GetStickerSet(
			Data::InputStickerSet(StickerSetIdentifier{
				.shortName = _iconSpec.packShortName,
			}),
			MTP_int(0)
		)).done(crl::guard(this, [=](const MTPmessages_StickerSet &result) {
			_requestId = 0;
			result.match([&](const MTPDmessages_stickerSet &data) {
				if (const auto set = _session->data().stickers().feedSetFull(data)) {
					if (_iconSpec.stickerIndex >= 0
						&& _iconSpec.stickerIndex < set->stickers.size()) {
						_sticker = set->stickers[_iconSpec.stickerIndex];
						_stickerMedia = _sticker->createMediaView();
						_stickerMedia->thumbnailWanted(_sticker->stickerSetOrigin());
					}
					if (set->hasThumbnail()) {
						_thumbnailView = set->createThumbnailView();
						set->loadThumbnail();
					}
				}
				update();
			}, [&](const MTPDmessages_stickerSetNotModified &) {
				update();
			});
		})).fail(crl::guard(this, [=] {
			_requestId = 0;
			update();
		})).send();
	}

	Image *currentImage() const {
		if (_stickerMedia) {
			if (const auto image = _stickerMedia->thumbnail()) {
				return image;
			}
		}
		return _thumbnailView ? _thumbnailView->image() : nullptr;
	}

	const not_null<Main::Session*> _session;
	QString _title;
	PluginIconSpec _iconSpec;
	mtpRequestId _requestId = 0;
	DocumentData *_sticker = nullptr;
	std::shared_ptr<Data::DocumentMedia> _stickerMedia;
	std::shared_ptr<Data::StickersSetThumbnailView> _thumbnailView;
};

void ShowPluginPackageBox(
		not_null<Window::SessionController*> controller,
		const Plugins::PackagePreviewState &preview) {
	const auto title = !preview.info.name.trimmed().isEmpty()
		? preview.info.name.trimmed()
		: (!preview.info.id.trimmed().isEmpty()
			? preview.info.id.trimmed()
			: QFileInfo(preview.sourcePath).fileName());
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			preview.update
				? PluginUiText(u"Update Plugin"_q, u"Обновить плагин"_q)
				: PluginUiText(u"Install Plugin"_q, u"Установить плагин"_q)));

		box->addRow(object_ptr<PluginPackageIcon>(
			box,
			&controller->session(),
			title,
			preview.icon));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(title),
			st::sessionBigName),
			style::margins(st::boxPadding.left(), 0, st::boxPadding.right(), 0),
			style::al_top);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(PluginVersionText(preview)),
			st::sessionDateLabel),
			style::margins(st::boxPadding.left(), 0, st::boxPadding.right(), 0),
			style::al_top);
		if (!preview.info.author.trimmed().isEmpty()) {
			const auto authorLabel = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(PluginAuthorText(preview.info.author)),
				st::defaultFlatLabel),
				style::margins(st::boxPadding.left(), 0, st::boxPadding.right(), 0),
				style::al_top);
			WireExternalLinks(authorLabel);
		}
		if (!preview.info.description.trimmed().isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(preview.info.description.trimmed()),
				st::boxLabel),
				style::margins(st::boxPadding.left(), 0, st::boxPadding.right(), 0),
				style::al_top);
		}
		if (!preview.error.trimmed().isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(preview.error.trimmed()),
				st::boxLabel),
				style::margins(
					st::boxPadding.left(),
					st::boxPadding.bottom() / 2,
					st::boxPadding.right(),
					0),
				style::al_top);
		}

		box->addLeftButton(rpl::single(PluginUiText(u"Cancel"_q, u"Отмена"_q)), [=] {
			box->closeBox();
		});

		if (preview.compatible) {
			box->addButton(rpl::single(PluginPackageButtonText(preview)), [=] {
				auto error = QString();
				if (!Core::App().plugins().installPackage(preview.sourcePath, &error)) {
					controller->showToast(
						error.isEmpty()
							? PluginUiText(
								u"Could not install the plugin."_q,
								u"Не удалось установить плагин."_q)
							: error);
					return;
				}
				if (preview.sourcePath.startsWith(
					cWorkingDir() + QString::fromLatin1(kPluginIncomingFolder))) {
					QFile::remove(preview.sourcePath);
				}
				controller->showToast(
					preview.update
						? PluginUiText(u"Plugin updated."_q, u"Плагин обновлён."_q)
						: PluginUiText(u"Plugin installed."_q, u"Плагин установлен."_q));
				box->closeBox();
			});
		}
	}));
}

void ConfirmDontWarnBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<TextWithEntities> &&text,
		rpl::producer<QString> &&check,
		rpl::producer<QString> &&confirm,
		Fn<void(bool)> callback) {
	auto checkbox = object_ptr<Ui::Checkbox>(
		box.get(),
		std::move(check),
		false,
		st::defaultBoxCheckbox);
	const auto weak = base::make_weak(checkbox.data());
	auto confirmed = crl::guard(weak, [=, callback = std::move(callback)] {
		const auto checked = weak->checked();
		box->closeBox();
		callback(checked);
	});
	Ui::ConfirmBox(box, {
		.text = std::move(text),
		.confirmed = std::move(confirmed),
		.confirmText = std::move(confirm),
	});
	auto padding = st::boxPadding;
	padding.setTop(padding.bottom());
	box->addRow(std::move(checkbox), std::move(padding));
	box->addRow(object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
		box,
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_launch_dont_ask_settings(),
			st::boxLabel)
	))->toggleOn(weak->checkedValue());
}

void LaunchWithWarning(
		// not_null<Window::Controller*> controller,
		const QString &name,
		HistoryItem *item) {
	const auto nameType = Core::DetectNameType(name);
	const auto isIpReveal = (nameType != Core::NameType::Executable)
		&& Core::IsIpRevealingPath(name);
	const auto extension = Core::FileExtension(name).toLower();

	auto &app = Core::App();
	auto &settings = app.settings();
	const auto warn = [&] {
		if (item && item->history()->peer->isVerified()) {
			return false;
		}
		return (isIpReveal && settings.ipRevealWarning())
			|| ((nameType == Core::NameType::Executable
				|| nameType == Core::NameType::Unknown)
				&& !settings.noWarningExtensions().contains(extension));
	}();
	if (extension.isEmpty()) {
		// If you launch a file without extension, like "test", in case
		// there is an executable file with the same name in this folder,
		// like "test.bat", the executable file will be launched.
		//
		// Now we always force an Open With dialog box for such files.
		//
		// Let's force it for all platforms for files without extension.
		crl::on_main([=] {
			Platform::File::UnsafeShowOpenWith(name);
		});
		return;
	} else if (!warn) {
		File::Launch(name);
		return;
	}
	const auto callback = [=, &app, &settings](bool checked) {
		if (checked) {
			if (isIpReveal) {
				settings.setIpRevealWarning(false);
			} else {
				auto copy = settings.noWarningExtensions();
				copy.emplace(extension);
				settings.setNoWarningExtensions(std::move(copy));
			}
			app.saveSettingsDelayed();
		}
		File::Launch(name);
	};
	auto text = isIpReveal
		? tr::lng_launch_svg_warning(tr::marked)
		: ((nameType == Core::NameType::Executable)
			? tr::lng_launch_exe_warning
			: tr::lng_launch_other_warning)(
				lt_extension,
				rpl::single(tr::bold('.' + extension)),
				tr::marked);
	auto check = (isIpReveal
		? tr::lng_launch_exe_dont_ask
		: tr::lng_launch_dont_ask)();
	auto confirm = ((nameType == Core::NameType::Executable)
		? tr::lng_launch_exe_sure
		: tr::lng_launch_other_sure)();
	Ui::show(Box(
		ConfirmDontWarnBox,
		std::move(text),
		std::move(check),
		std::move(confirm),
		callback));
}

} // namespace

const char kOptionExternalVideoPlayer[] = "external-video-player";

base::binary_guard ReadBackgroundImageAsync(
		not_null<Data::DocumentMedia*> media,
		FnMut<QImage(QImage)> postprocess,
		FnMut<void(QImage&&)> done) {
	auto result = base::binary_guard();
	const auto gzipSvg = media->owner()->isPatternWallPaperSVG();
	crl::async([
		gzipSvg,
		bytes = media->bytes(),
		path = media->owner()->filepath(),
		postprocess = std::move(postprocess),
		guard = result.make_guard(),
		callback = std::move(done)
	]() mutable {
		auto image = Ui::ReadBackgroundImage(path, bytes, gzipSvg).image;
		if (image.isNull()) {
			image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::black);
		}
		if (postprocess) {
			image = postprocess(std::move(image));
		}
		crl::on_main(std::move(guard), [
			image = std::move(image),
			callback = std::move(callback)
		]() mutable {
			callback(std::move(image));
		});
	});
	return result;
}

void ResolveDocument(
		Window::SessionController *controller,
		not_null<DocumentData*> document,
		HistoryItem *item,
		MsgId topicRootId,
		PeerId monoforumPeerId) {
	if (document->isNull()) {
		return;
	}
	const auto msgId = item ? item->fullId() : FullMsgId();

	const auto showDocument = [&] {
		if (OptionExternalVideoPlayer.value()
			&& document->isVideoFile()
			&& !document->filepath().isEmpty()) {
			File::Launch(document->location(false).fname);
		} else if (controller) {
			controller->openDocument(
				document,
				true,
				{ msgId, topicRootId, monoforumPeerId });
		}
	};

	const auto media = document->createMediaView();
	const auto openImageInApp = [&] {
		if (document->size >= Images::kReadBytesLimit) {
			return false;
		}
		const auto &location = document->location(true);
		const auto mime = u"image/"_q;
		if (!location.isEmpty() && location.accessEnable()) {
			const auto guard = gsl::finally([&] {
				location.accessDisable();
			});
			const auto path = location.name();
			if (Core::MimeTypeForFile(QFileInfo(path)).name().startsWith(mime)
				&& QImageReader(path).canRead()) {
				showDocument();
				return true;
			}
		} else if (document->mimeString().startsWith(mime)
			&& !media->bytes().isEmpty()) {
			auto bytes = media->bytes();
			auto buffer = QBuffer(&bytes);
			if (QImageReader(&buffer).canRead()) {
				showDocument();
				return true;
			}
		}
		return false;
	};
	const auto &location = document->location(true);
	if (document->isTheme() && media->loaded(true)) {
		showDocument();
		location.accessDisable();
	} else if (media->canBePlayed(item)) {
		if (document->isAudioFile()
			|| document->isVoiceMessage()
			|| document->isVideoMessage()) {
			::Media::Player::instance()->playPause({ document, msgId });
			if (controller
				&& item
				&& item->media()
				&& item->media()->ttlSeconds()) {
				ChatHelpers::ShowTTLMediaLayerWidget(controller, item);
			}
		} else {
			showDocument();
		}
	} else {
		if (IsPluginPackage(document)) {
			if (!controller) {
				return;
			}
			const auto documentId = document->id;
			const auto session = &document->session();
			const auto localPath = LocalPluginPackagePath(document);
			if (!localPath.isEmpty()) {
				ShowPluginPackageBox(
					controller,
					Core::App().plugins().inspectPackage(localPath));
				return;
			}
			const auto tempPath = PluginIncomingPath(document);
			const auto alreadyLoading = document->loading()
				&& document->loadingFilePath() == tempPath;
			if (!alreadyLoading) {
				document->save(
					item ? item->fullId() : Data::FileOrigin(),
					tempPath);
				controller->showToast(
					PluginUiText(
						u"Downloading plugin package..."_q,
						u"Скачиваю пакет плагина..."_q));
				const auto wait = std::make_shared<rpl::lifetime>();
				session->downloaderTaskFinished(
				) | rpl::on_next(crl::guard(controller, [=] {
					const auto current = session->data().document(documentId);
					if (current->loading() && !QFileInfo::exists(tempPath)) {
						return;
					}
					wait->destroy();
					const auto readyPath = QFileInfo::exists(tempPath)
						? tempPath
						: LocalPluginPackagePath(current);
					if (readyPath.isEmpty()) {
						controller->showToast(
							PluginUiText(
								u"Could not prepare the plugin package."_q,
								u"Не удалось подготовить пакет плагина."_q));
						return;
					}
					ShowPluginPackageBox(
						controller,
						Core::App().plugins().inspectPackage(readyPath));
				}), *wait);
			} else {
				controller->showToast(
					PluginUiText(
						u"Plugin package is still downloading."_q,
						u"Пакет плагина ещё скачивается."_q));
			}
			return;
		}
		document->saveFromDataSilent();
		if (!openImageInApp()) {
			if (!document->filepath(true).isEmpty()) {
				LaunchWithWarning(location.name(), item);
			} else if (document->status == FileReady
				|| document->status == FileDownloadFailed) {
				DocumentSaveClickHandler::Save(
					item ? item->fullId() : Data::FileOrigin(),
					document);
			}
		}
	}
}

} // namespace Data
