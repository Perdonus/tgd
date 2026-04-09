/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <vector>

namespace Menu::Customization {

namespace SideMenuItemId {
inline constexpr auto MyProfile = "my_profile";
inline constexpr auto Bots = "bots";
inline constexpr auto NewGroup = "new_group";
inline constexpr auto NewChannel = "new_channel";
inline constexpr auto Contacts = "contacts";
inline constexpr auto Calls = "calls";
inline constexpr auto SavedMessages = "saved_messages";
inline constexpr auto Settings = "settings";
inline constexpr auto Plugins = "plugins";
inline constexpr auto ShowLogs = "show_logs";
inline constexpr auto GhostMode = "ghost_mode";
inline constexpr auto NightMode = "night_mode";

inline constexpr auto AddContact = "add_contact";
inline constexpr auto FixChatsOrder = "fix_chats_order";
inline constexpr auto ReloadTemplates = "reload_templates";

inline constexpr auto SeparatorPrimary = "separator_primary";
inline constexpr auto SeparatorSystem = "separator_system";
} // namespace SideMenuItemId

struct SideMenuEntry {
	QString id;
	bool visible = true;
	bool separator = false;
};

using PeerMenuEntry = SideMenuEntry;

namespace SideMenuProfileBlockPositionId {
inline constexpr auto Top = "top";
inline constexpr auto Bottom = "bottom";
} // namespace SideMenuProfileBlockPositionId

struct SideMenuOptions {
	bool showFooterText = true;
	QString profileBlockPosition;
};

namespace PeerMenuSurfaceId {
inline constexpr auto ChatsList = "chats_list";
inline constexpr auto History = "history";
inline constexpr auto Profile = "profile";
inline constexpr auto Replies = "replies";
inline constexpr auto Scheduled = "scheduled";
inline constexpr auto Context = "context";
inline constexpr auto SubsectionTabs = "subsection_tabs";
} // namespace PeerMenuSurfaceId

namespace PeerMenuItemId {
inline constexpr auto ToggleMute = "toggle_mute";
inline constexpr auto Ttl = "ttl";
inline constexpr auto CreateTopic = "create_topic";
inline constexpr auto Info = "info";
inline constexpr auto ViewAsMessages = "view_as_messages";
inline constexpr auto ViewAsTopics = "view_as_topics";
inline constexpr auto SearchTopics = "search_topics";
inline constexpr auto ManageChat = "manage_chat";
inline constexpr auto SupportInfo = "support_info";
inline constexpr auto StoryArchive = "story_archive";
inline constexpr auto BoostChat = "boost_chat";
inline constexpr auto VideoChat = "video_chat";
inline constexpr auto CreatePoll = "create_poll";
inline constexpr auto CreateTodoList = "create_todo_list";
inline constexpr auto ThemeEdit = "theme_edit";
inline constexpr auto ViewDiscussion = "view_discussion";
inline constexpr auto DirectMessages = "direct_messages";
inline constexpr auto ExportChat = "export_chat";
inline constexpr auto Translate = "translate";
inline constexpr auto DeletedMessages = "deleted_messages";
inline constexpr auto Report = "report";
inline constexpr auto ClearHistory = "clear_history";
inline constexpr auto DeleteChat = "delete_chat";
inline constexpr auto LeaveChat = "leave_chat";
inline constexpr auto JoinChat = "join_chat";
inline constexpr auto NewContact = "new_contact";
inline constexpr auto ShareContact = "share_contact";
inline constexpr auto EditContact = "edit_contact";
inline constexpr auto BotToGroup = "bot_to_group";
inline constexpr auto NewMembers = "new_members";
inline constexpr auto DeleteContact = "delete_contact";
inline constexpr auto DeleteTopic = "delete_topic";
inline constexpr auto TopicLink = "topic_link";
inline constexpr auto ManageTopic = "manage_topic";
inline constexpr auto ToggleTopicClosed = "toggle_topic_closed";
inline constexpr auto SendGift = "send_gift";
inline constexpr auto ViewStatistics = "view_statistics";
inline constexpr auto ToggleFolder = "toggle_folder";
inline constexpr auto BlockUser = "block_user";
inline constexpr auto HidePromotion = "hide_promotion";
inline constexpr auto TogglePin = "toggle_pin";
inline constexpr auto ToggleUnreadMark = "toggle_unread_mark";
inline constexpr auto ToggleArchive = "toggle_archive";
inline constexpr auto NewWindow = "new_window";
inline constexpr auto SetPersonalChannel = "set_personal_channel";

inline constexpr auto SeparatorPrimary = "separator_primary";
inline constexpr auto SeparatorSecondary = "separator_secondary";
inline constexpr auto SeparatorDanger = "separator_danger";
} // namespace PeerMenuItemId

namespace ContextMenuItemId {
inline constexpr auto SelectionCopy = "selection.copy";
inline constexpr auto SelectionTranslate = "selection.translate";
inline constexpr auto SelectionSearch = "selection.search";
inline constexpr auto SelectionForward = "selection.forward";
inline constexpr auto SelectionForwardWithoutAuthor = "selection.forward_without_author";
inline constexpr auto SelectionForwardSaved = "selection.forward_saved";
inline constexpr auto SelectionSendNow = "selection.send_now";
inline constexpr auto SelectionDelete = "selection.delete";
inline constexpr auto SelectionDownloadFiles = "selection.download_files";
inline constexpr auto SelectionClear = "selection.clear";

inline constexpr auto MessageGoTo = "message.go_to";
inline constexpr auto MessageViewReplies = "message.view_replies";
inline constexpr auto MessageReply = "message.reply";
inline constexpr auto MessageTodoEdit = "message.todo.edit";
inline constexpr auto MessageTodoAdd = "message.todo.add";
inline constexpr auto MessageEdit = "message.edit";
inline constexpr auto MessageEditHistory = "message.edit_history";
inline constexpr auto MessageCopyIdsTime = "message.copy_ids_time";
inline constexpr auto MessageFactcheck = "message.factcheck";
inline constexpr auto MessagePin = "message.pin";
inline constexpr auto MessageCopyPostLink = "message.copy_post_link";
inline constexpr auto MessageCopyText = "message.copy_text";
inline constexpr auto MessageTranslate = "message.translate";
inline constexpr auto LinkCopy = "link.copy";
inline constexpr auto MessageForward = "message.forward";
inline constexpr auto MessageForwardWithoutAuthor = "message.forward_without_author";
inline constexpr auto MessageForwardSaved = "message.forward_saved";
inline constexpr auto MessageSendNow = "message.send_now";
inline constexpr auto MessageDelete = "message.delete";
inline constexpr auto MessageReport = "message.report";
inline constexpr auto MessageSelect = "message.select";
inline constexpr auto MessageReschedule = "message.reschedule";
} // namespace ContextMenuItemId

[[nodiscard]] QString SideMenuLayoutPath();
[[nodiscard]] std::vector<SideMenuEntry> DefaultSideMenuLayout(
	bool supportMode,
	bool includeShowLogs);
[[nodiscard]] std::vector<SideMenuEntry> LoadSideMenuLayout(
	bool supportMode,
	bool includeShowLogs,
	bool *changed = nullptr);
[[nodiscard]] bool SaveSideMenuLayout(
	const std::vector<SideMenuEntry> &entries);
[[nodiscard]] bool ResetSideMenuLayout(
	bool supportMode,
	bool includeShowLogs);
[[nodiscard]] SideMenuOptions DefaultSideMenuOptions();
[[nodiscard]] SideMenuOptions LoadSideMenuOptions(bool *changed = nullptr);
[[nodiscard]] bool SaveSideMenuOptions(const SideMenuOptions &options);
[[nodiscard]] std::vector<PeerMenuEntry> LoadPeerMenuLayout(
	const QString &surfaceId,
	const std::vector<PeerMenuEntry> &defaults,
	bool *changed = nullptr);
[[nodiscard]] bool SavePeerMenuLayout(
	const QString &surfaceId,
	const std::vector<PeerMenuEntry> &entries);
[[nodiscard]] bool ResetPeerMenuLayout(
	const QString &surfaceId,
	const std::vector<PeerMenuEntry> &defaults);

} // namespace Menu::Customization
