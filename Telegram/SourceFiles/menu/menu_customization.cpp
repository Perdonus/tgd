/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_customization.h"

#include "core/launcher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

namespace Menu::Customization {
namespace {

constexpr auto kLayoutVersion = 1;

[[nodiscard]] SideMenuEntry MakeItem(const char *id, bool visible = true) {
	return {
		.id = QString::fromLatin1(id),
		.visible = visible,
		.separator = false,
	};
}

[[nodiscard]] SideMenuEntry MakeSeparator(
		const char *id,
		bool visible = true) {
	return {
		.id = QString::fromLatin1(id),
		.visible = visible,
		.separator = true,
	};
}

[[nodiscard]] QJsonObject EntryToJson(const SideMenuEntry &entry) {
	auto result = QJsonObject{
		{ QStringLiteral("id"), entry.id },
		{ QStringLiteral("visible"), entry.visible },
	};
	if (entry.separator) {
		result.insert(QStringLiteral("separator"), true);
	}
	return result;
}

[[nodiscard]] std::vector<SideMenuEntry> ParseEntries(const QJsonArray &array) {
	auto result = std::vector<SideMenuEntry>();
	result.reserve(array.size());
	for (const auto &value : array) {
		if (!value.isObject()) {
			continue;
		}
		const auto object = value.toObject();
		const auto id = object.value(QStringLiteral("id")).toString().trimmed();
		if (id.isEmpty()) {
			continue;
		}
		const auto separator = object.value(QStringLiteral("separator")).toBool(
			object.value(QStringLiteral("type")).toString()
				== QStringLiteral("separator"));
		result.push_back({
			.id = id,
			.visible = object.value(QStringLiteral("visible")).toBool(true),
			.separator = separator,
		});
	}
	return result;
}

[[nodiscard]] QSet<QString> DefaultNonSeparatorIds(
		const std::vector<SideMenuEntry> &entries) {
	auto result = QSet<QString>();
	for (const auto &entry : entries) {
		if (!entry.separator) {
			result.insert(entry.id);
		}
	}
	return result;
}

} // namespace

QString SideMenuLayoutPath() {
	return cWorkingDir() + QStringLiteral("tdata/menu_layout.json");
}

std::vector<SideMenuEntry> DefaultSideMenuLayout(
		bool supportMode,
		bool includeShowLogs) {
	auto result = std::vector<SideMenuEntry>();
	if (supportMode) {
		result.push_back(MakeItem(SideMenuItemId::AddContact));
		result.push_back(MakeItem(SideMenuItemId::FixChatsOrder));
		result.push_back(MakeItem(SideMenuItemId::ReloadTemplates));
	} else {
		result.push_back(MakeItem(SideMenuItemId::MyProfile));
		result.push_back(MakeItem(SideMenuItemId::Bots));
		result.push_back(MakeSeparator(SideMenuItemId::SeparatorPrimary));
		result.push_back(MakeItem(SideMenuItemId::NewGroup));
		result.push_back(MakeItem(SideMenuItemId::NewChannel));
		result.push_back(MakeItem(SideMenuItemId::Contacts));
		result.push_back(MakeItem(SideMenuItemId::Calls));
		result.push_back(MakeItem(SideMenuItemId::SavedMessages));
	}
	result.push_back(MakeSeparator(SideMenuItemId::SeparatorSystem));
	result.push_back(MakeItem(SideMenuItemId::Settings));
	result.push_back(MakeItem(SideMenuItemId::Plugins));
	if (includeShowLogs) {
		result.push_back(MakeItem(SideMenuItemId::ShowLogs));
	}
	result.push_back(MakeItem(SideMenuItemId::GhostMode));
	result.push_back(MakeItem(SideMenuItemId::NightMode));
	return result;
}

std::vector<SideMenuEntry> LoadSideMenuLayout(
		bool supportMode,
		bool includeShowLogs,
		bool *changed) {
	auto normalized = false;
	const auto defaults = DefaultSideMenuLayout(supportMode, includeShowLogs);
	const auto path = SideMenuLayoutPath();
	auto parsed = std::vector<SideMenuEntry>();

	auto file = QFile(path);
	if (file.exists() && file.open(QIODevice::ReadOnly)) {
		QJsonParseError error;
		const auto document = QJsonDocument::fromJson(file.readAll(), &error);
		if (error.error == QJsonParseError::NoError && document.isObject()) {
			const auto sideMenu = document.object().value(
				QStringLiteral("side_menu")).toObject();
			parsed = ParseEntries(sideMenu.value(QStringLiteral("entries")).toArray());
		} else {
			normalized = true;
		}
	} else if (file.exists()) {
		normalized = true;
	}

	if (parsed.empty()) {
		normalized = true;
		parsed = defaults;
	}

	const auto knownIds = DefaultNonSeparatorIds(defaults);
	auto seenKnown = QSet<QString>();
	auto result = std::vector<SideMenuEntry>();
	result.reserve(parsed.size() + defaults.size());

	for (const auto &entry : parsed) {
		if (entry.id.isEmpty()) {
			normalized = true;
			continue;
		} else if (entry.separator) {
			result.push_back(entry);
			continue;
		} else if (knownIds.contains(entry.id)) {
			if (seenKnown.contains(entry.id)) {
				normalized = true;
				continue;
			}
			seenKnown.insert(entry.id);
		}
		result.push_back(entry);
	}

	for (const auto &entry : defaults) {
		if (entry.separator || seenKnown.contains(entry.id)) {
			continue;
		}
		result.push_back(entry);
		seenKnown.insert(entry.id);
		normalized = true;
	}

	if (changed) {
		*changed = normalized;
	}
	if (normalized) {
		SaveSideMenuLayout(result);
	}
	return result;
}

bool SaveSideMenuLayout(const std::vector<SideMenuEntry> &entries) {
	const auto path = SideMenuLayoutPath();
	const auto directory = QFileInfo(path).absolutePath();
	if (!directory.isEmpty() && !QDir().mkpath(directory)) {
		return false;
	}

	auto array = QJsonArray();
	for (const auto &entry : entries) {
		if (entry.id.trimmed().isEmpty()) {
			continue;
		}
		array.push_back(EntryToJson(entry));
	}

	const auto document = QJsonDocument(QJsonObject{
		{ QStringLiteral("version"), kLayoutVersion },
		{ QStringLiteral("side_menu"), QJsonObject{
			{ QStringLiteral("entries"), array },
		} },
	});

	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

bool ResetSideMenuLayout(bool supportMode, bool includeShowLogs) {
	return SaveSideMenuLayout(DefaultSideMenuLayout(
		supportMode,
		includeShowLogs));
}

} // namespace Menu::Customization
