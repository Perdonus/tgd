/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_context_menu.h"

#include "api/api_attached_stickers.h"
#include "api/api_common.h"
#include "api/api_editing.h"
#include "api/api_global_privacy.h"
#include "api/api_polls.h"
#include "api/api_report.h"
#include "api/api_ringtones.h"
#include "api/api_sending.h"
#include "api/api_transcribes.h"
#include "api/api_who_reacted.h"
#include "api/api_toggling_media.h" // Api::ToggleFavedSticker
#include "ayu/data/messages_storage.h"
#include "base/qt/qt_key_modifiers.h"
#include "base/unixtime.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_cursor_state.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_text.h"
#include "history/view/history_view_schedule_box.h"
#include "history/view/media/history_view_media.h"
#include "history/view/media/history_view_save_document_action.h"
#include "history/view/media/history_view_web_page.h"
#include "history/view/reactions/history_view_reactions_list.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_widget.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/menu/menu_multiline_action.h"
#include "ui/image/image.h"
#include "ui/toast/toast.h"
#include "ui/text/format_song_document_name.h"
#include "ui/text/text_utilities.h"
#include "ui/controls/delete_message_context_action.h"
#include "ui/controls/who_reacted_context_action.h"
#include "ui/boxes/edit_factcheck_box.h"
#include "ui/boxes/report_box_graphics.h"
#include "ui/ui_utility.h"
#include "menu/menu_item_download_files.h"
#include "menu/menu_item_rate_transcribe.h"
#include "menu/menu_item_rate_transcribe_session.h"
#include "menu/menu_send.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/show_or_premium_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/power_saving.h"
#include "boxes/delete_messages_box.h"
#include "boxes/moderate_messages_box.h"
#include "boxes/report_messages_box.h"
#include "boxes/sticker_set_box.h"
#include "boxes/stickers_box.h"
#include "boxes/translate_box.h"
#include "base/flat_set.h"
#include "data/components/factchecks.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_groups.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_file_click_handler.h"
#include "data/data_file_origin.h"
#include "data/data_message_reactions.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "chat_helpers/message_field.h" // FactcheckFieldIniter.
#include "core/file_utilities.h"
#include "core/click_handler_types.h"
#include "base/platform/base_platform_info.h"
#include "base/call_delayed.h"
#include "settings/settings_premium.h"
#include "window/window_peer_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include "core/application.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "spellcheck/spellcheck_types.h"
#include "apiwrap.h"
#include "ui/widgets/labels.h"
#include "ui/layers/generic_box.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>

#include <memory>

namespace HistoryView {
namespace {

constexpr auto kRescheduleLimit = 20;
constexpr auto kTagNameLimit = 12;
constexpr auto kPublicPostLinkToastDuration = 4 * crl::time(1000);

constexpr auto kContextLayoutVersion = 1;
constexpr auto kContextMenuActionSelectionCopy = "selection.copy";
constexpr auto kContextMenuActionSelectionTranslate = "selection.translate";
constexpr auto kContextMenuActionSelectionSearch = "selection.search";
constexpr auto kContextMenuActionSelectionForward = "selection.forward";
constexpr auto kContextMenuActionSelectionForwardWithoutAuthor = "selection.forward_without_author";
constexpr auto kContextMenuActionSelectionForwardSaved = "selection.forward_saved";
constexpr auto kContextMenuActionSelectionSendNow = "selection.send_now";
constexpr auto kContextMenuActionSelectionDelete = "selection.delete";
constexpr auto kContextMenuActionSelectionDownloadFiles = "selection.download_files";
constexpr auto kContextMenuActionSelectionClear = "selection.clear";
constexpr auto kContextMenuActionMessageGoTo = "message.go_to";
constexpr auto kContextMenuActionMessageViewReplies = "message.view_replies";
constexpr auto kContextMenuActionMessageReply = "message.reply";
constexpr auto kContextMenuActionMessageTodoEdit = "message.todo.edit";
constexpr auto kContextMenuActionMessageTodoAdd = "message.todo.add";
constexpr auto kContextMenuActionMessageEdit = "message.edit";
constexpr auto kContextMenuActionMessageEditHistory = "message.edit_history";
constexpr auto kContextMenuActionMessageCopyIdsTime = "message.copy_ids_time";
constexpr auto kContextMenuActionMessageFactcheck = "message.factcheck";
constexpr auto kContextMenuActionMessagePin = "message.pin";
constexpr auto kContextMenuActionMessageCopyPostLink = "message.copy_post_link";
constexpr auto kContextMenuActionMessageCopyText = "message.copy_text";
constexpr auto kContextMenuActionMessageTranslate = "message.translate";
constexpr auto kContextMenuActionLinkCopy = "link.copy";
constexpr auto kContextMenuActionMessageForward = "message.forward";
constexpr auto kContextMenuActionMessageForwardWithoutAuthor = "message.forward_without_author";
constexpr auto kContextMenuActionMessageForwardSaved = "message.forward_saved";
constexpr auto kContextMenuActionMessageSendNow = "message.send_now";
constexpr auto kContextMenuActionMessageDelete = "message.delete";
constexpr auto kContextMenuActionMessageReport = "message.report";
constexpr auto kContextMenuActionMessageSelect = "message.select";
constexpr auto kContextMenuActionMessageReschedule = "message.reschedule";

[[nodiscard]] ContextMenuLayoutEntry MakeLayoutEntry(
		const char *id,
		bool visible = true) {
	return {
		.id = QString::fromLatin1(id),
		.visible = visible,
	};
}

class ContextMenuBuilder final {
public:
	ContextMenuBuilder(
		not_null<Ui::PopupMenu*> menu,
		ContextMenuResolvedLayout *resolved,
		ContextMenuSurface surface)
	: _menu(menu)
	, _resolved(resolved) {
		if (_resolved) {
			_resolved->surface = surface;
			_resolved->actions.clear();
		}
	}

	[[nodiscard]] not_null<Ui::PopupMenu*> raw() const {
		return _menu;
	}

	void addAction(
		const char *id,
		const QString &text,
		Fn<void()> callback,
		const style::icon *icon = nullptr,
		bool stripEligible = false) {
		auto shared = std::make_shared<Fn<void()>>(std::move(callback));
		_menu->addAction(text, [shared] {
			(*shared)();
		}, icon);
		pushResolved(id, text, icon, std::move(shared), stripEligible);
	}

	void addCustom(
		const char *id,
		Fn<void(not_null<Ui::PopupMenu*>)> append,
		const style::icon *icon = nullptr,
		std::shared_ptr<Fn<void()>> trigger = nullptr,
		bool stripEligible = false,
		const QString &text = QString()) {
		append(_menu);
		pushResolved(id, text, icon, std::move(trigger), stripEligible);
	}

private:
	void pushResolved(
		const char *id,
		QString text,
		const style::icon *icon,
		std::shared_ptr<Fn<void()>> trigger,
		bool stripEligible) {
		if (!_resolved || !id || !*id) {
			return;
		}
		_resolved->actions.push_back(ContextMenuResolvedAction{
			.id = QString::fromLatin1(id),
			.text = std::move(text),
			.icon = icon,
			.trigger = std::move(trigger),
			.stripEligible = stripEligible,
		});
	}

	const not_null<Ui::PopupMenu*> _menu;
	ContextMenuResolvedLayout *_resolved = nullptr;
};

[[nodiscard]] bool AstrogramRussianUi() {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString AstrogramUiText(const char *en, const char *ru) {
	return AstrogramRussianUi()
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString ForwardWithoutAuthorText() {
	return AstrogramUiText(
		"Forward without author",
		"Переслать без автора");
}

[[nodiscard]] Data::ForwardDraft ForwardWithoutAuthorDraft(
		MessageIdsList ids) {
	return Data::ForwardDraft{
		.ids = std::move(ids),
		.options = Data::ForwardOptions::NoSenderNames,
	};
}

void ShowForwardWithoutAuthorBox(
		not_null<Window::SessionNavigation*> navigation,
		MessageIdsList ids,
		Fn<void()> &&successCallback = nullptr) {
	Window::ShowForwardMessagesBox(
		navigation,
		ForwardWithoutAuthorDraft(std::move(ids)),
		std::move(successCallback));
}

[[nodiscard]] QString FormatRevisionTime(int timestamp) {
	const auto dt = QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime();
	return AstrogramRussianUi()
		? dt.toString(u"dd.MM.yyyy HH:mm:ss"_q)
		: QLocale().toString(dt, QLocale::LongFormat);
}

[[nodiscard]] QString BuildRevisionsClipboardText(
		const std::vector<AyuMessages::MessageSnapshot> &revisions) {
	auto lines = QStringList();
	lines.reserve(int(revisions.size()) * 3);
	auto index = 1;
	for (const auto &revision : revisions) {
		lines.push_back(AstrogramUiText("Revision %1", "Правка %1").arg(index++));
		lines.push_back(FormatRevisionTime(revision.editDate ? revision.editDate : revision.date));
		lines.push_back(revision.text);
		lines.push_back(QString());
	}
	return lines.join(u"\n"_q).trimmed();
}

void ShowEditHistoryBox(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto revisions = AyuMessages::getEditedMessages(item, 50);
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			AstrogramUiText("Edit History", "История правок")));
		box->addLeftButton(
			rpl::single(AstrogramUiText("Copy", "Копировать")),
			[=] {
				if (const auto clipboard = QGuiApplication::clipboard()) {
					clipboard->setText(BuildRevisionsClipboardText(revisions));
				}
				controller->window().showToast(AstrogramUiText(
					"Edit history copied.",
					"История правок скопирована."));
			});
		if (revisions.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(AstrogramUiText(
					"No saved revisions.",
					"Сохранённых правок нет.")),
				st::boxLabel),
				style::margins(
					st::boxPadding.left(),
					0,
					st::boxPadding.right(),
					0),
				style::al_top);
			return;
		}
		for (auto i = 0, count = int(revisions.size()); i != count; ++i) {
			const auto &revision = revisions[i];
			const auto title = AstrogramUiText(
				"Revision %1 • %2",
				"Правка %1 • %2"
			).arg(i + 1).arg(FormatRevisionTime(
				revision.editDate ? revision.editDate : revision.date));
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(title),
				st::sessionDateLabel),
				style::margins(
					st::boxPadding.left(),
					i ? (st::boxPadding.bottom() / 2) : 0,
					st::boxPadding.right(),
					0),
				style::al_top);
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(revision.text),
				st::boxLabel),
				style::margins(
					st::boxPadding.left(),
					0,
					st::boxPadding.right(),
					0),
				style::al_top);
		}
	}));
}

bool HasEditMessageAction(
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	const auto context = list->elementContext();
	if (!item
		|| item->isSending()
		|| item->hasFailed()
		|| item->isEditingMedia()
		|| !request.selectedItems.empty()
		|| (context != Context::History
			&& context != Context::Replies
			&& context != Context::ShortcutMessages
			&& context != Context::ScheduledTopic
			&& context != Context::Monoforum)) {
		return false;
	}
	const auto peer = item->history()->peer;
	if (const auto channel = peer->asChannel()) {
		if (!channel->isMegagroup() && !channel->canEditMessages()) {
			return false;
		}
	}
	return true;
}

void SavePhotoToFile(not_null<PhotoData*> photo) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}

	const auto image = media->image(Data::PhotoSize::Large)->original(); // clazy:exclude=unused-non-trivial-variable
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		tr::lng_save_photo(tr::now),
		u"JPEG Image (*.jpg);;"_q + FileDialog::AllFilesFilter(),
		filedialogDefaultName(u"photo"_q, u".jpg"_q),
		crl::guard(&photo->session(), [=](const QString &result) {
			if (!result.isEmpty()) {
				media->saveToFile(result);
			}
		}));
}

void CopyImage(not_null<PhotoData*> photo) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}
	media->setToClipboard();
}

void ShowStickerPackInfo(
		not_null<DocumentData*> document,
		not_null<ListWidget*> list) {
	StickerSetBox::Show(list->controller()->uiShow(), document);
}

void ToggleFavedSticker(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	Api::ToggleFavedSticker(controller->uiShow(), document, contextId);
}

void AddPhotoActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<PhotoData*> photo,
		HistoryItem *item,
		not_null<ListWidget*> list) {
	const auto contextId = item ? item->fullId() : FullMsgId();
	if (!list->hasCopyMediaRestriction(item)) {
		menu->addAction(
			tr::lng_context_save_image(tr::now),
			base::fn_delayed(
				st::defaultDropdownMenu.menu.ripple.hideDuration,
				&photo->session(),
				[=] { SavePhotoToFile(photo); }),
			&st::menuIconSaveImage);
		menu->addAction(tr::lng_context_copy_image(tr::now), [=] {
			const auto item = photo->owner().message(contextId);
			if (!list->showCopyMediaRestriction(item)) {
				CopyImage(photo);
			}
		}, &st::menuIconCopy);
	}
	if (photo->hasAttachedStickers()) {
		const auto controller = list->controller();
		auto callback = [=] {
			auto &attached = photo->session().api().attachedStickers();
			attached.requestAttachedStickerSets(controller, photo);
		};
		menu->addAction(
			tr::lng_context_attached_stickers(tr::now),
			std::move(callback),
			&st::menuIconStickers);
	}
}

void SaveGif(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId) {
	if (const auto item = controller->session().data().message(itemId)) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				Api::ToggleSavedGif(
					controller->uiShow(),
					document,
					item->fullId(),
					true);
			}
		}
	}
}

void OpenGif(not_null<ListWidget*> list, FullMsgId itemId) {
	const auto controller = list->controller();
	if (const auto item = controller->session().data().message(itemId)) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				list->elementOpenDocument(document, itemId, true);
			}
		}
	}
}

void ShowInFolder(not_null<DocumentData*> document) {
	const auto filepath = document->filepath(true);
	if (!filepath.isEmpty()) {
		File::ShowInFolder(filepath);
	}
}

void AddDocumentActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<DocumentData*> document,
		HistoryItem *item,
		not_null<ListWidget*> list) {
	if (document->loading()) {
		menu->addAction(tr::lng_context_cancel_download(tr::now), [=] {
			document->cancel();
		}, &st::menuIconCancel);
		return;
	}
	const auto controller = list->controller();
	const auto contextId = item ? item->fullId() : FullMsgId();
	const auto session = &document->session();
	if (item && document->isGifv()) {
		const auto notAutoplayedGif = !Data::AutoDownload::ShouldAutoPlay(
			document->session().settings().autoDownload(),
			item->history()->peer,
			document);
		if (notAutoplayedGif) {
			const auto weak = base::make_weak(list.get());
			menu->addAction(tr::lng_context_open_gif(tr::now), [=] {
				if (const auto strong = weak.get()) {
					OpenGif(strong, contextId);
				}
			}, &st::menuIconShowInChat);
		}
		if (!list->hasCopyMediaRestriction(item)) {
			menu->addAction(tr::lng_context_save_gif(tr::now), [=] {
				SaveGif(list->controller(), contextId);
			}, &st::menuIconGif);
		}
	}
	if (document->sticker() && document->sticker()->set) {
		menu->addAction(
			(document->isStickerSetInstalled()
				? tr::lng_context_pack_info(tr::now)
				: tr::lng_context_pack_add(tr::now)),
			[=] { ShowStickerPackInfo(document, list); },
			&st::menuIconStickers);
		const auto isFaved = document->owner().stickers().isFaved(document);
		menu->addAction(
			(isFaved
				? tr::lng_faved_stickers_remove(tr::now)
				: tr::lng_faved_stickers_add(tr::now)),
			[=] { ToggleFavedSticker(controller, document, contextId); },
			isFaved ? &st::menuIconUnfave : &st::menuIconFave);
	}
	if (!document->filepath(true).isEmpty()) {
		menu->addAction(
			(Platform::IsMac()
				? tr::lng_context_show_in_finder(tr::now)
				: tr::lng_context_show_in_folder(tr::now)),
			[=] { ShowInFolder(document); },
			&st::menuIconShowInFolder);
	}
	if (document->hasAttachedStickers()) {
		const auto controller = list->controller();
		auto callback = [=] {
			auto &attached = session->api().attachedStickers();
			attached.requestAttachedStickerSets(controller, document);
		};
		menu->addAction(
			tr::lng_context_attached_stickers(tr::now),
			std::move(callback),
			&st::menuIconStickers);
	}
	if (item && !list->hasCopyMediaRestriction(item)) {
		const auto controller = list->controller();
		AddSaveSoundForNotifications(menu, item, document, controller);
	}
	if ((document->isVoiceMessage()
			|| document->isVideoMessage())
		&& Menu::HasRateTranscribeItem(item)) {
		if (!menu->empty()) {
			menu->insertAction(0, base::make_unique_q<Menu::RateTranscribe>(
				menu,
				menu->st().menu,
				Menu::RateTranscribeCallbackFactory(item)));
		}
	}
	AddSaveDocumentAction(menu, item, document, list);
	AddCopyFilename(
		menu,
		document,
		[=] { return list->showCopyRestrictionForSelected(); });
}

void AddPostLinkAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request) {
	const auto item = request.item;
	if (!item
		|| !item->hasDirectLink()
		|| request.pointState == PointState::Outside) {
		return;
	} else if (request.link
		&& !request.link->copyToClipboardContextItemText().isEmpty()) {
		return;
	}
	const auto itemId = item->fullId();
	const auto context = request.view
		? request.view->context()
		: Context::History;
	const auto controller = request.navigation->parentController();
	menu->addAction(
		kContextMenuActionMessageCopyPostLink,
		(item->history()->peer->isMegagroup()
			? tr::lng_context_copy_message_link
			: tr::lng_context_copy_post_link)(tr::now),
		[=] { CopyPostLink(controller, itemId, context); },
		&st::menuIconLink,
		true);
}

MessageIdsList ExtractIdsList(const SelectedItems &items) {
	return ranges::views::all(
		items
	) | ranges::views::transform(
		&SelectedItem::msgId
	) | ranges::to_vector;
}

bool AddForwardSelectedAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!request.overSelection || request.selectedItems.empty()) {
		return false;
	}
	if (!ranges::all_of(request.selectedItems, &SelectedItem::canForward)) {
		return false;
	}

	menu->addAction(kContextMenuActionSelectionForward, tr::lng_context_forward_selected(tr::now), [=] {
		const auto weak = base::make_weak(list);
		const auto callback = [=] {
			if (const auto strong = weak.get()) {
				strong->cancelSelection();
			}
		};
		Window::ShowForwardMessagesBox(
			request.navigation,
			ExtractIdsList(request.selectedItems),
			callback);
	}, &st::menuIconForward, true);
	menu->addAction(
		kContextMenuActionSelectionForwardWithoutAuthor,
		ForwardWithoutAuthorText(),
		[=] {
		const auto weak = base::make_weak(list);
		ShowForwardWithoutAuthorBox(
			request.navigation,
			ExtractIdsList(request.selectedItems),
			[=] {
				if (const auto strong = weak.get()) {
					strong->cancelSelection();
				}
			});
	}, &st::menuIconForward, true);
	menu->addAction(
		kContextMenuActionSelectionForwardSaved,
		AstrogramUiText("Forward to Saved Messages", "Переслать в Избранное"),
		[=] {
		const auto weak = base::make_weak(list);
		const auto ids = ExtractIdsList(request.selectedItems);
		if (ids.empty()) {
			return;
		}
		const auto first = request.navigation->session().data().message(ids.front());
		if (!first) {
			return;
		}
		const auto api = &first->history()->peer->session().api();
		const auto self = api->session().user()->asUser();
		auto action = Api::SendAction(first->history()->peer->owner().history(self));
		action.clearDraft = false;
		action.generateLocal = false;
		const auto history = first->history()->peer->owner().history(self);
		auto resolved = history->resolveForwardDraft(Data::ForwardDraft{ .ids = ids });
		api->forwardMessages(std::move(resolved), action, [=] {
			Ui::Toast::Show(AstrogramUiText("Forwarded to Saved Messages.", "Переслано в Избранное."));
			if (const auto strong = weak.get()) {
				strong->cancelSelection();
			}
		});
	}, &st::menuIconFave, true);
	return true;
}

bool AddForwardMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	if (!request.selectedItems.empty()) {
		return false;
	} else if (!item || !item->allowsForward()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	if (asGroup) {
		if (const auto group = owner->groups().find(item)) {
			if (!ranges::all_of(group->items, &HistoryItem::allowsForward)) {
				return false;
			}
		}
	}
	const auto itemId = item->fullId();
	menu->addAction(kContextMenuActionMessageForward, tr::lng_context_forward_msg(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			Window::ShowForwardMessagesBox(
				request.navigation,
				(asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId }));
		}
	}, &st::menuIconForward, true);
	menu->addAction(
		kContextMenuActionMessageForwardWithoutAuthor,
		ForwardWithoutAuthorText(),
		[=] {
		if (const auto item = owner->message(itemId)) {
			ShowForwardWithoutAuthorBox(
				request.navigation,
				(asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId }));
		}
	}, &st::menuIconForward, true);
	menu->addAction(
		kContextMenuActionMessageForwardSaved,
		AstrogramUiText("Forward to Saved Messages", "Переслать в Избранное"),
		[=] {
		if (const auto item = owner->message(itemId)) {
			const auto api = &item->history()->peer->session().api();
			const auto self = api->session().user()->asUser();
			auto action = Api::SendAction(item->history()->peer->owner().history(self));
			action.clearDraft = false;
			action.generateLocal = false;
			const auto history = item->history()->peer->owner().history(self);
			auto resolved = history->resolveForwardDraft(Data::ForwardDraft{
				.ids = (asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId })
			});
			api->forwardMessages(std::move(resolved), action, [] {
				Ui::Toast::Show(AstrogramUiText("Forwarded to Saved Messages.", "Переслано в Избранное."));
			});
		}
	}, &st::menuIconFave, true);
	return true;
}

void AddForwardAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddForwardSelectedAction(menu, request, list);
	AddForwardMessageAction(menu, request, list);
}

bool AddSendNowSelectedAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!request.overSelection || request.selectedItems.empty()) {
		return false;
	}
	if (!ranges::all_of(request.selectedItems, &SelectedItem::canSendNow)) {
		return false;
	}

	const auto session = &request.navigation->session();
	auto histories = ranges::views::all(
		request.selectedItems
	) | ranges::views::transform([&](const SelectedItem &item) {
		return session->data().message(item.msgId);
	}) | ranges::views::filter([](HistoryItem *item) {
		return item != nullptr;
	}) | ranges::views::transform(
		&HistoryItem::history
	);
	if (histories.begin() == histories.end()) {
		return false;
	}
	const auto history = *histories.begin();

	menu->addAction(kContextMenuActionSelectionSendNow, tr::lng_context_send_now_selected(tr::now), [=] {
		const auto weak = base::make_weak(list);
		const auto callback = [=] {
			request.navigation->showBackFromStack();
		};
		Window::ShowSendNowMessagesBox(
			request.navigation,
			history,
			ExtractIdsList(request.selectedItems),
			callback);
	}, &st::menuIconSend, true);
	return true;
}

bool AddSendNowMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	if (!request.selectedItems.empty()) {
		return false;
	} else if (!item || !item->allowsSendNow()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	if (asGroup) {
		if (const auto group = owner->groups().find(item)) {
			if (!ranges::all_of(group->items, &HistoryItem::allowsSendNow)) {
				return false;
			}
		}
	}
	const auto itemId = item->fullId();
	menu->addAction(kContextMenuActionMessageSendNow, tr::lng_context_send_now_msg(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			Window::ShowSendNowMessagesBox(
				request.navigation,
				item->history(),
				(asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId }));
		}
	}, &st::menuIconSend, true);
	return true;
}

bool AddRescheduleAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto owner = &request.navigation->session().data();

	const auto goodSingle = HasEditMessageAction(request, list)
		&& request.item->allowsReschedule();
	const auto goodMany = [&] {
		if (goodSingle) {
			return false;
		}
		const auto &items = request.selectedItems;
		if (!request.overSelection || items.empty()) {
			return false;
		}
		if (items.size() > kRescheduleLimit) {
			return false;
		}
		return ranges::all_of(items, &SelectedItem::canReschedule);
	}();
	if (!goodSingle && !goodMany) {
		return false;
	}
	auto ids = goodSingle
		? MessageIdsList{ request.item->fullId() }
		: ExtractIdsList(request.selectedItems);
	ranges::sort(ids, [&](const FullMsgId &a, const FullMsgId &b) {
		const auto itemA = owner->message(a);
		const auto itemB = owner->message(b);
		return (itemA && itemB) && (itemA->position() < itemB->position());
	});

	auto text = ((ids.size() == 1)
		? tr::lng_context_reschedule
		: tr::lng_context_reschedule_selected)(tr::now);

	menu->addAction(kContextMenuActionMessageReschedule, std::move(text), [=] {
		const auto firstItem = owner->message(ids.front());
		if (!firstItem) {
			return;
		}
		const auto callback = [=](Api::SendOptions options) {
			list->cancelSelection();
			auto groupedIds = std::vector<MessageGroupId>();
			for (const auto &id : ids) {
				const auto item = owner->message(id);
				if (!item || !item->isScheduled()) {
					continue;
				}
				if (const auto groupId = item->groupId()) {
					if (ranges::contains(groupedIds, groupId)) {
						continue;
					}
					groupedIds.push_back(groupId);
				}
				Api::RescheduleMessage(item, options);
				// Increase the scheduled date by 1s to keep the order.
				options.scheduled += 1;
			}
		};

		const auto peer = firstItem->history()->peer;
		const auto sendMenuType = !peer
			? SendMenu::Type::Disabled
			: peer->starsPerMessageChecked()
			? SendMenu::Type::SilentOnly
			: peer->isSelf()
			? SendMenu::Type::Reminder
			: HistoryView::CanScheduleUntilOnline(peer)
			? SendMenu::Type::ScheduledToUser
			: SendMenu::Type::Disabled;

		const auto itemDate = firstItem->date();
		const auto date = (itemDate == Api::kScheduledUntilOnlineTimestamp)
			? HistoryView::DefaultScheduleTime()
			: itemDate + (firstItem->isScheduled() ? 0 : crl::time(600));
		const auto repeatPeriod = firstItem->scheduleRepeatPeriod();

		const auto box = request.navigation->parentController()->show(
			HistoryView::PrepareScheduleBox(
				&request.navigation->session(),
				request.navigation->uiShow(),
				{ .type = sendMenuType, .effectAllowed = false },
				callback,
				{ .scheduleRepeatPeriod = repeatPeriod },
				date));

		owner->itemRemoved(
		) | rpl::on_next([=](not_null<const HistoryItem*> item) {
			if (ranges::contains(ids, item->fullId())) {
				box->closeBox();
			}
		}, box->lifetime());
	}, &st::menuIconReschedule);
	return true;
}

bool AddReplyToMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto context = list->elementContext();
	const auto item = request.quote.item
		? request.quote.item
		: request.item;
	const auto topic = item ? item->topic() : nullptr;
	const auto peer = item ? item->history()->peer.get() : nullptr;
	if (!item
		|| !item->isRegular()
		|| (context != Context::History
			&& context != Context::Replies
			&& context != Context::Monoforum)) {
		return false;
	}
	const auto canSendReply = topic
		? Data::CanSendAnything(topic)
		: Data::CanSendAnything(peer);
	const auto canReply = canSendReply || item->allowsForward();
	if (!canReply) {
		return false;
	}

	const auto todoListTaskId = request.link
		? request.link->property(kTodoListItemIdProperty).toInt()
		: 0;
	const auto &quote = request.quote;
	auto text = (todoListTaskId
		? tr::lng_context_reply_to_task
		: quote.highlight.quote.empty()
		? tr::lng_context_reply_msg
		: tr::lng_context_quote_and_reply)(
			tr::now,
			Ui::Text::FixAmpersandInAction);
	menu->addAction(kContextMenuActionMessageReply, std::move(text), [=, itemId = item->fullId()] {
		list->replyToMessageRequestNotify({
			.messageId = itemId,
			.quote = quote.highlight.quote,
			.quoteOffset = quote.highlight.quoteOffset,
			.todoItemId = todoListTaskId,
		}, base::IsCtrlPressed());
	}, &st::menuIconReply, true);
	return true;
}

bool AddTodoListAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto context = list->elementContext();
	const auto item = request.item;
	if (!item
		|| !Window::PeerMenuShowAddTodoListTasks(item)
		|| (context != Context::History
			&& context != Context::Replies
			&& context != Context::Monoforum
			&& context != Context::Pinned)) {
		return false;
	}
	const auto itemId = item->fullId();
	const auto controller = list->controller();
	menu->addAction(kContextMenuActionMessageTodoEdit, tr::lng_context_edit_msg(tr::now), [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			Window::PeerMenuEditTodoList(controller, item);
		}
	}, &st::menuIconEdit);
	menu->addAction(kContextMenuActionMessageTodoAdd, tr::lng_todo_add_title(tr::now), [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			Window::PeerMenuAddTodoListTasks(controller, item);
		}
	}, &st::menuIconAdd);
	return true;
}

bool AddViewRepliesAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto context = list->elementContext();
	const auto item = request.item;
	if (!item
		|| !item->isRegular()
		|| (context != Context::History && context != Context::Pinned)) {
		return false;
	}
	const auto topicRootId = item->history()->isForum()
		? item->topicRootId()
		: 0;
	const auto repliesCount = item->repliesCount();
	const auto withReplies = (repliesCount > 0);
	if (!withReplies || !item->history()->peer->isMegagroup()) {
		if (!topicRootId) {
			return false;
		}
	}
	const auto rootId = topicRootId
		? topicRootId
		: repliesCount
		? item->id
		: item->replyToTop();
	const auto highlightId = topicRootId ? item->id : 0;
	const auto phrase = topicRootId
		? tr::lng_replies_view_topic(tr::now)
		: (repliesCount > 0)
		? tr::lng_replies_view(
			tr::now,
			lt_count,
			repliesCount)
		: tr::lng_replies_view_thread(tr::now);
	const auto controller = list->controller();
	const auto history = item->history();
	menu->addAction(kContextMenuActionMessageViewReplies, phrase, crl::guard(controller, [=] {
		controller->showRepliesForMessage(
			history,
			rootId,
			highlightId);
	}), &st::menuIconViewReplies);
	return true;
}

bool AddEditMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!HasEditMessageAction(request, list)) {
		return false;
	}
	const auto item = request.item;
	if (!item->allowsEdit(base::unixtime::now())) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto itemId = item->fullId();
	menu->addAction(kContextMenuActionMessageEdit, tr::lng_context_edit_msg(tr::now), [=] {
		const auto item = owner->message(itemId);
		if (!item) {
			return;
		}
		list->editMessageRequestNotify(item->fullId());
	}, &st::menuIconEdit, true);
	return true;
}

bool AddEditHistoryAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (request.overSelection || !request.selectedItems.empty()) {
		return false;
	}
	const auto item = request.item;
	if (!item || item->isService() || !AyuMessages::hasRevisions(item)) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto itemId = item->fullId();
	menu->addAction(kContextMenuActionMessageEditHistory, AstrogramUiText(
		"Edit history",
		"История правок"), [=] {
		if (const auto current = owner->message(itemId)) {
			ShowEditHistoryBox(list->controller(), current);
		}
	}, &st::menuIconEdit);
	return true;
}

void AddFactcheckAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	if (!item || !item->history()->session().factchecks().canEdit(item)) {
		return;
	}
	const auto itemId = item->fullId();
	const auto text = item->factcheckText();
	const auto session = &item->history()->session();
	const auto phrase = text.empty()
		? tr::lng_context_add_factcheck(tr::now)
		: tr::lng_context_edit_factcheck(tr::now);
	menu->addAction(kContextMenuActionMessageFactcheck, phrase, [=] {
		const auto limit = session->factchecks().lengthLimit();
		const auto controller = request.navigation->parentController();
		controller->show(Box(EditFactcheckBox, text, limit, [=](
				TextWithEntities result) {
			const auto show = controller->uiShow();
			session->factchecks().save(itemId, text, result, show);
		}, FactcheckFieldIniter(controller->uiShow())));
	}, &st::menuIconFactcheck);
}

bool AddPinMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto context = list->elementContext();
	const auto item = request.item;
	if (!item || !item->isRegular()) {
		return false;
	}
	const auto topic = item->topic();
	const auto sublist = item->savedSublist();
	if (context != Context::History && context != Context::Pinned) {
		if ((context != Context::Replies || !topic)
			&& (context != Context::Monoforum
				|| !sublist
				|| !item->history()->amMonoforumAdmin())) {
			return false;
		}
	}
	const auto group = item->history()->owner().groups().find(item);
	const auto pinItem = ((item->canPin() && item->isPinned()) || !group)
		? item
		: group->items.front().get();
	if (!pinItem->canPin()) {
		return false;
	}
	const auto pinItemId = pinItem->fullId();
	const auto isPinned = pinItem->isPinned();
	const auto controller = list->controller();
	menu->addAction(kContextMenuActionMessagePin, isPinned ? tr::lng_context_unpin_msg(tr::now) : tr::lng_context_pin_msg(tr::now), crl::guard(controller, [=] {
		Window::ToggleMessagePinned(controller, pinItemId, !isPinned);
	}), isPinned ? &st::menuIconUnpin : &st::menuIconPin);
	return true;
}

bool AddGoToMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto context = list->elementContext();
	const auto view = request.view;
	if (!view
		|| !view->data()->isRegular()
		|| context != Context::Pinned
		|| !view->hasOutLayout()) {
		return false;
	}
	const auto itemId = view->data()->fullId();
	const auto controller = list->controller();
	menu->addAction(kContextMenuActionMessageGoTo, tr::lng_context_to_msg(tr::now), crl::guard(controller, [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showMessage(item);
		}
	}), &st::menuIconShowInChat);
	return true;
}

void AddSendNowAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddSendNowSelectedAction(menu, request, list);
	AddSendNowMessageAction(menu, request, list);
}

bool AddDeleteSelectedAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!request.overSelection || request.selectedItems.empty()) {
		return false;
	}
	if (!ranges::all_of(request.selectedItems, &SelectedItem::canDelete)) {
		return false;
	}

	menu->addAction(kContextMenuActionSelectionDelete, tr::lng_context_delete_selected(tr::now), [=] {
		auto items = ExtractIdsList(request.selectedItems);
		auto box = Box<DeleteMessagesBox>(
			&request.navigation->session(),
			std::move(items));
		box->setDeleteConfirmedCallback(crl::guard(list, [=] {
			list->cancelSelection();
		}));
		request.navigation->parentController()->show(std::move(box));
	}, &st::menuIconDelete);
	return true;
}

bool AddDeleteMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	if (!request.selectedItems.empty()) {
		return false;
	} else if (!item || !item->canDelete()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	if (asGroup) {
		if (const auto group = owner->groups().find(item)) {
			if (ranges::any_of(group->items, [](auto item) {
				return item->isLocal() || !item->canDelete();
			})) {
				return false;
			}
		}
	}
	const auto controller = list->controller();
	const auto itemId = item->fullId();
	const auto callback = crl::guard(controller, [=] {
		if (const auto item = owner->message(itemId)) {
			if (asGroup) {
				if (const auto group = owner->groups().find(item)) {
					controller->show(Box<DeleteMessagesBox>(
						&owner->session(),
						owner->itemsToIds(group->items)));
					return;
				}
			}
			if (item->isUploading()) {
				controller->cancelUploadLayer(item);
				return;
			}
			const auto list = HistoryItemsList{ item };
			if (CanCreateModerateMessagesBox(list)) {
				controller->show(
					Box(CreateModerateMessagesBox, list, nullptr));
			} else {
				const auto suggestModerateActions = false;
				controller->show(
					Box<DeleteMessagesBox>(item, suggestModerateActions));
			}
		}
	});
	if (item->isUploading()) {
		menu->addAction(
			kContextMenuActionMessageDelete,
			tr::lng_context_cancel_upload(tr::now),
			callback,
			&st::menuIconCancel);
		return true;
	}
	menu->addCustom(
		kContextMenuActionMessageDelete,
		[=](not_null<Ui::PopupMenu*> raw) {
			raw->addAction(Ui::DeleteMessageContextAction(
				raw->menu(),
				callback,
				item->ttlDestroyAt(),
				[=] { delete raw; }));
		},
		&st::menuIconDelete);
	return true;
}

void AddDeleteAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!AddDeleteSelectedAction(menu, request, list)) {
		AddDeleteMessageAction(menu, request, list);
	}
}

void AddDownloadFilesAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!request.overSelection
		|| request.selectedItems.empty()
		|| list->hasCopyRestrictionForSelected()) {
		return;
	}
	menu->addCustom(
		kContextMenuActionSelectionDownloadFiles,
		[=](not_null<Ui::PopupMenu*> raw) {
			Menu::AddDownloadFilesAction(
				raw,
				request.navigation->parentController(),
				request.selectedItems,
				list);
		},
		&st::menuIconDownload);
}

void AddReportAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	if (!request.selectedItems.empty()) {
		return;
	} else if (!item || !item->suggestReport()) {
		return;
	}
	const auto owner = &item->history()->owner();
	const auto controller = list->controller();
	const auto itemId = item->fullId();
	const auto callback = crl::guard(controller, [=] {
		if (const auto item = owner->message(itemId)) {
			const auto group = owner->groups().find(item);
			const auto ids = group
				? (ranges::views::all(
					group->items
				) | ranges::views::transform([](const auto &i) {
					return i->fullId().msg;
				}) | ranges::to_vector)
				: std::vector<MsgId>{ 1, itemId.msg };
			const auto peer = item->history()->peer;
			ShowReportMessageBox(controller->uiShow(), peer, ids, {});
		}
	});
	menu->addAction(
		kContextMenuActionMessageReport,
		tr::lng_context_report_msg(tr::now),
		callback,
		&st::menuIconReport);
}

bool AddClearSelectionAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!request.overSelection || request.selectedItems.empty()) {
		return false;
	}
	menu->addAction(kContextMenuActionSelectionClear, tr::lng_context_clear_selection(tr::now), [=] {
		list->cancelSelection();
	}, &st::menuIconSelect, true);
	return true;
}

bool AddSelectMessageAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	if (request.overSelection && !request.selectedItems.empty()) {
		return false;
	} else if (!item
		|| item->isLocal()
		|| item->isService()
		|| list->hasSelectRestriction()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto itemId = item->fullId();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	menu->addAction(kContextMenuActionMessageSelect, tr::lng_context_select_msg(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			if (asGroup) {
				list->selectItemAsGroup(item);
			} else {
				list->selectItem(item);
			}
		}
	}, &st::menuIconSelect, true);
	return true;
}

void AddSelectionAction(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!AddClearSelectionAction(menu, request, list)) {
		AddSelectMessageAction(menu, request, list);
	}
}

void AddTopMessageActions(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddGoToMessageAction(menu, request, list);
	AddViewRepliesAction(menu, request, list);
	AddEditMessageAction(menu, request, list);
	AddEditHistoryAction(menu, request, list);
	if (const auto item = request.item; item && request.selectedItems.empty()) {
		menu->addAction(
			kContextMenuActionMessageCopyIdsTime,
			AstrogramUiText("Copy IDs and time", "Скопировать ID и время"),
			[=] {
				const auto peerId = item->history()->peer->id.value;
				const auto messageId = item->id.bare;
				const auto dt = QDateTime::fromSecsSinceEpoch(item->date()).toLocalTime();
				const auto text = AstrogramUiText(
					"Chat ID: %1\nMessage ID: %2\nService time: %3",
					"Chat ID: %1\nMessage ID: %2\nСлужебное время: %3"
				).arg(QString::number(static_cast<qulonglong>(peerId)))
				 .arg(QString::number(messageId))
				 .arg(dt.toString(Qt::ISODate));
				if (const auto clipboard = QGuiApplication::clipboard()) {
					clipboard->setText(text, QClipboard::Clipboard);
				}
				list->controller()->window().showToast(AstrogramUiText(
					"IDs copied.",
					"ID скопированы."));
			},
			&st::menuIconCopy);
	}
	AddFactcheckAction(menu, request, list);
	AddPinMessageAction(menu, request, list);
}

void AddMessageActions(
		ContextMenuBuilder *menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddPostLinkAction(menu, request);
	AddForwardAction(menu, request, list);
	AddSendNowAction(menu, request, list);
	AddDeleteAction(menu, request, list);
	AddDownloadFilesAction(menu, request, list);
	AddReportAction(menu, request, list);
	AddSelectionAction(menu, request, list);
	AddRescheduleAction(menu, request, list);
}

void AddCopyLinkAction(
		ContextMenuBuilder *menu,
		const ClickHandlerPtr &link) {
	if (!link) {
		return;
	}
	const auto action = link->copyToClipboardContextItemText();
	if (action.isEmpty()) {
		return;
	}
	const auto text = link->copyToClipboardText();
	menu->addAction(
		kContextMenuActionLinkCopy,
		action,
		[=] { QGuiApplication::clipboard()->setText(text); },
		&st::menuIconCopy,
		true);
}

void EditTagBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const Data::ReactionId &id) {
	const auto owner = &controller->session().data();
	const auto title = owner->reactions().myTagTitle(id);
	box->setTitle(title.isEmpty()
		? tr::lng_context_tag_add_name()
		: tr::lng_context_tag_edit_name());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_edit_tag_about(),
		st::editTagAbout));
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::editTagField,
		tr::lng_edit_tag_name(),
		title));
	field->setMaxLength(kTagNameLimit * 2);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	struct State {
		std::unique_ptr<Ui::Text::CustomEmoji> custom;
		QImage image;
	};
	const auto state = field->lifetime().make_state<State>();

	if (const auto customId = id.custom()) {
		state->custom = owner->customEmojiManager().create(
			customId,
			[=] { field->update(); });
	} else {
		owner->reactions().preloadReactionImageFor(id);
	}
	field->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(field);
		const auto top = st::editTagField.textMargins.top();
		if (const auto custom = state->custom.get()) {
			const auto inactive = !field->window()->isActiveWindow();
			custom->paint(p, {
				.textColor = st::windowFg->c,
				.now = crl::now(),
				.position = QPoint(0, top),
				.paused = inactive || On(PowerSaving::kEmojiChat),
			});
		} else {
			if (state->image.isNull()) {
				state->image = owner->reactions().resolveReactionImageFor(
					id);
			}
			if (!state->image.isNull()) {
				const auto size = st::reactionInlineSize;
				const auto skip = (size - st::reactionInlineImage) / 2;
				p.drawImage(skip, top + skip, state->image);
			}
		}
	}, field->lifetime());

	Ui::AddLengthLimitLabel(field, kTagNameLimit);

	const auto save = [=] {
		const auto text = field->getLastText();
		if (text.size() > kTagNameLimit) {
			field->showError();
			return;
		}
		const auto weak = base::make_weak(box);
		controller->session().data().reactions().renameTag(id, text);
		if (const auto strong = weak.get()) {
			strong->closeBox();
		}
	};

	field->submits(
	) | rpl::on_next(save, field->lifetime());

	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void ShowWhoReadInfo(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId,
		Ui::WhoReadParticipant who) {
	const auto peer = controller->session().data().peer(itemId.peer);
	const auto participant = peer->owner().peer(PeerId(who.id));
	const auto migrated = participant->migrateFrom();
	const auto origin = who.dateReacted
		? Info::Profile::Origin{
			Info::Profile::GroupReactionOrigin{ peer, itemId.msg },
		}
		: Info::Profile::Origin();
	auto memento = std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<Info::ContentMemento>>{
		std::make_shared<Info::Profile::Memento>(
			participant,
			migrated ? migrated->id : PeerId(),
			origin),
	});
	controller->showSection(std::move(memento));
}

[[nodiscard]] rpl::producer<not_null<UserData*>> LookupMessageAuthor(
		not_null<HistoryItem*> item) {
	struct Author {
		UserData *user = nullptr;
		std::vector<Fn<void(UserData*)>> callbacks;
	};
	struct Authors {
		base::flat_map<FullMsgId, Author> map;
	};
	static auto Cache = base::flat_map<not_null<Main::Session*>, Authors>();

	const auto channel = item->history()->peer->asChannel();
	const auto session = &channel->session();
	const auto id = item->fullId();
	if (!Cache.contains(session)) {
		Cache.emplace(session);
		session->lifetime().add([session] {
			Cache.remove(session);
		});
	}

	return [channel, id](auto consumer) {
		const auto session = &channel->session();
		auto &map = Cache[session].map;
		auto i = map.find(id);
		if (i == end(map)) {
			i = map.emplace(id).first;
			const auto finishWith = [=](UserData *user) {
				auto &entry = Cache[session].map[id];
				entry.user = user;
				for (const auto &callback : base::take(entry.callbacks)) {
					callback(user);
				}
			};
			session->api().request(MTPchannels_GetMessageAuthor(
				channel->inputChannel(),
				MTP_int(id.msg.bare)
			)).done([=](const MTPUser &result) {
				finishWith(session->data().processUser(result));
			}).fail([=] {
				finishWith(nullptr);
			}).send();
		} else if (const auto user = i->second.user
			; user || i->second.callbacks.empty()) {
			if (user) {
				consumer.put_next(not_null(user));
			}
			return rpl::lifetime();
		}

		auto lifetime = rpl::lifetime();
		const auto done = [=](UserData *result) {
			if (result) {
				consumer.put_next(not_null(result));
			}
		};
		const auto guard = lifetime.make_state<base::has_weak_ptr>();
		i->second.callbacks.push_back(crl::guard(guard, done));
		return lifetime;
	};
}

[[nodiscard]] base::unique_qptr<Ui::Menu::ItemBase> MakeMessageAuthorAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller) {
	const auto parent = menu->menu();
	const auto user = std::make_shared<UserData*>(nullptr);
	const auto action = Ui::Menu::CreateAction(
		parent,
		tr::lng_contacts_loading(tr::now),
		[=] { if (*user) { controller->showPeerInfo(*user); } });
	action->setDisabled(true);
	auto lifetime = LookupMessageAuthor(
		item
	) | rpl::on_next([=](not_null<UserData*> author) {
		action->setText(
			tr::lng_context_sent_by(tr::now, lt_user, author->name()));
		action->setDisabled(false);
		*user = author;
	});
	auto result = base::make_unique_q<Ui::Menu::Action>(
		menu->menu(),
		st::whoSentItem,
		action,
		nullptr,
		nullptr);
	result->lifetime().add(std::move(lifetime));
	return result;
}

} // namespace

ContextMenuCustomizationLayout DefaultContextMenuCustomizationLayout() {
	auto result = ContextMenuCustomizationLayout();
	result.version = kContextLayoutVersion;
	result.message.menu = {
		MakeLayoutEntry(kContextMenuActionMessageGoTo),
		MakeLayoutEntry(kContextMenuActionMessageViewReplies),
		MakeLayoutEntry(kContextMenuActionMessageReply),
		MakeLayoutEntry(kContextMenuActionMessageTodoEdit),
		MakeLayoutEntry(kContextMenuActionMessageTodoAdd),
		MakeLayoutEntry(kContextMenuActionMessageEdit),
		MakeLayoutEntry(kContextMenuActionMessageEditHistory),
		MakeLayoutEntry(kContextMenuActionMessageCopyIdsTime),
		MakeLayoutEntry(kContextMenuActionMessageFactcheck),
		MakeLayoutEntry(kContextMenuActionMessagePin),
		MakeLayoutEntry(kContextMenuActionMessageCopyText),
		MakeLayoutEntry(kContextMenuActionMessageTranslate),
		MakeLayoutEntry(kContextMenuActionLinkCopy),
		MakeLayoutEntry(kContextMenuActionMessageCopyPostLink),
		MakeLayoutEntry(kContextMenuActionMessageForward),
		MakeLayoutEntry(kContextMenuActionMessageForwardWithoutAuthor),
		MakeLayoutEntry(kContextMenuActionMessageForwardSaved),
		MakeLayoutEntry(kContextMenuActionMessageSendNow),
		MakeLayoutEntry(kContextMenuActionMessageDelete),
		MakeLayoutEntry(kContextMenuActionMessageReport),
		MakeLayoutEntry(kContextMenuActionMessageSelect),
		MakeLayoutEntry(kContextMenuActionMessageReschedule),
	};
	result.message.strip = {
		MakeLayoutEntry(kContextMenuActionMessageReply),
		MakeLayoutEntry(kContextMenuActionMessageCopyText),
		MakeLayoutEntry(kContextMenuActionLinkCopy),
		MakeLayoutEntry(kContextMenuActionMessageTranslate),
		MakeLayoutEntry(kContextMenuActionMessageForward),
		MakeLayoutEntry(kContextMenuActionMessageForwardWithoutAuthor),
		MakeLayoutEntry(kContextMenuActionMessageSelect),
	};
	result.selection.menu = {
		MakeLayoutEntry(kContextMenuActionSelectionCopy),
		MakeLayoutEntry(kContextMenuActionSelectionTranslate),
		MakeLayoutEntry(kContextMenuActionSelectionSearch),
		MakeLayoutEntry(kContextMenuActionSelectionForward),
		MakeLayoutEntry(kContextMenuActionSelectionForwardWithoutAuthor),
		MakeLayoutEntry(kContextMenuActionSelectionForwardSaved),
		MakeLayoutEntry(kContextMenuActionSelectionSendNow),
		MakeLayoutEntry(kContextMenuActionSelectionDelete),
		MakeLayoutEntry(kContextMenuActionSelectionDownloadFiles),
		MakeLayoutEntry(kContextMenuActionSelectionClear),
	};
	result.selection.strip = {
		MakeLayoutEntry(kContextMenuActionSelectionCopy),
		MakeLayoutEntry(kContextMenuActionSelectionTranslate),
		MakeLayoutEntry(kContextMenuActionSelectionSearch),
		MakeLayoutEntry(kContextMenuActionSelectionForward),
		MakeLayoutEntry(kContextMenuActionSelectionForwardWithoutAuthor),
		MakeLayoutEntry(kContextMenuActionSelectionClear),
	};
	return result;
}

namespace {

[[nodiscard]] std::vector<ContextMenuLayoutEntry> ParseLayoutEntries(
		const QJsonValue &value,
		const std::vector<ContextMenuLayoutEntry> &fallback) {
	const auto array = value.toArray();
	if (array.isEmpty()) {
		return fallback;
	}
	auto result = std::vector<ContextMenuLayoutEntry>();
	result.reserve(array.size() + int(fallback.size()));
	auto seen = base::flat_set<QString>();
	for (const auto &entry : array) {
		const auto object = entry.toObject();
		const auto id = object.value(u"id"_q).toString().trimmed();
		if (id.isEmpty() || seen.contains(id)) {
			continue;
		}
		seen.emplace(id);
		result.push_back({
			.id = id,
			.visible = object.contains(u"visible"_q)
				? object.value(u"visible"_q).toBool(true)
				: true,
		});
	}
	for (const auto &entry : fallback) {
		if (!seen.contains(entry.id)) {
			result.push_back(entry);
		}
	}
	return result.empty() ? fallback : result;
}

[[nodiscard]] ContextMenuSurfaceLayout ParseSurfaceLayout(
		const QJsonValue &value,
		const ContextMenuSurfaceLayout &fallback) {
	const auto object = value.toObject();
	auto result = fallback;
	result.menu = ParseLayoutEntries(object.value(u"menu"_q), fallback.menu);
	result.strip = ParseLayoutEntries(object.value(u"strip"_q), fallback.strip);
	return result;
}

[[nodiscard]] QJsonArray SerializeLayoutEntries(
		const std::vector<ContextMenuLayoutEntry> &entries) {
	auto result = QJsonArray();
	for (const auto &entry : entries) {
		auto object = QJsonObject();
		object.insert(u"id"_q, entry.id);
		object.insert(u"visible"_q, entry.visible);
		result.push_back(object);
	}
	return result;
}

[[nodiscard]] QJsonObject SerializeSurfaceLayout(
		const ContextMenuSurfaceLayout &layout) {
	auto result = QJsonObject();
	result.insert(u"menu"_q, SerializeLayoutEntries(layout.menu));
	result.insert(u"strip"_q, SerializeLayoutEntries(layout.strip));
	return result;
}

} // namespace

ContextMenuCustomizationLayout ParseContextMenuCustomizationLayout(
		const QJsonObject &json) {
	auto result = DefaultContextMenuCustomizationLayout();
	result.version = json.value(u"version"_q).toInt(kContextLayoutVersion);
	result.message = ParseSurfaceLayout(json.value(u"message"_q), result.message);
	result.selection = ParseSurfaceLayout(
		json.value(u"selection"_q),
		result.selection);
	return result;
}

QJsonObject SerializeContextMenuCustomizationLayout(
		const ContextMenuCustomizationLayout &layout) {
	auto result = QJsonObject();
	result.insert(u"version"_q, layout.version);
	result.insert(u"message"_q, SerializeSurfaceLayout(layout.message));
	result.insert(u"selection"_q, SerializeSurfaceLayout(layout.selection));
	return result;
}

const ContextMenuSurfaceLayout &LookupContextMenuSurfaceLayout(
		const ContextMenuCustomizationLayout &layout,
		ContextMenuSurface surface) {
	return (surface == ContextMenuSurface::Selection)
		? layout.selection
		: layout.message;
}

ContextMenuRequest::ContextMenuRequest(
	not_null<Window::SessionNavigation*> navigation)
: navigation(navigation) {
}

base::unique_qptr<Ui::PopupMenu> FillContextMenu(
		not_null<ListWidget*> list,
		const ContextMenuRequest &request,
		ContextMenuResolvedLayout *resolved) {
	const auto link = request.link;
	const auto view = request.view;
	const auto item = request.item;
	const auto itemId = item ? item->fullId() : FullMsgId();
	const auto lnkPhoto = link
		? reinterpret_cast<PhotoData*>(
			link->property(kPhotoLinkMediaProperty).toULongLong())
		: nullptr;
	const auto lnkDocument = link
		? reinterpret_cast<DocumentData*>(
			link->property(kDocumentLinkMediaProperty).toULongLong())
		: nullptr;
	const auto poll = item
		? (item->media() ? item->media()->poll() : nullptr)
		: nullptr;
	const auto hasSelection = !request.selectedItems.empty()
		|| !request.selectedText.empty();
	const auto hasWhoReactedItem = item
		&& Api::WhoReactedExists(item, Api::WhoReactedList::All);

	auto result = base::make_unique_q<Ui::PopupMenu>(
		list,
		st::popupMenuWithIcons);
	auto builder = ContextMenuBuilder(
		result.get(),
		resolved,
		request.overSelection
			? ContextMenuSurface::Selection
			: ContextMenuSurface::Message);

	AddReplyToMessageAction(&builder, request, list);
	AddTodoListAction(&builder, request, list);

	if (request.overSelection
		&& !list->hasCopyRestrictionForSelected()
		&& !list->getSelectedText().empty()) {
		const auto text = request.selectedItems.empty()
			? tr::lng_context_copy_selected(tr::now)
			: tr::lng_context_copy_selected_items(tr::now);
		builder.addAction(kContextMenuActionSelectionCopy, text, [=] {
			if (!list->showCopyRestrictionForSelected()) {
				TextUtilities::SetClipboardText(list->getSelectedText());
			}
		}, &st::menuIconCopy, true);
	}
	if (request.overSelection
		&& !Ui::SkipTranslate(list->getSelectedText().rich)) {
		const auto owner = &view->history()->owner();
		builder.addAction(
			kContextMenuActionSelectionTranslate,
			tr::lng_context_translate_selected(tr::now),
			[=] {
			if (const auto item = owner->message(itemId)) {
				list->controller()->show(Box(
					Ui::TranslateBox,
					item->history()->peer,
					MsgId(),
					list->getSelectedText().rich,
					list->hasCopyRestrictionForSelected()));
			}
		}, &st::menuIconTranslate, true);
	}
	if (request.overSelection && !list->getSelectedText().rich.text.isEmpty()) {
		builder.addAction(
			kContextMenuActionSelectionSearch,
			AstrogramUiText("Search selected text", "Искать выделенный текст"),
			[=] {
				const auto query = QUrl::toPercentEncoding(
					list->getSelectedText().rich.text.trimmed());
				if (!query.isEmpty()) {
					QDesktopServices::openUrl(QUrl(
						u"https://www.google.com/search?q="_q + QString::fromLatin1(query)));
				}
			},
			&st::menuIconSearch,
			true);
	}

	AddTopMessageActions(&builder, request, list);
	if (lnkPhoto && request.selectedItems.empty()) {
		AddPhotoActions(result, lnkPhoto, item, list);
	} else if (lnkDocument) {
		AddDocumentActions(result, lnkDocument, item, list);
	} else if (poll) {
		const auto context = list->elementContext();
		AddPollActions(result, poll, item, context, list->controller());
	} else if (!request.overSelection && view && !hasSelection) {
		const auto owner = &view->history()->owner();
		const auto media = view->media();
		const auto mediaHasTextForCopy = media && media->hasTextForCopy();
		if (const auto document = media ? media->getDocument() : nullptr) {
			AddDocumentActions(result, document, view->data(), list);
		}
		if (!link && (view->hasVisibleText() || mediaHasTextForCopy)) {
			if (!list->hasCopyRestriction(view->data())) {
				const auto asGroup = (request.pointState != PointState::GroupPart);
				builder.addAction(
					kContextMenuActionMessageCopyText,
					tr::lng_context_copy_text(tr::now),
					[=] {
					if (const auto item = owner->message(itemId)) {
						if (!list->showCopyRestriction(item)) {
							if (asGroup) {
								if (const auto group = owner->groups().find(item)) {
									TextUtilities::SetClipboardText(HistoryGroupText(group));
									return;
								}
							}
							TextUtilities::SetClipboardText(HistoryItemText(item));
						}
					}
				}, &st::menuIconCopy, true);
			}

			const auto translate = mediaHasTextForCopy
				? (HistoryView::TransribedText(item)
					.append('\n')
					.append(item->originalText()))
				: item->originalText();
			if ((!item->translation() || !item->history()->translatedTo())
				&& !translate.text.isEmpty()
				&& !Ui::SkipTranslate(translate)) {
				builder.addAction(
					kContextMenuActionMessageTranslate,
					tr::lng_context_translate(tr::now),
					[=] {
					if (const auto item = owner->message(itemId)) {
						list->controller()->show(Box(
							Ui::TranslateBox,
							item->history()->peer,
							mediaHasTextForCopy
								? MsgId()
								: item->fullId().msg,
							translate,
							list->hasCopyRestriction(view->data())));
					}
				}, &st::menuIconTranslate, true);
			}
		}
	}

	AddCopyLinkAction(&builder, link);
	AddMessageActions(&builder, request, list);

	const auto wasAmount = result->actions().size();
	if (const auto textItem = view ? view->textItem() : item) {
		AddEmojiPacksAction(
			result,
			textItem,
			HistoryView::EmojiPacksSource::Message,
			list->controller());
	}
	if (item) {
		const auto added = (result->actions().size() > wasAmount);
		AddSelectRestrictionAction(result, item, !added);
	}
	if (hasWhoReactedItem) {
		AddWhoReactedAction(result, list, item, list->controller());
	} else if (item) {
		MaybeAddWhenEditedForwardedAction(result, item, list->controller());
	}

	return result;
}

void CopyPostLink(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId,
		Context context,
		std::optional<TimeId> videoTimestamp) {
	CopyPostLink(controller->uiShow(), itemId, context, videoTimestamp);
}

void CopyPostLink(
		std::shared_ptr<Main::SessionShow> show,
		FullMsgId itemId,
		Context context,
		std::optional<TimeId> videoTimestamp) {
	const auto item = show->session().data().message(itemId);
	if (!item || !item->hasDirectLink()) {
		return;
	}
	const auto inRepliesContext = (context == Context::Replies);
	const auto forceNonPublicLink = !videoTimestamp && base::IsCtrlPressed();
	QGuiApplication::clipboard()->setText(
		item->history()->session().api().exportDirectMessageLink(
			item,
			inRepliesContext,
			forceNonPublicLink,
			videoTimestamp));

	const auto isPublicLink = [&] {
		if (forceNonPublicLink) {
			return false;
		}
		const auto channel = item->history()->peer->asChannel();
		Assert(channel != nullptr);
		if (const auto rootId = item->replyToTop()) {
			const auto root = item->history()->owner().message(
				channel->id,
				rootId);
			const auto sender = root
				? root->discussionPostOriginalSender()
				: nullptr;
			if (sender && sender->hasUsername()) {
				return true;
			}
		}
		return channel->hasUsername();
	}();
	if (isPublicLink && !videoTimestamp) {
		show->showToast({
			.text = tr::lng_channel_public_link_copied(
				tr::now, tr::bold
			).append('\n').append(Platform::IsMac()
				? tr::lng_public_post_private_hint_cmd(tr::now)
				: tr::lng_public_post_private_hint_ctrl(tr::now)),
			.duration = kPublicPostLinkToastDuration,
		});
	} else {
		show->showToast(isPublicLink
			? tr::lng_channel_public_link_copied(tr::now)
			: tr::lng_context_about_private_link(tr::now));
	}
}

void CopyStoryLink(
		std::shared_ptr<Main::SessionShow> show,
		FullStoryId storyId) {
	const auto session = &show->session();
	const auto maybeStory = session->data().stories().lookup(storyId);
	if (!maybeStory) {
		return;
	}
	const auto story = *maybeStory;
	QGuiApplication::clipboard()->setText(
		session->api().exportDirectStoryLink(story));
	show->showToast(tr::lng_channel_public_link_copied(tr::now));
}

void AddPollActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<PollData*> poll,
		not_null<HistoryItem*> item,
		Context context,
		not_null<Window::SessionController*> controller) {
	{
		constexpr auto kRadio = "\xf0\x9f\x94\x98";
		const auto radio = QString::fromUtf8(kRadio);
		auto text = poll->question;
		for (const auto &answer : poll->answers) {
			text.append('\n').append(radio).append(answer.text);
		}
		if (!Ui::SkipTranslate(text)) {
			menu->addAction(tr::lng_context_translate(tr::now), [=] {
				controller->show(Box(
					Ui::TranslateBox,
					item->history()->peer,
					MsgId(),
					std::move(text),
					item->forbidsForward()));
			}, &st::menuIconTranslate);
		}
	}
	if ((context != Context::History)
		&& (context != Context::Replies)
		&& (context != Context::Pinned)
		&& (context != Context::ScheduledTopic)) {
		return;
	}
	const auto itemId = item->fullId();
	if (!poll->quiz() || poll->closed() || poll->voted()) {
		menu->addAction(tr::lng_polls_view_results(tr::now), [=] {
			controller->showPollResults(poll, itemId);
		});
	}
	if (poll->closed()) {
		return;
	}
	if (poll->voted() && !poll->quiz()) {
		menu->addAction(tr::lng_polls_retract(tr::now), [=] {
			poll->session().api().polls().sendVotes(itemId, {});
		}, &st::menuIconRetractVote);
	}
	if (item->canStopPoll()) {
		menu->addAction(tr::lng_polls_stop(tr::now), [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = tr::lng_polls_stop_warning(),
				.confirmed = [=](Fn<void()> &&close) {
					close();
					if (const auto item = poll->owner().message(itemId)) {
						controller->session().api().polls().close(item);
					}
				},
				.confirmText = tr::lng_polls_stop_sure(),
				.cancelText = tr::lng_cancel(),
			}));
		}, &st::menuIconRemove);
	}
}

void AddSaveSoundForNotifications(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		not_null<Window::SessionController*> controller) {
	if (ItemHasTtl(item)) {
		return;
	}
	const auto &ringtones = document->session().api().ringtones();
	if (document->size > ringtones.maxSize()) {
		return;
	} else if (ranges::contains(ringtones.list(), document->id)) {
		return;
	} else if (int(ringtones.list().size()) >= ringtones.maxSavedCount()) {
		return;
	} else if (document->song()) {
		if (document->duration() > ringtones.maxDuration()) {
			return;
		}
	} else if (document->voice()) {
		if (document->duration() > ringtones.maxDuration()) {
			return;
		}
	} else {
		return;
	}
	const auto show = controller->uiShow();
	menu->addAction(tr::lng_context_save_custom_sound(tr::now), [=] {
		Api::ToggleSavedRingtone(
			document,
			item->fullId(),
			[=] { show->showToast(
				tr::lng_ringtones_toast_added(tr::now)); },
			true);
	}, &st::menuIconSoundAdd);
}

void AddWhenEditedForwardedAuthorActionHelper(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller,
		bool insertSeparator) {
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (!forwarded->story && forwarded->psaType.isEmpty()) {
			if (insertSeparator && !menu->empty()) {
				menu->addSeparator(&st::expandedMenuSeparator);
			}
			menu->addAction(Ui::WhenReadContextAction(
				menu.get(),
				Api::WhenOriginal(item->from(), forwarded->originalDate)));
		}
	} else if (const auto edited = item->Get<HistoryMessageEdited>()) {
		if (!item->hideEditedBadge()) {
			if (insertSeparator && !menu->empty()) {
				menu->addSeparator(&st::expandedMenuSeparator);
			}
			menu->addAction(Ui::WhenReadContextAction(
				menu.get(),
				Api::WhenEdited(item->from(), edited->date)));
		}
	}
	if (item->canLookupMessageAuthor()) {
		if (insertSeparator && !menu->empty()) {
			menu->addSeparator(&st::expandedMenuSeparator);
		}
		menu->addAction(MakeMessageAuthorAction(menu, item, controller));
	}
}

void AddWhoReactedAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<QWidget*> context,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller) {
	const auto whoReadIds = std::make_shared<Api::WhoReadList>();
	const auto weak = base::make_weak(menu.get());
	const auto user = item->history()->peer;
	const auto showOrPremium = [=] {
		if (const auto strong = weak.get()) {
			strong->hideMenu();
		}
		const auto type = Ui::ShowOrPremium::ReadTime;
		const auto name = user->shortName();
		auto box = Box(Ui::ShowOrPremiumBox, type, name, [=] {
			const auto api = &controller->session().api();
			api->globalPrivacy().updateHideReadTime({});
		}, [=] {
			Settings::ShowPremium(controller, u"revtime_hidden"_q);
		});
		controller->show(std::move(box));
	};
	const auto itemId = item->fullId();
	const auto participantChosen = [=](Ui::WhoReadParticipant who) {
		if (const auto strong = weak.get()) {
			strong->hideMenu();
		}
		ShowWhoReadInfo(controller, itemId, who);
	};
	const auto showAllChosen = [=, itemId = item->fullId()]{
		// Pressing on an item that has a submenu doesn't hide it :(
		if (const auto strong = weak.get()) {
			strong->hideMenu();
		}
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showSection(
				std::make_shared<Info::Memento>(
					whoReadIds,
					itemId,
					HistoryView::Reactions::DefaultSelectedTab(
						item,
						whoReadIds)));
		}
	};
	if (!menu->empty()) {
		menu->addSeparator(&st::expandedMenuSeparator);
	}
	if (item->history()->peer->isUser()) {
		AddWhenEditedForwardedAuthorActionHelper(
			menu,
			item,
			controller,
			false);
		menu->addAction(Ui::WhenReadContextAction(
			menu.get(),
			Api::WhoReacted(item, context, st::defaultWhoRead, whoReadIds),
			showOrPremium));
	} else {
		menu->addAction(Ui::WhoReactedContextAction(
			menu.get(),
			Api::WhoReacted(item, context, st::defaultWhoRead, whoReadIds),
			Data::ReactedMenuFactory(&controller->session()),
			participantChosen,
			showAllChosen));
		AddWhenEditedForwardedAuthorActionHelper(
			menu,
			item,
			controller,
			true);
	}
}

void MaybeAddWhenEditedForwardedAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller) {
	AddWhenEditedForwardedAuthorActionHelper(menu, item, controller, true);
}

void AddEditTagAction(
		not_null<Ui::PopupMenu*> menu,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	const auto owner = &controller->session().data();
	const auto editLabel = owner->reactions().myTagTitle(id).isEmpty()
		? tr::lng_context_tag_add_name(tr::now)
		: tr::lng_context_tag_edit_name(tr::now);
	menu->addAction(editLabel, [=] {
		controller->show(Box(EditTagBox, controller, id));
	}, &st::menuIconTagRename);
}

void AddTagPackAction(
		not_null<Ui::PopupMenu*> menu,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	if (const auto custom = id.custom()) {
		const auto owner = &controller->session().data();
		if (const auto set = owner->document(custom)->sticker()) {
			if (set->set.id) {
				AddEmojiPacksAction(
					menu,
					{ set->set },
					EmojiPacksSource::Tag,
					controller);
			}
		}
	}
}

void ShowTagMenu(
		not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
		QPoint position,
		not_null<QWidget*> context,
		not_null<HistoryItem*> item,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	using namespace Data;
	const auto itemId = item->fullId();
	const auto owner = &controller->session().data();
	*menu = base::make_unique_q<Ui::PopupMenu>(
		context,
		st::popupMenuExpandedSeparator);
	(*menu)->addAction(tr::lng_context_filter_by_tag(tr::now), [=] {
		HashtagClickHandler(SearchTagToQuery(id)).onClick({
			.button = Qt::LeftButton,
			.other = QVariant::fromValue(ClickHandlerContext{
				.sessionWindow = controller,
			}),
		});
	}, &st::menuIconTagFilter);

	AddEditTagAction(menu->get(), id, controller);

	const auto removeTag = [=] {
		if (const auto item = owner->message(itemId)) {
			const auto &list = item->reactions();
			if (ranges::contains(list, id, &MessageReaction::id)) {
				item->toggleReaction(id, HistoryReactionSource::Quick);
			}
		}
	};
	(*menu)->addAction(base::make_unique_q<Ui::Menu::Action>(
		(*menu)->menu(),
		st::menuWithIconsAttention,
		Ui::Menu::CreateAction(
			(*menu)->menu(),
			tr::lng_context_remove_tag(tr::now),
			removeTag),
		&st::menuIconTagRemoveAttention,
		&st::menuIconTagRemoveAttention));

	AddTagPackAction(menu->get(), id, controller);

	(*menu)->popup(position);
}

void ShowTagInListMenu(
		not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
		QPoint position,
		not_null<QWidget*> context,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	*menu = base::make_unique_q<Ui::PopupMenu>(
		context,
		st::popupMenuExpandedSeparator);

	AddEditTagAction(menu->get(), id, controller);
	AddTagPackAction(menu->get(), id, controller);

	(*menu)->popup(position);
}

void AddCopyFilename(
		not_null<Ui::PopupMenu*> menu,
		not_null<DocumentData*> document,
		Fn<bool()> showCopyRestrictionForSelected) {
	const auto filenameToCopy = [&] {
		if (document->isAudioFile()) {
			return TextForMimeData().append(
				Ui::Text::FormatSongNameFor(document).string());
		} else if (document->sticker()
			|| document->isAnimation()
			|| document->isVideoMessage()
			|| document->isVideoFile()
			|| document->isVoiceMessage()) {
			return TextForMimeData();
		} else {
			return TextForMimeData().append(document->filename());
		}
	}();
	if (!filenameToCopy.empty()) {
		menu->addAction(tr::lng_context_copy_filename(tr::now), [=] {
			if (!showCopyRestrictionForSelected()) {
				TextUtilities::SetClipboardText(filenameToCopy);
			}
		}, &st::menuIconCopy);
	}
}

void ShowWhoReactedMenu(
		not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
		QPoint position,
		not_null<QWidget*> context,
		not_null<HistoryItem*> item,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller,
		rpl::lifetime &lifetime) {
	if (item->reactionsAreTags()) {
		ShowTagMenu(menu, position, context, item, id, controller);
		return;
	}

	struct State {
		int addedToBottom = 0;
	};
	const auto itemId = item->fullId();
	const auto participantChosen = [=](Ui::WhoReadParticipant who) {
		ShowWhoReadInfo(controller, itemId, who);
	};
	const auto showAllChosen = [=, itemId = item->fullId()]{
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showSection(std::make_shared<Info::Memento>(
				nullptr,
				itemId,
				HistoryView::Reactions::DefaultSelectedTab(item, id)));
		}
	};
	const auto owner = &controller->session().data();
	const auto reactions = &owner->reactions();
	const auto &list = reactions->list(
		Data::Reactions::Type::Active);
	const auto activeNonQuick = !id.paid()
		&& (id != reactions->favoriteId())
		&& (ranges::contains(list, id, &Data::Reaction::id)
			|| (controller->session().premium() && id.custom()));
	const auto filler = lifetime.make_state<Ui::WhoReactedListMenu>(
		Data::ReactedMenuFactory(&controller->session()),
		participantChosen,
		showAllChosen);
	const auto state = lifetime.make_state<State>();
	Api::WhoReacted(
		item,
		id,
		context,
		st::defaultWhoRead
	) | rpl::filter([=](const Ui::WhoReadContent &content) {
		return content.state != Ui::WhoReadState::Unknown;
	}) | rpl::on_next([=, &lifetime](Ui::WhoReadContent &&content) {
		const auto creating = !*menu;
		const auto refillTop = [=] {
			if (activeNonQuick) {
				(*menu)->addAction(tr::lng_context_set_as_quick(tr::now), [=] {
					reactions->setFavorite(id);
				}, &st::menuIconFave);
				(*menu)->addSeparator();
			}
		};
		const auto appendBottom = [=] {
			state->addedToBottom = 0;
			if (const auto custom = id.custom()) {
				if (const auto set = owner->document(custom)->sticker()) {
					if (set->set.id) {
						state->addedToBottom = 2;
						AddEmojiPacksAction(
							menu->get(),
							{ set->set },
							EmojiPacksSource::Reaction,
							controller);
					}
				}
			}
		};
		if (creating) {
			*menu = base::make_unique_q<Ui::PopupMenu>(
				context,
				st::whoReadMenu);
			(*menu)->lifetime().add(base::take(lifetime));
			refillTop();
		}
		filler->populate(
			menu->get(),
			content,
			refillTop,
			state->addedToBottom,
			appendBottom);
		if (creating) {
			(*menu)->popup(position);
		}
	}, lifetime);
}

std::vector<StickerSetIdentifier> CollectEmojiPacks(
		not_null<HistoryItem*> item,
		EmojiPacksSource source) {
	auto result = std::vector<StickerSetIdentifier>();
	const auto owner = &item->history()->owner();
	const auto push = [&](DocumentId id) {
		if (const auto set = owner->document(id)->sticker()) {
			if (set->set.id
				&& !ranges::contains(
					result,
					set->set.id,
					&StickerSetIdentifier::id)) {
				result.push_back(set->set);
			}
		}
	};
	switch (source) {
	case EmojiPacksSource::Message:
		for (const auto &entity : item->originalText().entities) {
			if (entity.type() == EntityType::CustomEmoji) {
				const auto data = Data::ParseCustomEmojiData(entity.data());
				push(data);
			}
		}
		break;
	case EmojiPacksSource::Reactions:
		for (const auto &reaction : item->reactions()) {
			if (const auto customId = reaction.id.custom()) {
				push(customId);
			}
		}
		break;
	default: Unexpected("Source in CollectEmojiPacks.");
	}
	return result;
}

void AddEmojiPacksAction(
		not_null<Ui::PopupMenu*> menu,
		std::vector<StickerSetIdentifier> packIds,
		EmojiPacksSource source,
		not_null<Window::SessionController*> controller) {
	if (packIds.empty()) {
		return;
	}

	const auto count = int(packIds.size());
	const auto manager = &controller->session().data().customEmojiManager();
	const auto name = (count == 1)
		? TextWithEntities{ manager->lookupSetName(packIds[0].id) }
		: TextWithEntities();
	if (!menu->empty()) {
		menu->addSeparator();
	}
	auto text = [&] {
		switch (source) {
		case EmojiPacksSource::Message:
			return name.text.isEmpty()
				? tr::lng_context_animated_emoji_many(
					tr::now,
					lt_count,
					count,
					tr::rich)
				: tr::lng_context_animated_emoji(
					tr::now,
					lt_name,
					TextWithEntities{ name },
					tr::rich);
		case EmojiPacksSource::Tag:
			return tr::lng_context_animated_tag(
				tr::now,
				lt_name,
				TextWithEntities{ name },
				tr::rich);
		case EmojiPacksSource::Reaction:
			if (!name.text.isEmpty()) {
				return tr::lng_context_animated_reaction(
					tr::now,
					lt_name,
					TextWithEntities{ name },
					tr::rich);
			}
			[[fallthrough]];
		case EmojiPacksSource::Reactions:
			return name.text.isEmpty()
				? tr::lng_context_animated_reactions_many(
					tr::now,
					lt_count,
					count,
					tr::rich)
				: tr::lng_context_animated_reactions(
					tr::now,
					lt_name,
					TextWithEntities{ name },
					tr::rich);
		}
		Unexpected("Source in AddEmojiPacksAction.");
	}();
	auto button = base::make_unique_q<Ui::Menu::MultilineAction>(
		menu->menu(),
		menu->st().menu,
		st::historyHasCustomEmoji,
		st::historyHasCustomEmojiPosition,
		std::move(text));
	const auto weak = base::make_weak(controller);
	button->setActionTriggered([=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (packIds.size() > 1) {
			strong->show(Box<StickersBox>(strong->uiShow(), packIds));
			return;
		}
		// Single used emoji pack.
		strong->show(Box<StickerSetBox>(
			strong->uiShow(),
			packIds.front(),
			Data::StickersType::Emoji));
	});
	menu->addAction(std::move(button));
}

void AddEmojiPacksAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		EmojiPacksSource source,
		not_null<Window::SessionController*> controller) {
	AddEmojiPacksAction(
		menu,
		CollectEmojiPacks(item, source),
		source,
		controller);
}

void AddSelectRestrictionAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		bool addIcon) {
	const auto peer = item->history()->peer;
	if ((peer->allowsForwarding() && !item->forbidsForward())
		|| item->isSponsored()) {
		return;
	}
	if (addIcon && !menu->empty()) {
		menu->addSeparator();
	}
	auto button = base::make_unique_q<Ui::Menu::MultilineAction>(
		menu->menu(),
		menu->st().menu,
		st::historyHasCustomEmoji,
		addIcon
			? st::historySponsoredAboutMenuLabelPosition
			: st::historyHasCustomEmojiPosition,
		(peer->isMegagroup()
			? tr::lng_context_noforwards_info_group
			: (peer->isChannel())
			? tr::lng_context_noforwards_info_channel
			: (peer->isUser() && peer->asUser()->isBot())
			? tr::lng_context_noforwards_info_bot
			: tr::lng_context_noforwards_info_channel)(
			tr::now,
			tr::rich),
		addIcon ? &st::menuIconCopyright : nullptr);
	button->setAttribute(Qt::WA_TransparentForMouseEvents);
	menu->addAction(std::move(button));
}

TextWithEntities TransribedText(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || !document->isVoiceMessage()) {
		return {};
	}
	const auto &entry = document->session().api().transcribes().entry(item);
	if (!entry.requestId
		&& entry.shown
		&& !entry.toolong
		&& !entry.failed
		&& !entry.pending
		&& !entry.result.isEmpty()) {
		return { entry.result };
	}
	return {};
}

bool ItemHasTtl(HistoryItem *item) {
	return (item && item->media())
		? (item->media()->ttlSeconds() > 0)
		: false;
}

} // namespace HistoryView
