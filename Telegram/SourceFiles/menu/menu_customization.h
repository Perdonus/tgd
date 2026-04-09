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

} // namespace Menu::Customization
