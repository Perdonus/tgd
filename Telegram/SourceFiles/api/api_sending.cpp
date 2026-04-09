/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_sending.h"

#include "api/api_polls.h"
#include "api/api_text_entities.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/business/data_shortcut_messages.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_channel.h" // ChannelData::addsSignature.
#include "data/data_user.h" // UserData::name
#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_changes.h"
#include "data/data_types.h"
#include "data/stickers/data_stickers.h"
#include "data/data_poll.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h" // NewMessageFlags.
#include "chat_helpers/message_field.h" // ConvertTextTagsToEntities.
#include "chat_helpers/stickers_dice_pack.h" // DicePacks::kDiceString.
#include "ui/text/text_entity.h" // TextWithEntities.
#include "ui/item_text_options.h" // Ui::ItemTextOptions.
#include "main/main_session.h"
#include "main/main_app_config.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "storage/file_upload.h"
#include "styles/style_boxes.h"
#include "mainwidget.h"
#include "apiwrap.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Api {
namespace {

void InnerFillMessagePostFlags(
		const SendOptions &options,
		not_null<PeerData*> peer,
		MessageFlags &flags) {
	if (ShouldSendSilent(peer, options)) {
		flags |= MessageFlag::Silent;
	}
	if (!peer->amAnonymous()
		|| (!peer->isBroadcast()
			&& options.sendAs
			&& options.sendAs != peer)) {
		flags |= MessageFlag::HasFromId;
	}
	const auto channel = peer->asBroadcast();
	if (!channel) {
		return;
	}
	flags |= MessageFlag::Post;
	// Don't display views and author of a new post when it's scheduled.
	if (options.scheduled) {
		return;
	}
	flags |= MessageFlag::HasViews;
	if (channel->addsSignature()) {
		flags |= MessageFlag::HasPostAuthor;
	}
}

[[nodiscard]] bool NeedsForwardlessReupload(not_null<HistoryItem*> item) {
	return !item->history()->peer->allowsForwarding()
		|| item->forbidsForward()
		|| item->forbidsSaving();
}

[[nodiscard]] QString ForwardlessTempDirectory() {
	const auto path = QDir::cleanPath(
		QDir::tempPath() + u"/Astrogram/share-without-author"_q);
	QDir().mkpath(path);
	return path;
}

[[nodiscard]] QString ForwardlessDocumentFileName(
		not_null<DocumentData*> document) {
	auto name = QFileInfo(document->filename()).fileName().trimmed();
	if (!name.isEmpty()) {
		return name;
	} else if (document->isVoiceMessage()) {
		return document->hasMimeType(u"audio/mp3"_q)
			? u"voice.mp3"_q
			: u"voice.ogg"_q;
	} else if (document->isVideoMessage()) {
		return u"video-message.mp4"_q;
	} else if (document->isSong()) {
		return u"audio.mp3"_q;
	} else if (document->isVideoFile()) {
		return u"video.mp4"_q;
	}
	return u"document.bin"_q;
}

[[nodiscard]] QString ForwardlessTempPath(QString name) {
	name = QFileInfo(name).fileName().trimmed();
	if (name.isEmpty()) {
		name = u"file.bin"_q;
	}
	return QDir(ForwardlessTempDirectory()).filePath(
		QString::number(qulonglong(base::RandomValue<uint64>()), 16)
			+ u'-'
			+ name);
}

[[nodiscard]] bool WriteBytesToPath(
		const QString &path,
		const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return false;
	}
	QFile file(path);
	return file.open(QIODevice::WriteOnly)
		&& (file.write(bytes) == bytes.size());
}

[[nodiscard]] QByteArray ReadBytesFromPath(const QString &path) {
	if (path.isEmpty()) {
		return {};
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return file.readAll();
}

[[nodiscard]] TextWithTags ForwardlessCaption(
		not_null<HistoryItem*> item,
		Data::ForwardOptions forwardOptions) {
	if (forwardOptions == Data::ForwardOptions::NoNamesAndCaptions) {
		return {};
	}
	const auto original = item->originalText();
	return {
		original.text,
		TextUtilities::ConvertEntitiesToTextTags(original.entities),
	};
}

void SendForwardlessPreparedUpload(
		SendAction action,
		const QString &path,
		TextWithTags caption,
		SendMediaType type,
		bool spoiler) {
	const auto premium = action.history->session().user()->isPremium();
	auto list = Storage::PrepareMediaList(
		QStringList{ path },
		st::sendMediaPreviewSize,
		premium);
	if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
		return;
	}
	list.files.front().spoiler = spoiler;
	action.history->session().api().sendFiles(
		std::move(list),
		type,
		std::move(caption),
		nullptr,
		action);
	action.history->session().api().finishForwarding(action);
}

void SendForwardlessVoiceUpload(
		SendAction action,
		QByteArray bytes,
		VoiceWaveform waveform,
		crl::time duration,
		bool video) {
	if (bytes.isEmpty()) {
		return;
	}
	action.history->session().api().sendVoiceMessage(
		std::move(bytes),
		waveform,
		duration,
		video,
		action);
	action.history->session().api().finishForwarding(action);
}

void SendForwardlessDocument(
		SendAction action,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		TextWithTags caption,
		bool spoiler) {
	const auto origin = Data::FileOrigin(item->fullId());
	const auto isRound = (document->round() != nullptr);
	const auto isVoice = (document->voice() != nullptr) || isRound;
	const auto sendVoice = [=](QByteArray bytes) {
		auto waveform = VoiceWaveform();
		if (const auto voice = document->voice()) {
			waveform = voice->waveform;
		} else if (const auto round = document->round()) {
			waveform = round->waveform;
		}
		SendForwardlessVoiceUpload(
			action,
			std::move(bytes),
			std::move(waveform),
			document->duration(),
			isRound);
	};
	const auto existingPath = document->filepath(true);
	if (!existingPath.isEmpty()) {
		if (isVoice) {
			if (const auto bytes = ReadBytesFromPath(existingPath)
				; !bytes.isEmpty()) {
				sendVoice(bytes);
				return;
			}
		} else {
			SendForwardlessPreparedUpload(
				action,
				existingPath,
				std::move(caption),
				SendMediaType::File,
				spoiler);
			return;
		}
	}
	const auto media = document->activeMediaView();
	if (media && media->loaded(true) && !media->bytes().isEmpty()) {
		if (isVoice) {
			sendVoice(media->bytes());
			return;
		}
		const auto tempPath = ForwardlessTempPath(
			ForwardlessDocumentFileName(document));
		if (WriteBytesToPath(tempPath, media->bytes())) {
			SendForwardlessPreparedUpload(
				action,
				tempPath,
				std::move(caption),
				SendMediaType::File,
				spoiler);
			return;
		}
	}

	const auto tempPath = ForwardlessTempPath(
		ForwardlessDocumentFileName(document));
	document->save(origin, tempPath, LoadFromCloudOrLocal, true);
	auto sendReady = [=]() mutable -> bool {
		if (isVoice) {
			if (const auto current = document->activeMediaView(); current
				&& current->loaded(true)
				&& !current->bytes().isEmpty()) {
				sendVoice(current->bytes());
				return true;
			}
			const auto readyPath = QFileInfo::exists(tempPath)
				? tempPath
				: document->filepath(true);
			if (const auto bytes = ReadBytesFromPath(readyPath)
				; !bytes.isEmpty()) {
				sendVoice(bytes);
				return true;
			}
			return false;
		}
		const auto readyPath = QFileInfo::exists(tempPath)
			? tempPath
			: document->filepath(true);
		if (readyPath.isEmpty()) {
			return false;
		}
		SendForwardlessPreparedUpload(
			action,
			readyPath,
			std::move(caption),
			SendMediaType::File,
			spoiler);
		return true;
	};
	if (sendReady()) {
		return;
	}

	const auto wait = std::make_shared<rpl::lifetime>();
	document->session().downloaderTaskFinished() | rpl::on_next([=]() mutable {
		if (document->loading() && !QFileInfo::exists(tempPath)) {
			return;
		}
		wait->destroy();
		sendReady();
	}, *wait);
}

void SendForwardlessPhoto(
		SendAction action,
		not_null<HistoryItem*> item,
		not_null<PhotoData*> photo,
		TextWithTags caption,
		bool spoiler) {
	const auto origin = Data::FileOrigin(item->fullId());
	const auto tempPath = ForwardlessTempPath(u"photo.jpg"_q);
	auto sendReady = [=]() mutable -> bool {
		const auto media = photo->activeMediaView();
		if (!media || !media->loaded() || !media->saveToFile(tempPath)) {
			return false;
		}
		SendForwardlessPreparedUpload(
			action,
			tempPath,
			std::move(caption),
			SendMediaType::Photo,
			spoiler);
		return true;
	};
	if (sendReady()) {
		return;
	}
	photo->load(origin, LoadFromCloudOrLocal, true);
	if (sendReady()) {
		return;
	}

	const auto wait = std::make_shared<rpl::lifetime>();
	photo->session().downloaderTaskFinished() | rpl::on_next([=]() mutable {
		if (photo->loading()) {
			return;
		}
		wait->destroy();
		sendReady();
	}, *wait);
}

void SendSimpleMedia(SendAction action, MTPInputMedia inputMedia) {
	const auto history = action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();

	action.clearDraft = false;
	action.generateLocal = false;
	api->sendAction(action);

	const auto randomId = base::RandomValue<uint64>();

	auto flags = NewMessageFlags(peer);
	auto sendFlags = MTPmessages_SendMedia::Flags(0);
	if (action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
		sendFlags |= MTPmessages_SendMedia::Flag::f_reply_to;
	}
	const auto silentPost = ShouldSendSilent(peer, action.options);
	InnerFillMessagePostFlags(action.options, peer, flags);
	if (silentPost) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_silent;
	}
	const auto sendAs = action.options.sendAs;
	if (sendAs) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_send_as;
	}
	const auto messagePostAuthor = peer->isBroadcast()
		? session->user()->name()
		: QString();
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		action.options.starsApproved);
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
		if (action.options.scheduleRepeatPeriod) {
			sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_repeat_period;
		}
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
		sendFlags |= MTPmessages_SendMedia::Flag::f_quick_reply_shortcut;
	}
	if (action.options.effectId) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_effect;
	}
	if (action.options.suggest) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_suggested_post;
	}
	if (action.options.invertCaption) {
		flags |= MessageFlag::InvertMedia;
		sendFlags |= MTPmessages_SendMedia::Flag::f_invert_media;
	}
	if (starsPaid) {
		action.options.starsApproved -= starsPaid;
		sendFlags |= MTPmessages_SendMedia::Flag::f_allow_paid_stars;
	}

	auto &histories = history->owner().histories();
	histories.sendPreparedMessage(
		history,
		action.replyTo,
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
			MTP_flags(sendFlags),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			std::move(inputMedia),
			MTPstring(),
			MTP_long(randomId),
			MTPReplyMarkup(),
			MTPvector<MTPMessageEntity>(),
			MTP_int(action.options.scheduled),
			MTP_int(action.options.scheduleRepeatPeriod),
			(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(session, action.options.shortcutId),
			MTP_long(action.options.effectId),
			MTP_long(starsPaid),
			SuggestToMTP(action.options.suggest)
		), [=](const MTPUpdates &result, const MTP::Response &response) {
	}, [=](const MTP::Error &error, const MTP::Response &response) {
		api->sendMessageFail(error, peer, randomId);
	});

	api->finishForwarding(action);
}

template <typename MediaData>
void SendExistingMedia(
		MessageToSend &&message,
		not_null<MediaData*> media,
		Fn<MTPInputMedia()> inputMedia,
		Data::FileOrigin origin,
		std::optional<MsgId> localMessageId) {
	const auto history = message.action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();

	message.action.clearDraft = false;
	message.action.generateLocal = true;
	api->sendAction(message.action);

	const auto newId = FullMsgId(
		peer->id,
		localMessageId
			? (*localMessageId)
			: session->data().nextLocalMessageId());
	const auto randomId = base::RandomValue<uint64>();
	auto &action = message.action;

	auto flags = NewMessageFlags(peer);
	auto sendFlags = MTPmessages_SendMedia::Flags(0);
	if (action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
		sendFlags |= MTPmessages_SendMedia::Flag::f_reply_to;
	}
	const auto silentPost = ShouldSendSilent(peer, action.options);
	InnerFillMessagePostFlags(action.options, peer, flags);
	if (silentPost) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_silent;
	}
	const auto sendAs = action.options.sendAs;
	if (sendAs) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_send_as;
	}
	auto caption = TextWithEntities{
		message.textWithTags.text,
		TextUtilities::ConvertTextTagsToEntities(message.textWithTags.tags)
	};
	TextUtilities::Trim(caption);
	auto sentEntities = EntitiesToMTP(
		session,
		caption.entities,
		ConvertOption::SkipLocal);
	if (!sentEntities.v.isEmpty()) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_entities;
	}
	const auto captionText = caption.text;
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		action.options.starsApproved);
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
		if (action.options.scheduleRepeatPeriod) {
			sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_repeat_period;
		}
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
		sendFlags |= MTPmessages_SendMedia::Flag::f_quick_reply_shortcut;
	}
	if (action.options.effectId) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_effect;
	}
	if (action.options.suggest) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_suggested_post;
	}
	if (action.options.invertCaption) {
		flags |= MessageFlag::InvertMedia;
		sendFlags |= MTPmessages_SendMedia::Flag::f_invert_media;
	}
	if (starsPaid) {
		action.options.starsApproved -= starsPaid;
		sendFlags |= MTPmessages_SendMedia::Flag::f_allow_paid_stars;
	}

	session->data().registerMessageRandomId(randomId, newId);

	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = flags,
		.from = NewMessageFromId(action),
		.replyTo = action.replyTo,
		.date = NewMessageDate(action.options),
		.shortcutId = action.options.shortcutId,
		.starsPaid = starsPaid,
		.postAuthor = NewMessagePostAuthor(action),
		.effectId = action.options.effectId,
		.suggest = HistoryMessageSuggestInfo(action.options),
		.mediaSpoiler = action.options.mediaSpoiler,
	}, media, caption);

	const auto performRequest = [=](const auto &repeatRequest) -> void {
		auto &histories = history->owner().histories();
		const auto session = &history->session();
		const auto usedFileReference = media->fileReference();
		histories.sendPreparedMessage(
			history,
			action.replyTo,
			randomId,
			Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
				MTP_flags(sendFlags),
				peer->input(),
				Data::Histories::ReplyToPlaceholder(),
				inputMedia(),
				MTP_string(captionText),
				MTP_long(randomId),
				MTPReplyMarkup(),
				sentEntities,
				MTP_int(action.options.scheduled),
				MTP_int(action.options.scheduleRepeatPeriod),
				(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
				Data::ShortcutIdToMTP(session, action.options.shortcutId),
				MTP_long(action.options.effectId),
				MTP_long(starsPaid),
				SuggestToMTP(action.options.suggest)
			), [=](const MTPUpdates &result, const MTP::Response &response) {
		}, [=](const MTP::Error &error, const MTP::Response &response) {
			if (error.code() == 400
				&& error.type().startsWith(u"FILE_REFERENCE_"_q)) {
				api->refreshFileReference(origin, [=](const auto &result) {
					if (media->fileReference() != usedFileReference) {
						repeatRequest(repeatRequest);
					} else {
						api->sendMessageFail(error, peer, randomId, newId);
					}
				});
			} else {
				api->sendMessageFail(error, peer, randomId, newId);
			}
		});
	};
	performRequest(performRequest);

	api->finishForwarding(action);
}

} // namespace

void SendExistingDocument(
		MessageToSend &&message,
		not_null<DocumentData*> document,
		std::optional<MsgId> localMessageId) {
	const auto inputMedia = [=] {
		return MTP_inputMediaDocument(
			MTP_flags(message.action.options.mediaSpoiler
				? MTPDinputMediaDocument::Flag::f_spoiler
				: MTPDinputMediaDocument::Flags(0)),
			document->mtpInput(),
			MTPInputPhoto(), // video_cover
			MTPint(), // ttl_seconds
			MTPint(), // video_timestamp
			MTPstring()); // query
	};
	SendExistingMedia(
		std::move(message),
		document,
		inputMedia,
		document->stickerOrGifOrigin(),
		std::move(localMessageId));

	if (document->sticker()) {
		document->owner().stickers().incrementSticker(document);
	}
}

void SendExistingPhoto(
		MessageToSend &&message,
		not_null<PhotoData*> photo,
		std::optional<MsgId> localMessageId) {
	const auto inputMedia = [=] {
		return MTP_inputMediaPhoto(
			MTP_flags(0),
			photo->mtpInput(),
			MTPint());
	};
	SendExistingMedia(
		std::move(message),
		photo,
		inputMedia,
		Data::FileOrigin(),
		std::move(localMessageId));
}

[[nodiscard]] bool SendWithoutAuthor(
		SendAction action,
		not_null<HistoryItem*> item,
		Data::ForwardOptions forwardOptions) {
	auto message = MessageToSend(action);
	const auto media = item->media();
	if (!media) {
		const auto originalText = item->originalText();
		if (originalText.text.isEmpty()) {
			return false;
		}
		message.textWithTags = TextWithTags{
			originalText.text,
			TextUtilities::ConvertEntitiesToTextTags(originalText.entities),
		};
		action.history->session().api().sendMessage(std::move(message));
		return true;
	}
	if (const auto contact = media->sharedContact()) {
		action.history->session().api().shareContact(
			contact->phoneNumber,
			contact->firstName,
			contact->lastName,
			action);
		return true;
	}
	if (const auto point = media->locationPoint()) {
		if (const auto venue = media->venue()) {
			SendVenue(action, *venue);
		} else {
			SendLocation(action, point->lat(), point->lon());
		}
		return true;
	}
	if (const auto poll = media->poll()) {
		action.history->session().api().polls().create(
			*poll,
			action,
			[] {},
			[] {});
		action.history->session().api().finishForwarding(action);
		return true;
	}
	const auto reupload = NeedsForwardlessReupload(item)
		|| (media->ttlSeconds() > 0);
	message.textWithTags = ForwardlessCaption(item, forwardOptions);
	message.action.options.mediaSpoiler = media->hasSpoiler();
	message.action.options.invertCaption = item->invertMedia();
	message.action.options.ttlSeconds = media->ttlSeconds();
	if (const auto photo = media->photo()) {
		if (reupload) {
			SendForwardlessPhoto(
				message.action,
				item,
				photo,
				std::move(message.textWithTags),
				media->hasSpoiler());
			return true;
		}
		SendExistingPhoto(std::move(message), photo);
		return true;
	} else if (const auto document = media->document()) {
		if (reupload) {
			SendForwardlessDocument(
				message.action,
				item,
				document,
				std::move(message.textWithTags),
				media->hasSpoiler());
			return true;
		}
		SendExistingDocument(std::move(message), document);
		return true;
	}
	return false;
}

bool SendDice(MessageToSend &message) {
	const auto full = QStringView(message.textWithTags.text).trimmed();
	auto length = 0;
	if (!Ui::Emoji::Find(full.data(), full.data() + full.size(), &length)
		|| length != full.size()
		|| !message.textWithTags.tags.isEmpty()) {
		return false;
	}
	auto &config = message.action.history->session().appConfig();
	static const auto hardcoded = std::vector<QString>{
		Stickers::DicePacks::kDiceString,
		Stickers::DicePacks::kDartString,
		Stickers::DicePacks::kSlotString,
		Stickers::DicePacks::kFballString,
		Stickers::DicePacks::kFballString + QChar(0xFE0F),
		Stickers::DicePacks::kBballString,
	};
	const auto list = config.get<std::vector<QString>>(
		"emojies_send_dice",
		hardcoded);
	const auto emoji = full.toString();
	if (!ranges::contains(list, emoji)) {
		return false;
	}
	const auto history = message.action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();

	message.textWithTags = TextWithTags();
	message.action.clearDraft = false;
	message.action.generateLocal = true;

	auto &action = message.action;
	api->sendAction(action);

	const auto newId = FullMsgId(
		peer->id,
		session->data().nextLocalMessageId());
	const auto randomId = base::RandomValue<uint64>();

	auto &histories = history->owner().histories();
	auto flags = NewMessageFlags(peer);
	auto sendFlags = MTPmessages_SendMedia::Flags(0);
	if (action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
		sendFlags |= MTPmessages_SendMedia::Flag::f_reply_to;
	}
	const auto silentPost = ShouldSendSilent(peer, action.options);
	InnerFillMessagePostFlags(action.options, peer, flags);
	if (silentPost) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_silent;
	}
	const auto sendAs = action.options.sendAs;
	if (sendAs) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_send_as;
	}
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
		if (action.options.scheduleRepeatPeriod) {
			sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_repeat_period;
		}
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
		sendFlags |= MTPmessages_SendMedia::Flag::f_quick_reply_shortcut;
	}
	if (action.options.effectId) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_effect;
	}
	if (action.options.suggest) {
		sendFlags |= MTPmessages_SendMedia::Flag::f_suggested_post;
	}
	if (action.options.invertCaption) {
		flags |= MessageFlag::InvertMedia;
		sendFlags |= MTPmessages_SendMedia::Flag::f_invert_media;
	}
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		action.options.starsApproved);
	if (starsPaid) {
		action.options.starsApproved -= starsPaid;
		sendFlags |= MTPmessages_SendMedia::Flag::f_allow_paid_stars;
	}

	session->data().registerMessageRandomId(randomId, newId);

	auto seed = QByteArray(32, Qt::Uninitialized);
	base::RandomFill(bytes::make_detached_span(seed));
	const auto stake = action.options.stakeSeedHash.isEmpty()
		? 0
		: action.options.stakeNanoTon;
	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = flags,
		.from = NewMessageFromId(action),
		.replyTo = action.replyTo,
		.date = NewMessageDate(action.options),
		.shortcutId = action.options.shortcutId,
		.starsPaid = starsPaid,
		.postAuthor = NewMessagePostAuthor(action),
		.effectId = action.options.effectId,
		.suggest = HistoryMessageSuggestInfo(action.options),
	}, TextWithEntities(), MTP_messageMediaDice(
		MTP_flags(stake
			? MTPDmessageMediaDice::Flag::f_game_outcome
			: MTPDmessageMediaDice::Flag()),
		MTP_int(0),
		MTP_string(emoji),
		MTP_messages_emojiGameOutcome(
			MTP_bytes(seed),
			MTP_long(stake),
			MTP_long(0))));
	histories.sendPreparedMessage(
		history,
		action.replyTo,
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
			MTP_flags(sendFlags),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			(stake
				? MTP_inputMediaStakeDice(
					MTP_bytes(action.options.stakeSeedHash),
					MTP_long(stake),
					MTP_bytes(seed))
				: MTP_inputMediaDice(MTP_string(emoji))),
			MTP_string(),
			MTP_long(randomId),
			MTPReplyMarkup(),
			MTP_vector<MTPMessageEntity>(),
			MTP_int(action.options.scheduled),
			MTP_int(action.options.scheduleRepeatPeriod),
			(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(session, action.options.shortcutId),
			MTP_long(action.options.effectId),
			MTP_long(starsPaid),
			SuggestToMTP(action.options.suggest)
		), [=](const MTPUpdates &result, const MTP::Response &response) {
	}, [=](const MTP::Error &error, const MTP::Response &response) {
		api->sendMessageFail(error, peer, randomId, newId);
	});
	api->finishForwarding(action);
	return true;
}

void SendLocation(SendAction action, float64 lat, float64 lon) {
	SendSimpleMedia(
		action,
		MTP_inputMediaGeoPoint(
			MTP_inputGeoPoint(
				MTP_flags(0),
				MTP_double(lat),
				MTP_double(lon),
				MTPint()))); // accuracy_radius
}

void SendVenue(SendAction action, Data::InputVenue venue) {
	SendSimpleMedia(
		action,
		MTP_inputMediaVenue(
			MTP_inputGeoPoint(
				MTP_flags(0),
				MTP_double(venue.lat),
				MTP_double(venue.lon),
				MTPint()), // accuracy_radius
			MTP_string(venue.title),
			MTP_string(venue.address),
			MTP_string(venue.provider),
			MTP_string(venue.id),
			MTP_string(venue.venueType)));
}

void FillMessagePostFlags(
		const SendAction &action,
		not_null<PeerData*> peer,
		MessageFlags &flags) {
	InnerFillMessagePostFlags(action.options, peer, flags);
}

void SendConfirmedFile(
		not_null<Main::Session*> session,
		const std::shared_ptr<FilePrepareResult> &file) {
	const auto isEditing = (file->type != SendMediaType::Audio)
		&& (file->type != SendMediaType::Round)
		&& (file->to.replaceMediaOf != 0);
	const auto newId = FullMsgId(
		file->to.peer,
		(isEditing
			? file->to.replaceMediaOf
			: session->data().nextLocalMessageId()));
	const auto groupId = file->album ? file->album->groupId : uint64(0);
	if (file->album) {
		const auto proj = [](const SendingAlbum::Item &item) {
			return item.taskId;
		};
		const auto it = ranges::find(file->album->items, file->taskId, proj);
		Assert(it != file->album->items.end());

		it->msgId = newId;
	}

	const auto itemToEdit = isEditing
		? session->data().message(newId)
		: nullptr;
	const auto history = session->data().history(file->to.peer);
	const auto peer = history->peer;

	if (!isEditing) {
		const auto histories = &session->data().histories();
		file->to.replyTo.messageId = histories->convertTopicReplyToId(
			history,
			file->to.replyTo.messageId);
		file->to.replyTo.topicRootId = histories->convertTopicReplyToId(
			history,
			file->to.replyTo.topicRootId);
	}

	session->uploader().upload(newId, file);

	auto action = SendAction(history, file->to.options);
	action.clearDraft = false;
	action.replyTo = file->to.replyTo;
	action.generateLocal = true;
	action.replaceMediaOf = file->to.replaceMediaOf;
	session->api().sendAction(action);

	auto caption = TextWithEntities{
		file->caption.text,
		TextUtilities::ConvertTextTagsToEntities(file->caption.tags)
	};
	const auto prepareFlags = Ui::ItemTextOptions(
		history,
		session->user()).flags;
	TextUtilities::PrepareForSending(caption, prepareFlags);
	TextUtilities::Trim(caption);

	auto flags = isEditing ? MessageFlags() : NewMessageFlags(peer);
	if (file->to.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
	}
	FillMessagePostFlags(action, peer, flags);
	if (file->to.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;

		// Scheduled messages have no 'edited' badge.
		flags |= MessageFlag::HideEdited;
	}
	if (file->to.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;

		// Shortcut messages have no 'edited' badge.
		flags |= MessageFlag::HideEdited;
	}
	if (file->type == SendMediaType::Audio
		|| file->type == SendMediaType::Round) {
		if (!peer->isChannel() || peer->isMegagroup()) {
			flags |= MessageFlag::MediaIsUnread;
		}
	}
	if (file->to.options.invertCaption) {
		flags |= MessageFlag::InvertMedia;
	}
	const auto media = MTPMessageMedia([&] {
		if (file->type == SendMediaType::Photo) {
			using Flag = MTPDmessageMediaPhoto::Flag;
			return MTP_messageMediaPhoto(
				MTP_flags(Flag::f_photo
					| (file->spoiler ? Flag::f_spoiler : Flag())),
				file->photo,
				MTPint());
		} else if (file->type == SendMediaType::File) {
			using Flag = MTPDmessageMediaDocument::Flag;
			return MTP_messageMediaDocument(
				MTP_flags(Flag::f_document
					| (file->spoiler ? Flag::f_spoiler : Flag())
					| (file->videoCover ? Flag::f_video_cover : Flag())),
				file->document,
				MTPVector<MTPDocument>(), // alt_documents
				file->videoCover ? file->videoCover->photo : MTPPhoto(),
				MTPint(), // video_timestamp
				MTPint());
		} else if (file->type == SendMediaType::Audio) {
			const auto ttlSeconds = file->to.options.ttlSeconds;
			using Flag = MTPDmessageMediaDocument::Flag;
			return MTP_messageMediaDocument(
				MTP_flags(Flag::f_document
					| Flag::f_voice
					| (ttlSeconds ? Flag::f_ttl_seconds : Flag())
					| (file->videoCover ? Flag::f_video_cover : Flag())),
				file->document,
				MTPVector<MTPDocument>(), // alt_documents
				file->videoCover ? file->videoCover->photo : MTPPhoto(),
				MTPint(), // video_timestamp
				MTP_int(ttlSeconds));
		} else if (file->type == SendMediaType::Round) {
			using Flag = MTPDmessageMediaDocument::Flag;
			const auto ttlSeconds = file->to.options.ttlSeconds;
			return MTP_messageMediaDocument(
				MTP_flags(Flag::f_document
					| Flag::f_round
					| (ttlSeconds ? Flag::f_ttl_seconds : Flag())
					| (file->spoiler ? Flag::f_spoiler : Flag())),
				file->document,
				MTPVector<MTPDocument>(), // alt_documents
				MTPPhoto(), // video_cover
				MTPint(), // video_timestamp
				MTP_int(ttlSeconds));
		} else {
			Unexpected("Type in sendFilesConfirmed.");
		}
	}());

	if (itemToEdit) {
		auto edition = HistoryMessageEdition();
		edition.isEditHide = (flags & MessageFlag::HideEdited);
		edition.editDate = 0;
		edition.ttl = 0;
		edition.mtpMedia = &media;
		edition.textWithEntities = caption;
		edition.invertMedia = file->to.options.invertCaption;
		edition.useSameViews = true;
		edition.useSameForwards = true;
		edition.useSameMarkup = true;
		edition.useSameReplies = true;
		edition.useSameReactions = true;
		edition.useSameSuggest = true;
		edition.savePreviousMedia = true;
		itemToEdit->applyEdition(std::move(edition));
	} else {
		history->addNewLocalMessage({
			.id = newId.msg,
			.flags = flags,
			.from = NewMessageFromId(action),
			.replyTo = file->to.replyTo,
			.date = NewMessageDate(file->to.options),
			.shortcutId = file->to.options.shortcutId,
			.starsPaid = std::min(
				history->peer->starsPerMessageChecked(),
				file->to.options.starsApproved),
			.postAuthor = NewMessagePostAuthor(action),
			.groupedId = groupId,
			.effectId = file->to.options.effectId,
			.suggest = HistoryMessageSuggestInfo(file->to.options),
		}, caption, media);
	}

	if (isEditing) {
		return;
	}

	session->data().sendHistoryChangeNotifications();
	if (!itemToEdit) {
		session->changes().historyUpdated(
			history,
			(action.options.scheduled
				? Data::HistoryUpdate::Flag::ScheduledSent
				: Data::HistoryUpdate::Flag::MessageSent));
	}
}

} // namespace Api
