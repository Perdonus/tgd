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

constexpr auto kLayoutVersion = 2;
constexpr auto kSideMenuSection = "side_menu";
constexpr auto kPeerMenuSection = "peer_menu";
constexpr auto kShowFooterTextKey = "show_footer_text";
constexpr auto kProfileBlockPositionKey = "profile_block_position";

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

[[nodiscard]] QJsonObject ReadLayoutObject(bool *parsed = nullptr) {
	const auto path = SideMenuLayoutPath();
	auto file = QFile(path);
	if (!file.exists()) {
		if (parsed) {
			*parsed = true;
		}
		return {};
	} else if (!file.open(QIODevice::ReadOnly)) {
		if (parsed) {
			*parsed = false;
		}
		return {};
	}

	QJsonParseError error;
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	const auto ok = (error.error == QJsonParseError::NoError)
		&& document.isObject();
	if (parsed) {
		*parsed = ok;
	}
	return ok ? document.object() : QJsonObject();
}

[[nodiscard]] bool WriteLayoutObject(QJsonObject object) {
	const auto path = SideMenuLayoutPath();
	const auto directory = QFileInfo(path).absolutePath();
	if (!directory.isEmpty() && !QDir().mkpath(directory)) {
		return false;
	}

	object.insert(QStringLiteral("version"), kLayoutVersion);
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

[[nodiscard]] std::vector<SideMenuEntry> ParseSectionEntries(
		const QJsonObject &root,
		const QString &sectionName) {
	return ParseEntries(
		root.value(sectionName).toObject().value(QStringLiteral("entries")).toArray());
}

[[nodiscard]] std::vector<SideMenuEntry> ParsePeerEntries(
		const QJsonObject &root,
		const QString &surfaceId) {
	return ParseEntries(root.value(kPeerMenuSection).toObject().value(
		surfaceId).toObject().value(QStringLiteral("entries")).toArray());
}

[[nodiscard]] std::vector<SideMenuEntry> NormalizeEntries(
		const std::vector<SideMenuEntry> &parsed,
		const std::vector<SideMenuEntry> &defaults,
		bool *normalized = nullptr) {
	auto changed = false;
	const auto knownIds = DefaultNonSeparatorIds(defaults);
	auto seenKnown = QSet<QString>();
	auto result = std::vector<SideMenuEntry>();
	result.reserve(parsed.size() + defaults.size());

	for (const auto &entry : parsed) {
		if (entry.id.isEmpty()) {
			changed = true;
			continue;
		} else if (entry.separator) {
			result.push_back(entry);
			continue;
		} else if (knownIds.contains(entry.id)) {
			if (seenKnown.contains(entry.id)) {
				changed = true;
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
		changed = true;
	}

	if (normalized) {
		*normalized = changed;
	}
	return result;
}

[[nodiscard]] bool IsKnownProfileBlockPosition(const QString &value) {
	return (value == QString::fromLatin1(SideMenuProfileBlockPositionId::Top))
		|| (value == QString::fromLatin1(SideMenuProfileBlockPositionId::Bottom));
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
	auto parsed = std::vector<SideMenuEntry>();
	auto parsedOk = false;
	const auto root = ReadLayoutObject(&parsedOk);
	if (parsedOk) {
		parsed = ParseSectionEntries(root, kSideMenuSection);
	} else {
		normalized = true;
	}

	if (parsed.empty()) {
		normalized = true;
		parsed = defaults;
	}

	auto normalizedByEntries = false;
	const auto result = NormalizeEntries(parsed, defaults, &normalizedByEntries);
	normalized = normalized || normalizedByEntries;

	if (changed) {
		*changed = normalized;
	}
	if (normalized) {
		SaveSideMenuLayout(result);
	}
	return result;
}

bool SaveSideMenuLayout(const std::vector<SideMenuEntry> &entries) {
	auto array = QJsonArray();
	for (const auto &entry : entries) {
		if (entry.id.trimmed().isEmpty()) {
			continue;
		}
		array.push_back(EntryToJson(entry));
	}

	auto root = ReadLayoutObject();
	auto sideMenu = root.value(QString::fromLatin1(kSideMenuSection)).toObject();
	sideMenu.insert(QStringLiteral("entries"), array);
	root.insert(QString::fromLatin1(kSideMenuSection), sideMenu);
	return WriteLayoutObject(std::move(root));
}

bool ResetSideMenuLayout(bool supportMode, bool includeShowLogs) {
	return SaveSideMenuLayout(DefaultSideMenuLayout(
		supportMode,
		includeShowLogs));
}

SideMenuOptions DefaultSideMenuOptions() {
	return {
		.showFooterText = true,
		.profileBlockPosition = QString::fromLatin1(
			SideMenuProfileBlockPositionId::Top),
	};
}

SideMenuOptions LoadSideMenuOptions(bool *changed) {
	auto normalized = false;
	auto parsedOk = false;
	const auto root = ReadLayoutObject(&parsedOk);
	auto result = DefaultSideMenuOptions();
	if (!parsedOk) {
		normalized = true;
	} else {
		const auto sideMenu = root.value(QString::fromLatin1(
			kSideMenuSection)).toObject();
		if (!sideMenu.contains(QString::fromLatin1(kShowFooterTextKey))) {
			normalized = true;
		} else {
			result.showFooterText = sideMenu.value(QString::fromLatin1(
				kShowFooterTextKey)).toBool(result.showFooterText);
		}
		if (!sideMenu.contains(QString::fromLatin1(kProfileBlockPositionKey))) {
			normalized = true;
		} else {
			result.profileBlockPosition = sideMenu.value(QString::fromLatin1(
				kProfileBlockPositionKey)).toString(result.profileBlockPosition).trimmed();
			if (!IsKnownProfileBlockPosition(result.profileBlockPosition)) {
				result.profileBlockPosition = DefaultSideMenuOptions().profileBlockPosition;
				normalized = true;
			}
		}
	}

	if (changed) {
		*changed = normalized;
	}
	if (normalized) {
		SaveSideMenuOptions(result);
	}
	return result;
}

bool SaveSideMenuOptions(const SideMenuOptions &options) {
	auto root = ReadLayoutObject();
	auto sideMenu = root.value(QString::fromLatin1(kSideMenuSection)).toObject();
	sideMenu.insert(
		QString::fromLatin1(kShowFooterTextKey),
		options.showFooterText);
	sideMenu.insert(
		QString::fromLatin1(kProfileBlockPositionKey),
		IsKnownProfileBlockPosition(options.profileBlockPosition)
			? options.profileBlockPosition
			: QString::fromLatin1(SideMenuProfileBlockPositionId::Top));
	root.insert(QString::fromLatin1(kSideMenuSection), sideMenu);
	return WriteLayoutObject(std::move(root));
}

std::vector<PeerMenuEntry> LoadPeerMenuLayout(
		const QString &surfaceId,
		const std::vector<PeerMenuEntry> &defaults,
		bool *changed) {
	auto normalized = false;
	auto parsed = std::vector<PeerMenuEntry>();
	auto parsedOk = false;
	const auto root = ReadLayoutObject(&parsedOk);
	if (parsedOk) {
		parsed = ParsePeerEntries(root, surfaceId);
	} else {
		normalized = true;
	}

	if (parsed.empty()) {
		normalized = true;
		parsed = defaults;
	}

	auto normalizedByEntries = false;
	const auto result = NormalizeEntries(parsed, defaults, &normalizedByEntries);
	normalized = normalized || normalizedByEntries;

	if (changed) {
		*changed = normalized;
	}
	if (normalized) {
		SavePeerMenuLayout(surfaceId, result);
	}
	return result;
}

bool SavePeerMenuLayout(
		const QString &surfaceId,
		const std::vector<PeerMenuEntry> &entries) {
	auto array = QJsonArray();
	for (const auto &entry : entries) {
		if (entry.id.trimmed().isEmpty()) {
			continue;
		}
		array.push_back(EntryToJson(entry));
	}

	auto root = ReadLayoutObject();
	auto peerMenu = root.value(QString::fromLatin1(kPeerMenuSection)).toObject();
	peerMenu.insert(surfaceId, QJsonObject{
		{ QStringLiteral("entries"), array },
	});
	root.insert(QString::fromLatin1(kPeerMenuSection), peerMenu);
	return WriteLayoutObject(std::move(root));
}

bool ResetPeerMenuLayout(
		const QString &surfaceId,
		const std::vector<PeerMenuEntry> &defaults) {
	return SavePeerMenuLayout(surfaceId, defaults);
}

} // namespace Menu::Customization
