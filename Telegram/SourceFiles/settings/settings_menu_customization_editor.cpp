/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_menu_customization_editor.h"

#include "settings/settings_common.h"
#include "history/view/history_view_context_menu.h"
#include "base/flat_set.h"
#include "menu/menu_customization.h"
#include "core/application.h"
#include "core/launcher.h"
#include "main/main_session.h"
#include "plugins/plugins_manager.h"
#include "ui/painter.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "lang/lang_instance.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QClipboard>
#include <QtGui/QFontMetrics>
#include <QtGui/QGradient>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <memory>
#include <vector>

namespace Settings {
QString ShellModePreferencesPath() {
	return cWorkingDir() + QStringLiteral("tdata/menu_editor_preview.json");
}

ShellModePreferences LoadShellModePreferences() {
	auto result = ShellModePreferences();
	auto file = QFile(ShellModePreferencesPath());
	if (!file.open(QIODevice::ReadOnly)) {
		result.immersiveAnimation = false;
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		result.immersiveAnimation = false;
		return result;
	}
	const auto object = document.object();
	result.expandedSidePanel = object.value(
		QStringLiteral("expanded_side_panel")).toBool(result.expandedSidePanel);
	result.leftEdgeSettings = object.value(
		QStringLiteral("left_edge_settings")).toBool(result.leftEdgeSettings);
	result.wideSettingsPane = object.value(
		QStringLiteral("wide_settings_pane")).toBool(result.wideSettingsPane);
	result.immersiveAnimation = false;
	return result;
}

bool SaveShellModePreferences(const ShellModePreferences &prefs) {
	const auto path = ShellModePreferencesPath();
	const auto directory = QFileInfo(path).absolutePath();
	if (!directory.isEmpty() && !QDir().mkpath(directory)) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	const auto document = QJsonDocument(QJsonObject{
		{ QStringLiteral("immersive_animation"), false },
		{ QStringLiteral("expanded_side_panel"), prefs.expandedSidePanel },
		{ QStringLiteral("left_edge_settings"), prefs.leftEdgeSettings },
		{ QStringLiteral("wide_settings_pane"), prefs.wideSettingsPane },
	});
	if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

namespace {

constexpr auto kPreviewHeight = 292;
constexpr auto kFuturePreviewHeight = 214;
constexpr auto kContextPreviewHeight = 322;
constexpr auto kCustomizationStatusDeckHeight = 186;
constexpr auto kRowHeight = 64;
constexpr auto kRowGap = 10;
constexpr auto kRowPadding = 14;
constexpr auto kRowRadius = 18;
constexpr auto kPreviewRadius = 24;
constexpr auto kContextStripPreviewLimit = 4;

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QColor ThemedRowFill(
		bool hovered,
		bool selected,
		bool floating) {
	if (floating) {
		return anim::with_alpha(st::windowBgActive->c, 0.18);
	} else if (selected) {
		return anim::with_alpha(st::windowBgActive->c, 0.12);
	} else if (hovered) {
		return anim::with_alpha(st::windowSubTextFg->c, 0.08);
	}
	return anim::with_alpha(st::windowSubTextFg->c, 0.04);
}

[[nodiscard]] QColor ThemedRowBorder(bool hovered, bool selected) {
	return anim::with_alpha(
		selected ? st::windowBgActive->c : st::windowSubTextFg->c,
		selected ? 0.28 : (hovered ? 0.16 : 0.10));
}

[[nodiscard]] QColor ThemedButtonFill(bool hovered, bool enabled) {
	if (!enabled) {
		return anim::with_alpha(st::windowSubTextFg->c, 0.08);
	}
	return anim::with_alpha(st::windowBgActive->c, hovered ? 0.20 : 0.12);
}

[[nodiscard]] QColor ThemedButtonText(bool enabled) {
	return enabled
		? st::windowFgActive->c
		: anim::with_alpha(st::windowSubTextFg->c, 0.65);
}

[[nodiscard]] QColor ThemedDestructiveButtonFill(bool hovered, bool enabled) {
	if (!enabled) {
		return anim::with_alpha(st::windowSubTextFg->c, 0.08);
	}
	return anim::with_alpha(st::boxTextFgError->c, hovered ? 0.20 : 0.12);
}

[[nodiscard]] QColor ThemedDestructiveButtonText(bool enabled) {
	return enabled
		? st::boxTextFgError->c
		: anim::with_alpha(st::windowSubTextFg->c, 0.65);
}

[[nodiscard]] QColor BlendThemeColor(
		const QColor &left,
		const QColor &right,
		float64 ratio) {
	ratio = std::clamp(ratio, 0., 1.);
	const auto mix = [=](int a, int b) {
		return int(a + ((b - a) * ratio));
	};
	return QColor(
		mix(left.red(), right.red()),
		mix(left.green(), right.green()),
		mix(left.blue(), right.blue()),
		mix(left.alpha(), right.alpha()));
}

[[nodiscard]] QColor ThemedDescriptorAccent(
		const QColor &seed,
		bool emphasized = false) {
	const auto anchored = BlendThemeColor(st::windowBgActive->c, seed, 0.22);
	return emphasized
		? BlendThemeColor(anchored, st::windowFgActive->c, 0.12)
		: BlendThemeColor(anchored, st::windowSubTextFg->c, 0.08);
}

[[nodiscard]] bool IsCustomSeparatorId(const QString &id) {
	return id.startsWith(u"custom_separator_"_q);
}

[[nodiscard]] QString NewCustomSeparatorId() {
	return QStringLiteral("custom_separator_%1").arg(
		QString::number(QDateTime::currentMSecsSinceEpoch()));
}

[[nodiscard]] bool IsBottomProfileBlockPosition(const QString &value) {
	return value == QString::fromLatin1(
		Menu::Customization::SideMenuProfileBlockPositionId::Bottom);
}

[[nodiscard]] QString ProfileBlockPositionText(const QString &value) {
	return IsBottomProfileBlockPosition(value)
		? RuEn("Профиль внизу", "Profile at the bottom")
		: RuEn("Профиль сверху", "Profile at the top");
}

[[nodiscard]] const style::icon *SideMenuIcon(const QString &id) {
	namespace Id = Menu::Customization::SideMenuItemId;
	if (id == QString::fromLatin1(Id::MyProfile)) {
		return &st::menuIconProfile;
	} else if (id == QString::fromLatin1(Id::Bots)) {
		return &st::menuIconBot;
	} else if (id == QString::fromLatin1(Id::NewGroup)) {
		return &st::menuIconGroups;
	} else if (id == QString::fromLatin1(Id::NewChannel)) {
		return &st::menuIconChannel;
	} else if (id == QString::fromLatin1(Id::Contacts)) {
		return &st::menuIconUserShow;
	} else if (id == QString::fromLatin1(Id::Calls)) {
		return &st::menuIconPhone;
	} else if (id == QString::fromLatin1(Id::SavedMessages)) {
		return &st::menuIconSavedMessages;
	} else if (id == QString::fromLatin1(Id::Settings)) {
		return &st::menuIconSettings;
	} else if (id == QString::fromLatin1(Id::Plugins)) {
		return &st::menuIconCustomize;
	} else if (id == QString::fromLatin1(Id::ShowLogs)) {
		return &st::menuIconFaq;
	} else if (id == QString::fromLatin1(Id::GhostMode)) {
		return &st::menuIconLock;
	} else if (id == QString::fromLatin1(Id::NightMode)) {
		return &st::menuIconNightMode;
	} else if (id == QString::fromLatin1(Id::AddContact)) {
		return &st::menuIconProfile;
	} else if (id == QString::fromLatin1(Id::FixChatsOrder)) {
		return &st::menuIconPin;
	} else if (id == QString::fromLatin1(Id::ReloadTemplates)) {
		return &st::menuIconRestore;
	}
	return nullptr;
}

struct EntryDescriptor {
	QString title;
	QString subtitle;
	QString glyph;
	const style::icon *icon = nullptr;
	QColor color;
};


struct SideMenuLayoutStats {
	int visibleActions = 0;
	int hiddenActions = 0;
	int visibleSeparators = 0;
	int customSeparators = 0;
	bool pluginsVisible = false;
	bool showLogsVisible = false;
	bool ghostModeVisible = false;
};

struct ContextLayoutStats {
	int visibleMenu = 0;
	int hiddenMenu = 0;
	int visibleStrip = 0;
	int hiddenStrip = 0;
	bool forwardWithoutAuthorVisible = false;
};

[[nodiscard]] bool HasVisibleSideMenuEntry(
		const std::vector<Menu::Customization::SideMenuEntry> &entries,
		const QString &id) {
	for (const auto &entry : entries) {
		if (entry.visible && !entry.separator && (entry.id == id)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool IsForwardWithoutAuthorAction(const QString &id) {
	namespace Id = Menu::Customization::ContextMenuItemId;
	return (id == QString::fromLatin1(Id::SelectionForwardWithoutAuthor))
		|| (id == QString::fromLatin1(Id::MessageForwardWithoutAuthor));
}

[[nodiscard]] SideMenuLayoutStats ComputeSideMenuLayoutStats(
		const std::vector<Menu::Customization::SideMenuEntry> &entries) {
	auto result = SideMenuLayoutStats();
	for (const auto &entry : entries) {
		if (entry.separator) {
			if (entry.visible) {
				++result.visibleSeparators;
				if (IsCustomSeparatorId(entry.id)) {
					++result.customSeparators;
				}
			}
			continue;
		}
		if (entry.visible) {
			++result.visibleActions;
		} else {
			++result.hiddenActions;
		}
	}
	result.pluginsVisible = HasVisibleSideMenuEntry(
		entries,
		QString::fromLatin1(Menu::Customization::SideMenuItemId::Plugins));
	result.showLogsVisible = HasVisibleSideMenuEntry(
		entries,
		QString::fromLatin1(Menu::Customization::SideMenuItemId::ShowLogs));
	result.ghostModeVisible = HasVisibleSideMenuEntry(
		entries,
		QString::fromLatin1(Menu::Customization::SideMenuItemId::GhostMode));
	return result;
}

[[nodiscard]] ContextLayoutStats ComputeContextLayoutStats(
		const std::vector<HistoryView::ContextMenuLayoutEntry> &menu,
		const std::vector<HistoryView::ContextMenuLayoutEntry> &strip) {
	auto result = ContextLayoutStats();
	for (const auto &entry : menu) {
		if (entry.visible) {
			++result.visibleMenu;
		} else {
			++result.hiddenMenu;
		}
		if (entry.visible && IsForwardWithoutAuthorAction(entry.id)) {
			result.forwardWithoutAuthorVisible = true;
		}
	}
	for (const auto &entry : strip) {
		if (entry.visible) {
			++result.visibleStrip;
		} else {
			++result.hiddenStrip;
		}
		if (entry.visible && IsForwardWithoutAuthorAction(entry.id)) {
			result.forwardWithoutAuthorVisible = true;
		}
	}
	return result;
}

[[nodiscard]] ContextLayoutStats ComputeContextLayoutStats(
		const HistoryView::ContextMenuCustomizationLayout &layout,
		HistoryView::ContextMenuSurface surface) {
	const auto &surfaceLayout = HistoryView::LookupContextMenuSurfaceLayout(
		layout,
		surface);
	return ComputeContextLayoutStats(surfaceLayout.menu, surfaceLayout.strip);
}


[[nodiscard]] EntryDescriptor DescribeEntry(
		const Menu::Customization::SideMenuEntry &entry,
		bool supportMode) {
	namespace Id = Menu::Customization::SideMenuItemId;
	if (entry.separator) {
		if (entry.id == QString::fromLatin1(Id::SeparatorPrimary)) {
			return {
				.title = RuEn("Основной разделитель", "Primary divider"),
				.subtitle = RuEn(
					"Отделяет верхний блок меню от быстрых действий.",
					"Separates the top profile block from quick actions."),
				.glyph = u"="_q,
				.icon = nullptr,
				.color = QColor(0x76, 0x86, 0x9B),
			};
		} else if (entry.id == QString::fromLatin1(Id::SeparatorSystem)) {
			return {
				.title = RuEn("Системный разделитель", "System divider"),
				.subtitle = RuEn(
					"Отделяет системные и Astrogram-пункты.",
					"Separates system and Astrogram actions."),
				.glyph = u"="_q,
				.icon = nullptr,
				.color = QColor(0x4D, 0xB7, 0x88),
			};
		}
		return {
			.title = RuEn("Пользовательский разделитель", "Custom divider"),
			.subtitle = RuEn(
				"Свободный разделитель, который можно переставлять или удалить.",
				"Free divider that can be moved or removed."),
			.glyph = u"="_q,
			.icon = nullptr,
			.color = QColor(0x35, 0xC3, 0x8F),
		};
	}

	const auto make = [&](const char *ruTitle,
			const char *enTitle,
			const char *ruSubtitle,
			const char *enSubtitle,
			const char *glyph,
			QColor color) {
		return EntryDescriptor{
			.title = RuEn(ruTitle, enTitle),
			.subtitle = RuEn(ruSubtitle, enSubtitle),
			.glyph = QString::fromLatin1(glyph),
			.icon = SideMenuIcon(entry.id),
			.color = color,
		};
	};

	if (entry.id == QString::fromLatin1(Id::MyProfile)) {
		return make(
			"Мой профиль",
			"My profile",
			"Переход в профиль и stories.",
			"Shortcut to profile and stories.",
			"P",
			QColor(0x4F, 0x8D, 0xFF));
	} else if (entry.id == QString::fromLatin1(Id::Bots)) {
		return make(
			"Боты",
			"Bots",
			"Быстрый вход в список ботов.",
			"Quick access to the bots list.",
			"B",
			QColor(0x68, 0x79, 0xFF));
	} else if (entry.id == QString::fromLatin1(Id::NewGroup)) {
		return make(
			"Новая группа",
			"New group",
			"Создание новой группы.",
			"Creates a new group.",
			"G",
			QColor(0x4A, 0xC6, 0x7A));
	} else if (entry.id == QString::fromLatin1(Id::NewChannel)) {
		return make(
			"Новый канал",
			"New channel",
			"Создание нового канала.",
			"Creates a new channel.",
			"C",
			QColor(0x31, 0xB0, 0xE7));
	} else if (entry.id == QString::fromLatin1(Id::Contacts)) {
		return make(
			"Контакты",
			"Contacts",
			"Список контактов и быстрый поиск.",
			"Contacts list and quick search.",
			"C",
			QColor(0x2D, 0xC8, 0xB3));
	} else if (entry.id == QString::fromLatin1(Id::Calls)) {
		return make(
			"Звонки",
			"Calls",
			"История звонков и старт новых вызовов.",
			"Calls history and quick call start.",
			"C",
			QColor(0x58, 0xC8, 0x66));
	} else if (entry.id == QString::fromLatin1(Id::SavedMessages)) {
		return make(
			"Избранное",
			"Saved messages",
			"Быстрый переход в Saved Messages.",
			"Shortcut to Saved Messages.",
			"S",
			QColor(0x37, 0xA6, 0xF0));
	} else if (entry.id == QString::fromLatin1(Id::Settings)) {
		return make(
			"Настройки",
			"Settings",
			"Верхний системный вход в настройки клиента.",
			"Primary system entry to client settings.",
			"S",
			QColor(0x8A, 0x93, 0xA3));
	} else if (entry.id == QString::fromLatin1(Id::Plugins)) {
		return make(
			"Плагины",
			"Plugins",
			"Выводит вход в менеджер плагинов прямо в боковое меню.",
			"Shows a direct entry to the plugins manager.",
			"P",
			QColor(0x35, 0xC3, 0x8F));
	} else if (entry.id == QString::fromLatin1(Id::ShowLogs)) {
		return make(
			"Показать логи",
			"Show logs",
			"Показывается только если лог-экшен доступен через плагины.",
			"Visible only when a logs action is available from plugins.",
			"L",
			QColor(0xF1, 0xA4, 0x2B));
	} else if (entry.id == QString::fromLatin1(Id::GhostMode)) {
		return make(
			"Режим призрака",
			"Ghost mode",
			"Тумблер Astrogram-приватности прямо из меню.",
			"Astrogram privacy toggle directly from the menu.",
			"G",
			QColor(0x54, 0xCC, 0x96));
	} else if (entry.id == QString::fromLatin1(Id::NightMode)) {
		return make(
			"Ночная тема",
			"Night mode",
			"Системный переключатель темы.",
			"Built-in theme switch.",
			"N",
			QColor(0x73, 0x62, 0xE8));
	} else if (entry.id == QString::fromLatin1(Id::AddContact)) {
		return make(
			"Добавить контакт",
			"Add contact",
			"Support-mode action для быстрого создания контакта.",
			"Support-mode action for creating a contact.",
			"A",
			QColor(0x39, 0xB9, 0x8D));
	} else if (entry.id == QString::fromLatin1(Id::FixChatsOrder)) {
		return make(
			"Закрепить порядок чатов",
			"Fix chats order",
			"Support-mode тумблер порядка чатов.",
			"Support-mode chats order toggle.",
			"F",
			QColor(0xF0, 0x9A, 0x36));
	} else if (entry.id == QString::fromLatin1(Id::ReloadTemplates)) {
		return make(
			"Перезагрузить шаблоны",
			"Reload templates",
			"Support-mode перезагрузка шаблонов.",
			"Support-mode templates reload.",
			"R",
			QColor(0x5A, 0xAE, 0xF5));
	}
	return make(
		supportMode ? "Неизвестный support-пункт" : "Неизвестный пункт",
		supportMode ? "Unknown support item" : "Unknown item",
		"Сохранён в layout-файле и будет оставлен как есть.",
		"Persisted in the layout file and kept as-is.",
		"?",
		QColor(0x99, 0xA1, 0xAD));
}

class SideMenuEditorState final {
public:
	SideMenuEditorState(bool supportMode, bool includeShowLogs)
	: _supportMode(supportMode)
	, _includeShowLogs(includeShowLogs)
	, _entries(Menu::Customization::LoadSideMenuLayout(
		supportMode,
		includeShowLogs))
	, _options(Menu::Customization::LoadSideMenuOptions())
	, _preview(LoadShellModePreferences()) {
		normalizeSelectedEntry();
	}

	[[nodiscard]] const std::vector<Menu::Customization::SideMenuEntry> &entries()
	const {
		return _entries;
	}

	[[nodiscard]] int selectedEntryIndex() const {
		if (_selectedEntryId.isEmpty()) {
			return -1;
		}
		for (auto i = 0; i != int(_entries.size()); ++i) {
			if (_entries[i].id == _selectedEntryId) {
				return i;
			}
		}
		return -1;
	}

	void setSelectedEntryIndex(int index) {
		const auto updated = hasIndex(index) ? _entries[index].id : QString();
		if (_selectedEntryId == updated) {
			return;
		}
		_selectedEntryId = updated;
		_changes.fire({});
	}

	[[nodiscard]] bool immersiveAnimation() const {
		return _preview.immersiveAnimation;
	}

	[[nodiscard]] bool wideSettingsPane() const {
		return _preview.wideSettingsPane;
	}

	[[nodiscard]] bool expandedSidePanel() const {
		return _preview.expandedSidePanel;
	}

	[[nodiscard]] bool leftEdgeSettings() const {
		return _preview.leftEdgeSettings;
	}

	[[nodiscard]] bool showFooterText() const {
		return _options.showFooterText;
	}

	[[nodiscard]] QString profileBlockPosition() const {
		return _options.profileBlockPosition;
	}

	[[nodiscard]] bool profileAtBottom() const {
		return IsBottomProfileBlockPosition(_options.profileBlockPosition);
	}

	[[nodiscard]] QString layoutPath() const {
		return Menu::Customization::SideMenuLayoutPath();
	}

	[[nodiscard]] QString previewPrefsPath() const {
		return ShellModePreferencesPath();
	}

	[[nodiscard]] bool supportMode() const {
		return _supportMode;
	}

	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

	[[nodiscard]] bool setImmersiveAnimation(bool value) {
		if (_preview.immersiveAnimation == value) {
			return true;
		}
		auto updated = _preview;
		updated.immersiveAnimation = value;
		if (!SaveShellModePreferences(updated)) {
			return false;
		}
		_preview = updated;
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool setWideSettingsPane(bool value) {
		if (_preview.wideSettingsPane == value) {
			return true;
		}
		auto updated = _preview;
		updated.wideSettingsPane = value;
		if (!SaveShellModePreferences(updated)) {
			return false;
		}
		_preview = updated;
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool setExpandedSidePanel(bool value) {
		if (_preview.expandedSidePanel == value) {
			return true;
		}
		auto updated = _preview;
		updated.expandedSidePanel = value;
		if (!SaveShellModePreferences(updated)) {
			return false;
		}
		_preview = updated;
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool setLeftEdgeSettings(bool value) {
		if (_preview.leftEdgeSettings == value) {
			return true;
		}
		auto updated = _preview;
		updated.leftEdgeSettings = value;
		if (!SaveShellModePreferences(updated)) {
			return false;
		}
		_preview = updated;
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool setShowFooterText(bool value) {
		if (_options.showFooterText == value) {
			return true;
		}
		auto updated = _options;
		updated.showFooterText = value;
		return applyOptions(updated);
	}

	[[nodiscard]] bool setProfileBlockPosition(const QString &value) {
		const auto normalized = IsBottomProfileBlockPosition(value)
			? QString::fromLatin1(
				Menu::Customization::SideMenuProfileBlockPositionId::Bottom)
			: QString::fromLatin1(
				Menu::Customization::SideMenuProfileBlockPositionId::Top);
		if (_options.profileBlockPosition == normalized) {
			return true;
		}
		auto updated = _options;
		updated.profileBlockPosition = normalized;
		return applyOptions(updated);
	}

	[[nodiscard]] bool reloadFromDisk() {
		_entries = Menu::Customization::LoadSideMenuLayout(
			_supportMode,
			_includeShowLogs);
		_options = Menu::Customization::LoadSideMenuOptions();
		_preview = LoadShellModePreferences();
		normalizeSelectedEntry();
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool resetToDefaults() {
		const auto updated = Menu::Customization::DefaultSideMenuLayout(
			_supportMode,
			_includeShowLogs);
		return applyEntries(updated);
	}

	[[nodiscard]] bool toggleVisible(int index) {
		if (!hasIndex(index)) {
			return false;
		}
		auto updated = _entries;
		updated[index].visible = !updated[index].visible;
		return applyEntries(updated);
	}

	[[nodiscard]] bool restoreEntry(int index) {
		if (!hasIndex(index)) {
			return false;
		} else if (_entries[index].visible) {
			setSelectedEntryIndex(index);
			return true;
		}
		auto updated = _entries;
		updated[index].visible = true;
		_selectedEntryId = updated[index].id;
		return applyEntries(updated);
	}

	[[nodiscard]] bool moveUp(int index) {
		return hasIndex(index) ? moveEntry(index, index - 1) : false;
	}

	[[nodiscard]] bool moveDown(int index) {
		return hasIndex(index) ? moveEntry(index, index + 1) : false;
	}

	[[nodiscard]] bool moveEntry(int from, int to) {
		if (!hasIndex(from)) {
			return false;
		}
		const auto maxTarget = std::max(int(_entries.size()) - 1, 0);
		const auto insertAt = std::clamp(to, 0, maxTarget);
		if (from == insertAt) {
			return true;
		}
		auto updated = _entries;
		const auto moved = updated[from];
		updated.erase(updated.begin() + from);
		updated.insert(updated.begin() + insertAt, moved);
		return applyEntries(updated);
	}

	[[nodiscard]] bool moveVisibleEntry(int fromVisible, int toVisible) {
		auto visibleIndexes = std::vector<int>();
		auto visibleEntries = std::vector<Menu::Customization::SideMenuEntry>();
		for (auto i = 0; i != int(_entries.size()); ++i) {
			if (!_entries[i].visible) {
				continue;
			}
			visibleIndexes.push_back(i);
			visibleEntries.push_back(_entries[i]);
		}
		if ((fromVisible < 0) || (fromVisible >= int(visibleEntries.size()))) {
			return false;
		}
		const auto maxTarget = std::max(int(visibleEntries.size()) - 1, 0);
		const auto insertAt = std::clamp(toVisible, 0, maxTarget);
		if (fromVisible == insertAt) {
			return true;
		}
		auto reordered = visibleEntries;
		const auto moved = reordered[fromVisible];
		reordered.erase(reordered.begin() + fromVisible);
		reordered.insert(reordered.begin() + insertAt, moved);
		auto updated = _entries;
		for (auto i = 0; i != int(visibleIndexes.size()); ++i) {
			updated[visibleIndexes[i]] = reordered[i];
		}
		return applyEntries(updated);
	}

	[[nodiscard]] int addCustomSeparatorAfter(int index) {
		auto updated = _entries;
		const auto insertAt = std::clamp(index + 1, 0, int(updated.size()));
		updated.insert(
			updated.begin() + insertAt,
			Menu::Customization::SideMenuEntry{
				NewCustomSeparatorId(),
				true,
				true,
			});
		_selectedEntryId = updated[insertAt].id;
		return applyEntries(updated) ? insertAt : -1;
	}

	[[nodiscard]] bool removeCustomSeparator(int index) {
		if (!hasIndex(index) || !isCustomSeparator(_entries[index])) {
			return false;
		}
		auto updated = _entries;
		updated.erase(updated.begin() + index);
		return applyEntries(updated);
	}

	[[nodiscard]] bool restoreAllHidden() {
		auto updated = _entries;
		auto changed = false;
		for (auto &entry : updated) {
			if (!entry.visible) {
				entry.visible = true;
				changed = true;
			}
		}
		return changed ? applyEntries(updated) : true;
	}

	[[nodiscard]] bool resetOptionsToDefaults() {
		return applyOptions(Menu::Customization::DefaultSideMenuOptions());
	}

private:
	[[nodiscard]] bool applyEntries(
			const std::vector<Menu::Customization::SideMenuEntry> &updated) {
		if (!Menu::Customization::SaveSideMenuLayout(updated)) {
			return false;
		}
		_entries = updated;
		normalizeSelectedEntry();
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool applyOptions(
			const Menu::Customization::SideMenuOptions &updated) {
		if (!Menu::Customization::SaveSideMenuOptions(updated)) {
			return false;
		}
		_options = updated;
		_changes.fire({});
		return true;
	}

	void normalizeSelectedEntry() {
		if (selectedEntryIndex() >= 0) {
			return;
		}
		for (const auto &entry : _entries) {
			if (!entry.visible) {
				continue;
			}
			_selectedEntryId = entry.id;
			return;
		}
		_selectedEntryId = _entries.empty() ? QString() : _entries.front().id;
	}

	[[nodiscard]] bool hasIndex(int index) const {
		return (index >= 0) && (index < int(_entries.size()));
	}

	[[nodiscard]] static bool isCustomSeparator(
			const Menu::Customization::SideMenuEntry &entry) {
		return entry.separator && IsCustomSeparatorId(entry.id);
	}

	const bool _supportMode = false;
	const bool _includeShowLogs = false;
	std::vector<Menu::Customization::SideMenuEntry> _entries;
	Menu::Customization::SideMenuOptions _options;
	ShellModePreferences _preview;
	QString _selectedEntryId;
	mutable rpl::event_stream<> _changes;
};

class SideMenuPreview final : public Ui::RpWidget {
public:
	SideMenuPreview(
		QWidget *parent,
		std::shared_ptr<SideMenuEditorState> state)
	: RpWidget(parent)
	, _state(std::move(state)) {
		update();
		_state->changes() | rpl::on_next([=] {
			update();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return (newWidth > 760) ? (kPreviewHeight + 18) : kPreviewHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);

		const auto outer = rect().adjusted(0, 0, -1, -1);
		auto outerPath = QPainterPath();
		outerPath.addRoundedRect(outer, kPreviewRadius, kPreviewRadius);

		auto background = QLinearGradient(
			QPointF(0., 0.),
			QPointF(width(), height()));
		background.setColorAt(0., QColor(0xE9, 0xF8, 0xF2));
		background.setColorAt(0.55, QColor(0xF3, 0xFB, 0xF7));
		background.setColorAt(1., QColor(0xEC, 0xF3, 0xFA));
		p.fillPath(outerPath, background);
		p.setClipPath(outerPath);

		for (auto i = 0; i != 6; ++i) {
			const auto alpha = 0.06 + (i * 0.01);
			p.setBrush(QColor(0x1D, 0xA9, 0x68, int(alpha * 255)));
			p.setPen(Qt::NoPen);
			const auto size = 90 + (i * 26);
			p.drawEllipse(
				QPointF(width() - 48 - (i * 28), 36 + (i * 22)),
				size,
				size);
		}

		const auto immersive = _state->immersiveAnimation();
		const auto expanded = _state->expandedSidePanel();
		const auto leftEdgeSettings = _state->leftEdgeSettings();
		const auto wideSettings = _state->wideSettingsPane();
		const auto showFooterText = _state->showFooterText();
		const auto profileAtBottom = _state->profileAtBottom();
		const auto stats = ComputeSideMenuLayoutStats(_state->entries());
		const auto menuWidth = expanded
			? std::clamp(width() * 48 / 100, 270, 360)
			: std::clamp(width() * 42 / 100, 230, 320);
		const auto widePaneWidth = wideSettings
			? std::clamp(width() * 26 / 100, 170, 250)
			: 0;
		const auto chatShift = immersive ? std::min(menuWidth / 6, 44) : 0;
		const auto chatLeft = menuWidth - (immersive ? 28 : 0) + chatShift;
		const auto chatRect = QRect(
			chatLeft,
			0,
			width() - chatLeft,
			height());

		auto chatGradient = QLinearGradient(
			QPointF(chatRect.left(), 0.),
			QPointF(chatRect.right(), chatRect.bottom()));
		chatGradient.setColorAt(0., QColor(0xD9, 0xEE, 0xE5));
		chatGradient.setColorAt(1., QColor(0xF6, 0xFA, 0xFD));
		p.fillRect(chatRect, chatGradient);

		auto bubble = [&](QRect rect, QColor color) {
			p.setBrush(color);
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(rect, 16, 16);
		};
		bubble(QRect(chatRect.left() + 38, 34, 154, 42), QColor(255, 255, 255, 224));
		bubble(QRect(chatRect.right() - 200, 96, 164, 48), QColor(0xD3, 0xF1, 0xE0));
		bubble(QRect(chatRect.left() + 68, 168, 210, 54), QColor(255, 255, 255, 224));
		bubble(QRect(chatRect.right() - 232, 244, 192, 50), QColor(0xD3, 0xF1, 0xE0));

		if (wideSettings || leftEdgeSettings) {
			const auto paneWidth = wideSettings
				? widePaneWidth
				: std::clamp(width() * 20 / 100, 144, 196);
			const auto paneRect = leftEdgeSettings
				? QRect(
					menuWidth - 14,
					24,
					paneWidth,
					height() - 48)
				: QRect(
					width() - paneWidth - 20,
					24,
					paneWidth,
					height() - 48);
			p.setBrush(QColor(255, 255, 255, 224));
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(paneRect, 20, 20);
			p.setPen(QColor(0x22, 0x2D, 0x3A));
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				paneRect.adjusted(18, 16, -18, -16),
				Qt::AlignLeft | Qt::AlignTop,
				leftEdgeSettings
					? RuEn(
						"Левоторцевые настройки\npreview-only",
						"Left-edge settings\npreview-only")
					: RuEn(
						"Широкая панель настроек\npreview-only",
						"Wide settings pane\npreview-only"));
			for (auto i = 0; i != 3; ++i) {
				const auto top = paneRect.top() + 68 + (i * 48);
				p.setBrush(QColor(0xEC, 0xF5, 0xEF));
				p.drawRoundedRect(
					QRect(
						paneRect.left() + 16,
						top,
						paneRect.width() - 32,
						32),
					12,
					12);
			}
		}

		const auto menuRect = QRect(0, 0, menuWidth, height());
		auto menuGradient = QLinearGradient(
			QPointF(menuRect.left(), 0.),
			QPointF(menuRect.right(), menuRect.bottom()));
		menuGradient.setColorAt(0., QColor(0x13, 0x2B, 0x3F, 238));
		menuGradient.setColorAt(1., QColor(0x18, 0x37, 0x4F, 230));
		p.setBrush(menuGradient);
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(
			menuRect.adjusted(0, 0, immersive ? -6 : 0, 0),
			kPreviewRadius,
			kPreviewRadius);

		const auto footerHeight = showFooterText ? 38 : 0;
		const auto profileRect = profileAtBottom
			? QRect(18, height() - footerHeight - 82, menuWidth - 36, 68)
			: QRect(18, 18, menuWidth - 36, 68);
		auto drawProfileCard = [&](const QRect &card) {
			p.setBrush(QColor(255, 255, 255, 20));
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(card, 18, 18);
			p.setBrush(QColor(0x35, 0xC3, 0x8F));
			p.drawEllipse(QRect(card.left() + 12, card.top() + 10, 42, 42));
			p.setPen(Qt::white);
			p.setFont(st::semiboldFont->f);
			p.drawText(
				QRect(card.left() + 12, card.top() + 10, 42, 42),
				Qt::AlignCenter,
				u"A"_q);
			p.setPen(QColor(255, 255, 255, 230));
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(card.left() + 70, card.top() + 25, RuEn("Astrogram", "Astrogram"));
			p.setPen(QColor(255, 255, 255, 150));
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(card.left() + 70, card.top() + 34, card.width() - 82, 22),
				Qt::AlignLeft | Qt::AlignVCenter,
				ProfileBlockPositionText(_state->profileBlockPosition()));
		};
		drawProfileCard(profileRect);

		auto hiddenCount = 0;
		const auto selectedIndex = _state->selectedEntryIndex();
		auto selectedMeta = EntryDescriptor();
		auto selectedHidden = false;
		for (auto actualIndex = 0; actualIndex != int(_state->entries().size()); ++actualIndex) {
			const auto &entry = _state->entries()[actualIndex];
			if (!entry.visible) {
				++hiddenCount;
			}
			if (actualIndex == selectedIndex) {
				selectedMeta = DescribeEntry(entry, _state->supportMode());
				selectedHidden = !entry.visible;
			}
		}

		if (selectedIndex >= 0) {
			const auto focusWidth = std::min(chatRect.width() - 32, 238);
			const auto focusRect = QRect(
				chatRect.left() + 16,
				height() - 84,
				focusWidth,
				56);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 255, 255, 228));
			p.drawRoundedRect(focusRect, 18, 18);
			p.setBrush(selectedMeta.color);
			p.drawEllipse(QRect(focusRect.left() + 12, focusRect.top() + 12, 28, 28));
			p.setPen(Qt::white);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(focusRect.left() + 12, focusRect.top() + 12, 28, 28),
				Qt::AlignCenter,
				selectedMeta.glyph.left(1));
			p.setPen(QColor(0x23, 0x2F, 0x3C));
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				QRect(focusRect.left() + 50, focusRect.top() + 10, focusRect.width() - 62, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				selectedMeta.title);
			p.setPen(QColor(0x67, 0x75, 0x84));
			p.setFont(st::defaultTextStyle.font->f);
			const auto focusLine = QFontMetrics(st::defaultTextStyle.font->f).elidedText(
				selectedMeta.subtitle
					+ u" · "_q
					+ (selectedHidden
						? RuEn("скрыто снизу", "hidden below")
						: RuEn("live preview", "live preview")),
				Qt::ElideRight,
				focusRect.width() - 62);
			p.drawText(
				QRect(focusRect.left() + 50, focusRect.top() + 28, focusRect.width() - 62, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				focusLine);
		}

		auto top = profileAtBottom ? 24 : (profileRect.bottom() + 16);
		const auto rowsBottom = profileAtBottom
			? (profileRect.top() - 14)
			: (height() - footerHeight - 18);
		auto shownRows = 0;
		for (auto actualIndex = 0; actualIndex != int(_state->entries().size()); ++actualIndex) {
			const auto &entry = _state->entries()[actualIndex];
			if (!entry.visible) {
				continue;
			}
			if (top > (rowsBottom - 30)) {
				break;
			}
			const auto meta = DescribeEntry(entry, _state->supportMode());
			if (entry.separator) {
				const auto separatorColor = IsCustomSeparatorId(entry.id)
					? QColor(0x35, 0xC3, 0x8F, 108)
					: (entry.id == QString::fromLatin1(Menu::Customization::SideMenuItemId::SeparatorSystem))
						? QColor(0x4D, 0xB7, 0x88, 92)
						: QColor(255, 255, 255, 64);
				p.fillRect(
					QRect(26, top + 13, menuWidth - 52, 1),
					separatorColor);
				p.setPen(QColor(255, 255, 255, 124));
				p.setFont(st::normalFont->f);
				p.drawText(
					QRect(menuWidth - 118, top + 2, 92, 18),
					Qt::AlignRight | Qt::AlignVCenter,
					IsCustomSeparatorId(entry.id)
						? RuEn("custom divider", "custom divider")
						: RuEn("runtime divider", "runtime divider"));
				top += 18;
				continue;
			}
			const auto selectedRow = (actualIndex == selectedIndex);
			const auto astroHookRow = (entry.id == QString::fromLatin1(Menu::Customization::SideMenuItemId::Plugins))
				|| (entry.id == QString::fromLatin1(Menu::Customization::SideMenuItemId::ShowLogs))
				|| (entry.id == QString::fromLatin1(Menu::Customization::SideMenuItemId::GhostMode));
			const auto rowRect = QRect(18, top, menuWidth - 36, expanded ? 38 : 34);
			p.setBrush(selectedRow
				? QColor(255, 255, 255, 28)
				: astroHookRow
					? QColor(0x35, 0xC3, 0x8F, shownRows == 0 ? 34 : 26)
					: (shownRows == 0
						? QColor(255, 255, 255, 18)
						: QColor(255, 255, 255, 8)));
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(rowRect, 14, 14);
			if (selectedRow) {
				p.setPen(QPen(QColor(0x35, 0xC3, 0x8F, 210), 2));
				p.drawRoundedRect(rowRect.adjusted(1, 1, -1, -1), 14, 14);
				p.setPen(Qt::NoPen);
			}

			p.setBrush(meta.color);
			p.drawEllipse(QRect(rowRect.left() + 8, rowRect.top() + 6, 22, 22));
			p.setPen(Qt::white);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(rowRect.left() + 8, rowRect.top() + 6, 22, 22),
				Qt::AlignCenter,
				meta.glyph.left(1));

			p.setPen(QColor(255, 255, 255, 232));
			p.drawText(
				QRect(
					rowRect.left() + 42,
					rowRect.top(),
					rowRect.width() - 126,
					rowRect.height()),
				Qt::AlignVCenter | Qt::AlignLeft,
				meta.title);
			if (astroHookRow) {
				const auto pillRect = QRect(rowRect.right() - 74, rowRect.top() + 7, 56, 20);
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(255, 255, 255, 32));
				p.drawRoundedRect(pillRect, 10, 10);
				p.setPen(QColor(255, 255, 255, 220));
				p.setFont(st::normalFont->f);
				p.drawText(
					pillRect,
					Qt::AlignCenter,
					(entry.id == QString::fromLatin1(Menu::Customization::SideMenuItemId::Plugins))
						? RuEn("Plugins", "Plugins")
						: (entry.id == QString::fromLatin1(Menu::Customization::SideMenuItemId::ShowLogs))
							? RuEn("Logs", "Logs")
							: RuEn("Ghost", "Ghost"));
			}
			top += expanded ? 44 : 40;
			++shownRows;
		}

		if (hiddenCount > 0) {
			const auto hiddenRect = profileAtBottom
				? QRect(24, profileRect.top() - 34, menuWidth - 48, 20)
				: QRect(24, height() - footerHeight - 30, menuWidth - 48, 18);
			p.setPen(QColor(255, 255, 255, 160));
			p.setFont(st::normalFont->f);
			p.drawText(
				hiddenRect,
				Qt::AlignLeft | Qt::AlignVCenter,
				RuEn(
					"%1 скрыто в editor-е",
					"%1 hidden in the editor").arg(hiddenCount));
		}

		if (showFooterText) {
			p.setPen(QColor(255, 255, 255, 126));
			p.setFont(st::normalFont->f);
			p.drawText(
				QRect(24, height() - 28, menuWidth - 48, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				RuEn(
					"Astrogram Desktop · footer text",
					"Astrogram Desktop · footer text"));
		}
		auto chatChipIndex = 0;
		auto previewChipIndex = 0;
		auto drawModeChip = [&](const QString &text, int fillAlpha, bool onMenu) {
			const auto fm = QFontMetrics(st::normalFont->f);
			const auto chipWidth = std::max(104, fm.horizontalAdvance(text) + 24);
			const auto chipRect = onMenu
				? QRect(24 + (previewChipIndex * (chipWidth + 8)), height() - 64, chipWidth, 28)
				: QRect(chatRect.left() + 12 + (chatChipIndex * (chipWidth + 8)), 12, chipWidth, 28);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x35, 0xC3, 0x8F, fillAlpha));
			p.drawRoundedRect(chipRect, 14, 14);
			p.setPen(onMenu ? QColor(255, 255, 255, 220) : QColor(0x13, 0x2B, 0x3F));
			p.setFont(st::normalFont->f);
			p.drawText(chipRect, Qt::AlignCenter, text);
			if (onMenu) {
				++previewChipIndex;
			} else {
				++chatChipIndex;
			}
		};
		if (immersive) {
			drawModeChip(RuEn("Иммерсивно", "Immersive"), 48, false);
		}
		if (leftEdgeSettings) {
			drawModeChip(RuEn("Левый край", "Left-edge"), 42, false);
		}
		if (wideSettings) {
			drawModeChip(RuEn("Шире settings", "Wide settings"), 42, false);
		}
		if (expanded) {
			drawModeChip(RuEn("Расширенная панель", "Expanded panel"), 38, true);
		}
		if (stats.pluginsVisible) {
			drawModeChip(RuEn("Кнопка Plugins", "Plugins button"), 34, true);
		}
		if (stats.showLogsVisible) {
			drawModeChip(RuEn("Show Logs", "Show Logs"), 34, true);
		}
		if (stats.visibleSeparators > 0) {
			drawModeChip(
				RuEn("Разделители %1", "Dividers %1").arg(stats.visibleSeparators),
				36,
				false);
		}
		if (!showFooterText) {
			drawModeChip(RuEn("Без footer", "Footer hidden"), 34, true);
		}
		if (profileAtBottom) {
			drawModeChip(RuEn("Профиль внизу", "Profile bottom"), 34, true);
		}

		p.setClipping(false);
		p.setPen(QColor(0xD3, 0xDE, 0xE8));
		p.drawRoundedRect(outer, kPreviewRadius, kPreviewRadius);
	}

private:
	const std::shared_ptr<SideMenuEditorState> _state;
};

class FutureSurfacesPreview final : public Ui::RpWidget {
public:
	FutureSurfacesPreview(
		QWidget *parent,
		std::shared_ptr<SideMenuEditorState> state)
	: RpWidget(parent)
	, _state(std::move(state)) {
		update();
		_state->changes() | rpl::on_next([=] {
			update();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return kFuturePreviewHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);

		const auto outer = rect().adjusted(0, 0, -1, -1);
		auto path = QPainterPath();
		path.addRoundedRect(outer, kPreviewRadius, kPreviewRadius);
		p.fillPath(path, QColor(0xF6, 0xFA, 0xFD));
		p.setClipPath(path);

		auto gradient = QLinearGradient(
			QPointF(0., 0.),
			QPointF(width(), height()));
		gradient.setColorAt(0., QColor(0xF2, 0xFB, 0xF7));
		gradient.setColorAt(1., QColor(0xF8, 0xFB, 0xFD));
		p.fillRect(rect(), gradient);

		const auto scene = QRect(18, 18, width() - 36, height() - 36);
		p.setBrush(QColor(0xE5, 0xF2, 0xEA));
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(scene, 22, 22);

		const auto leftRailWidth = _state->leftEdgeSettings() ? 170 : 134;
		const auto sidePane = QRect(scene.left() + 12, scene.top() + 12, leftRailWidth, scene.height() - 24);
		auto railGradient = QLinearGradient(
			QPointF(sidePane.left(), sidePane.top()),
			QPointF(sidePane.right(), sidePane.bottom()));
		railGradient.setColorAt(0., QColor(0x17, 0x34, 0x4C, 232));
		railGradient.setColorAt(1., QColor(0x1D, 0x44, 0x63, 232));
		p.setBrush(railGradient);
		p.drawRoundedRect(sidePane, 18, 18);
		p.setBrush(QColor(255, 255, 255, 24));
		p.drawRoundedRect(QRect(sidePane.left() + 12, sidePane.top() + 12, sidePane.width() - 24, 34), 12, 12);
		p.setBrush(QColor(0x35, 0xC3, 0x8F));
		p.drawEllipse(QRect(sidePane.left() + 22, sidePane.top() + 17, 22, 22));
		p.setPen(Qt::white);
		p.setFont(st::normalFont->f);
		p.drawText(
			QRect(sidePane.left() + 54, sidePane.top() + 16, sidePane.width() - 66, 24),
			Qt::AlignLeft | Qt::AlignVCenter,
			_state->leftEdgeSettings()
				? RuEn("Left-edge settings", "Left-edge settings")
				: RuEn("Preview shell", "Preview shell"));

		const auto chatRect = QRect(
			sidePane.right() + 14,
			scene.top() + 12,
			scene.width() - sidePane.width() - 28,
			scene.height() - 24);
		auto chatGradient = QLinearGradient(
			QPointF(chatRect.left(), chatRect.top()),
			QPointF(chatRect.right(), chatRect.bottom()));
		chatGradient.setColorAt(0., QColor(0xF7, 0xFB, 0xFD));
		chatGradient.setColorAt(1., QColor(0xEC, 0xF5, 0xEF));
		p.setBrush(chatGradient);
		p.drawRoundedRect(chatRect, 18, 18);

		p.setBrush(QColor(255, 255, 255, 220));
		p.drawRoundedRect(QRect(chatRect.left() + 18, chatRect.top() + 20, 214, 42), 16, 16);
		p.drawRoundedRect(QRect(chatRect.right() - 236, chatRect.top() + 84, 218, 46), 16, 16);
		p.drawRoundedRect(QRect(chatRect.left() + 48, chatRect.top() + 146, 248, 52), 16, 16);

		const auto peerPopup = QRect(chatRect.right() - 182, chatRect.top() + 18, 162, 126);
		p.setBrush(QColor(255, 255, 255, 242));
		p.drawRoundedRect(peerPopup, 18, 18);
		p.setBrush(QColor(0xD9, 0xF6, 0xE8));
		p.drawRoundedRect(QRect(peerPopup.left() + 12, peerPopup.top() + 12, 78, 22), 11, 11);
		p.setPen(QColor(0x1C, 0x8B, 0x62));
		p.drawText(
			QRect(peerPopup.left() + 12, peerPopup.top() + 12, 78, 22),
			Qt::AlignCenter,
			RuEn("peer menu", "peer menu"));
		p.setPen(QColor(0x23, 0x2F, 0x3C));
		p.drawText(
			QRect(peerPopup.left() + 14, peerPopup.top() + 44, peerPopup.width() - 28, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn("Переслать без автора", "Forward without author"));
		p.drawText(
			QRect(peerPopup.left() + 14, peerPopup.top() + 68, peerPopup.width() - 28, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn("Показать удалённые", "Show deleted"));
		p.drawText(
			QRect(peerPopup.left() + 14, peerPopup.top() + 92, peerPopup.width() - 28, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn("Скопировать ID", "Copy ID"));

		const auto contextPopup = QRect(chatRect.left() + 96, chatRect.top() + 72, 204, 112);
		p.setBrush(QColor(255, 255, 255, 246));
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(contextPopup, 18, 18);
		p.setBrush(QColor(0xEA, 0xF7, 0xF1));
		p.drawRoundedRect(QRect(contextPopup.left() + 14, contextPopup.top() + 14, 98, 22), 11, 11);
		p.setPen(QColor(0x1C, 0x8B, 0x62));
		p.drawText(
			QRect(contextPopup.left() + 14, contextPopup.top() + 14, 98, 22),
			Qt::AlignCenter,
			RuEn("context menu", "context menu"));
		p.setPen(QColor(0x23, 0x2F, 0x3C));
		p.drawText(
			QRect(contextPopup.left() + 16, contextPopup.top() + 44, contextPopup.width() - 32, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn("Ответить", "Reply"));
		p.drawText(
			QRect(contextPopup.left() + 16, contextPopup.top() + 66, contextPopup.width() - 32, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn("Изменить позже", "Edit later"));
		p.drawText(
			QRect(contextPopup.left() + 16, contextPopup.top() + 88, contextPopup.width() - 32, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn("Меню нижних иконок", "Bottom icon strip"));

		const auto strip = QRect(contextPopup.left() + 10, contextPopup.bottom() + 8, contextPopup.width() - 20, 30);
		p.setBrush(QColor(0xE7, 0xF8, 0xEF));
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(strip, 15, 15);
		for (auto i = 0; i != 4; ++i) {
			p.setBrush(QColor(0x35, 0xC3, 0x8F, i == 0 ? 210 : 128));
			p.drawEllipse(QRect(strip.left() + 16 + (i * 34), strip.top() + 5, 20, 20));
		}

		p.setClipping(false);
		p.setPen(QColor(0xD9, 0xE3, 0xEC));
		p.drawRoundedRect(outer, kPreviewRadius, kPreviewRadius);
	}

private:
	const std::shared_ptr<SideMenuEditorState> _state;
};

class SideMenuEntryList final : public Ui::RpWidget {
public:
	SideMenuEntryList(
		QWidget *parent,
		std::shared_ptr<SideMenuEditorState> state)
	: RpWidget(parent)
	, _state(std::move(state)) {
		setMouseTracking(true);
		const auto refreshFromState = [=] {
			const auto selected = _state->selectedEntryIndex();
			if ((selected >= 0)
				&& (selected < int(_state->entries().size()))
				&& _state->entries()[selected].visible) {
				_selected = selected;
			}
			clampSelection();
			update();
			updateGeometry();
		};
		refreshFromState();
		_state->changes() | rpl::on_next([=] {
			refreshFromState();
		}, lifetime());
	}

	[[nodiscard]] int selectedIndex() const {
		return _selected;
	}

	void setSelectedIndex(int index) {
		_selected = index;
		clampSelection();
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto visibleCount = std::max(int(visibleEntryIndexes().size()), 1);
		const auto base = 18 + (visibleCount * (kRowHeight + kRowGap));
		return base + hiddenSectionHeight(newWidth);
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto visible = visibleEntryIndexes();
		if (visible.empty()) {
			const auto emptyRect = QRect(0, 8, width(), kRowHeight + 8);
			p.setPen(QPen(ThemedRowBorder(false, false)));
			p.setBrush(st::boxBg);
			p.drawRoundedRect(emptyRect, kRowRadius, kRowRadius);
			p.setPen(st::windowFg);
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 12, -18, -18),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Все пункты сейчас скрыты из меню",
					"All actions are currently hidden"));
			p.setPen(st::windowSubTextFg);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 34, -18, -12),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Ниже остаются скрытые пункты: можно вернуть любой из них одним нажатием или сразу восстановить всё меню.",
					"Hidden items stay below: you can bring back any of them with one click or restore the whole menu at once."));
		} else {
			for (const auto index : visible) {
				if (index == _draggingRow) {
					continue;
				}
				const auto row = rowRect(index);
				paintRow(
					p,
					index,
					row,
					(index == _hoveredRow),
					(index == _selected),
					false);
			}
		}

		if (_draggingRow >= 0) {
			p.setPen(QPen(st::windowBgActive->c, 3));
			const auto y = insertionLineY();
			p.drawLine(QPoint(8, y), QPoint(width() - 8, y));
			paintRow(
				p,
				_draggingRow,
				floatingRowRect(),
				false,
				true,
				true);
		}

		const auto chips = hiddenChips(width());
		if (!chips.empty()) {
			const auto hiddenCount = std::count_if(
				_state->entries().begin(),
				_state->entries().end(),
				[](const auto &entry) { return !entry.visible; });
			p.setPen(st::windowSubTextFg);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(0, hiddenSectionTop(), width(), 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				RuEn(
					"Скрытые пункты (%1)",
					"Hidden items (%1)").arg(hiddenCount));
			for (const auto &chip : chips) {
				const auto hovered = chip.restoreAll
					? _hoveredRestoreAll
					: (chip.index == _hoveredHiddenEntry);
				const auto fillColor = chip.restoreAll
					? ThemedButtonFill(hovered, true)
					: anim::with_alpha(
						ThemedDescriptorAccent(chip.color, hovered),
						hovered ? 0.22 : 0.12);
				p.setPen(Qt::NoPen);
				p.setBrush(fillColor);
				p.drawRoundedRect(chip.rect, 14, 14);
				p.setPen(chip.restoreAll
					? ThemedButtonText(true)
					: (hovered ? st::windowFgActive->c : st::windowFg->c));
				p.setFont(st::normalFont->f);
				p.drawText(
					chip.rect.adjusted(12, 0, -12, 0),
					Qt::AlignLeft | Qt::AlignVCenter,
					chip.label);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
			const auto point = e->pos();
		if (_draggingRow >= 0) {
			_dragCurrentY = point.y();
			updateDragTarget(point.y());
			_hoveredRow = -1;
			_hoveredKind = ActionKind::None;
			_hoveredOnHandle = false;
			_hoveredHiddenEntry = -1;
			_hoveredRestoreAll = false;
			update();
			return;
		}
		if ((_pressedRow >= 0)
			&& _pressedOnHandle
			&& !_pressedOnButton
			&& ((point - _pressPoint).manhattanLength() >= 8)) {
			startDrag(point);
			return;
		}
		updateHover(point);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto point = e->pos();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;

		for (const auto &chip : hiddenChips(width())) {
			if (!chip.rect.contains(point)) {
				continue;
			}
			if (chip.restoreAll) {
				_state->restoreAllHidden();
				clampSelection();
			} else if (_state->restoreEntry(chip.index)) {
				_selected = chip.index;
				clampSelection();
			}
			updateHover(point);
			return;
		}

		for (const auto index : visibleEntryIndexes()) {
			const auto row = rowRect(index);
			if (!row.contains(point)) {
				continue;
			}
			_selected = index;
			_state->setSelectedEntryIndex(_selected);
			_pressPoint = point;
			const auto visibleRow = visibleRowForEntry(index);
			_dragGrabOffsetY = point.y() - baseRowRect(visibleRow).top();
			_pressedOnHandle = dragHandleRect(row).contains(point);
			for (const auto &button : actionButtonsForRow(index)) {
				if (!button.enabled || !button.rect.contains(point)) {
					continue;
				}
				_pressedOnButton = true;
				handleAction(index, button.kind);
				updateHover(point);
				return;
			}
			_pressedRow = index;
			updateHover(point);
			update();
			return;
		}
		updateHover(point);
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		if (_draggingRow >= 0) {
			finishDrag();
			return;
		}
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
			updateHover(e->pos());
	}

	void leaveEventHook(QEvent *e) override {
		Q_UNUSED(e);
		if (_draggingRow >= 0) {
			return;
		}
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredOnHandle = false;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		unsetCursor();
		update();
	}

private:
	enum class ActionKind {
		None,
		ToggleVisible,
		DeleteCustomSeparator,
	};

	struct ActionButton {
		QRect rect;
		QString label;
		ActionKind kind = ActionKind::None;
		bool enabled = true;
	};

	struct HiddenChip {
		QRect rect;
		int index = -1;
		QString label;
		QColor color;
		bool restoreAll = false;
	};

	[[nodiscard]] std::vector<int> visibleEntryIndexes() const {
		auto result = std::vector<int>();
		for (auto i = 0; i != int(_state->entries().size()); ++i) {
			if (_state->entries()[i].visible) {
				result.push_back(i);
			}
		}
		return result;
	}

	[[nodiscard]] QRect baseRowRect(int visibleRow) const {
		return QRect(
			0,
			8 + visibleRow * (kRowHeight + kRowGap),
			width(),
			kRowHeight);
	}

	[[nodiscard]] int visibleRowForEntry(int index) const {
		const auto visible = visibleEntryIndexes();
		const auto i = std::find(visible.begin(), visible.end(), index);
		return (i == visible.end()) ? -1 : int(i - visible.begin());
	}

	[[nodiscard]] QRect rowRectByVisibleRow(int visibleRow) const {
		auto result = baseRowRect(visibleRow);
		if ((_draggingVisibleRow < 0)
			|| (_dragTargetIndex < 0)
			|| (visibleRow == _draggingVisibleRow)) {
			return result;
		}
		const auto delta = kRowHeight + kRowGap;
		if (_draggingVisibleRow < _dragTargetIndex) {
			if ((visibleRow > _draggingVisibleRow) && (visibleRow <= _dragTargetIndex)) {
				result.translate(0, -delta);
			}
		} else if (_draggingVisibleRow > _dragTargetIndex) {
			if ((visibleRow >= _dragTargetIndex) && (visibleRow < _draggingVisibleRow)) {
				result.translate(0, delta);
			}
		}
		return result;
	}

	[[nodiscard]] QRect rowRect(int index) const {
		const auto visibleRow = visibleRowForEntry(index);
		return (visibleRow >= 0) ? rowRectByVisibleRow(visibleRow) : QRect();
	}

	[[nodiscard]] QRect floatingRowRect() const {
		auto result = baseRowRect(_draggingVisibleRow);
		result.moveTop(_dragCurrentY - _dragGrabOffsetY);
		return result;
	}

	[[nodiscard]] QRect dragHandleRect(const QRect &row) const {
		return QRect(row.left() + 10, row.top() + 14, 18, row.height() - 28);
	}

	[[nodiscard]] int hiddenSectionTop() const {
		const auto visibleCount = std::max(int(visibleEntryIndexes().size()), 1);
		return 18 + (visibleCount * (kRowHeight + kRowGap)) + 4;
	}

	[[nodiscard]] std::vector<HiddenChip> hiddenChips(int availableWidth) const {
		auto result = std::vector<HiddenChip>();
		const auto usableWidth = std::max(availableWidth, 120);
		const auto fm = QFontMetrics(st::normalFont->f);
		auto pushChip = [&](int index, QString label, QColor color, bool restoreAll) {
			label = fm.elidedText(label, Qt::ElideRight, 170);
			auto chipWidth = std::min(
				std::max(82, fm.horizontalAdvance(label) + 24),
				usableWidth);
			auto x = 0;
			auto y = hiddenSectionTop() + 24;
			if (!result.empty()) {
				const auto &last = result.back();
				x = last.rect.right() + 9;
				y = last.rect.top();
				if (x + chipWidth > usableWidth) {
					x = 0;
					y += 36;
				}
			}
			result.push_back(HiddenChip{
				.rect = QRect(x, y, chipWidth, 28),
				.index = index,
				.label = std::move(label),
				.color = color,
				.restoreAll = restoreAll,
			});
		};
		auto hiddenIndexes = std::vector<int>();
		for (auto i = 0; i != int(_state->entries().size()); ++i) {
			if (!_state->entries()[i].visible) {
				hiddenIndexes.push_back(i);
			}
		}
		if (hiddenIndexes.empty()) {
			return result;
		}
		pushChip(
			-1,
			RuEn("Вернуть всё", "Restore all"),
			st::windowBgActive->c,
			true);
		for (const auto index : hiddenIndexes) {
			const auto meta = DescribeEntry(_state->entries()[index], _state->supportMode());
			pushChip(index, meta.title, meta.color, false);
		}
		return result;
	}

	[[nodiscard]] int hiddenSectionHeight(int availableWidth) const {
		const auto chips = hiddenChips(availableWidth);
		if (chips.empty()) {
			return 0;
		}
		auto bottom = hiddenSectionTop() + 18;
		for (const auto &chip : chips) {
			bottom = std::max(bottom, chip.rect.bottom());
		}
		return (bottom - hiddenSectionTop()) + 16;
	}

	[[nodiscard]] std::vector<ActionButton> actionButtonsForRow(int index) const {
		const auto &entries = _state->entries();
		if ((index < 0) || (index >= int(entries.size())) || !entries[index].visible) {
			return {};
		}
		const auto row = rowRect(index);
		const auto &entry = entries[index];
		const auto fm = QFontMetrics(st::normalFont->f);
		const auto height = 28;
		auto right = row.right() - kRowPadding;
		auto result = std::vector<ActionButton>();
		const auto push = [&](QString label, ActionKind kind, bool enabled) {
			const auto buttonWidth = std::max(44, fm.horizontalAdvance(label) + 22);
			right -= buttonWidth;
			result.push_back(ActionButton{
				.rect = QRect(right, row.top() + 18, buttonWidth, height),
				.label = std::move(label),
				.kind = kind,
				.enabled = enabled,
			});
			right -= 8;
		};

		if (entry.separator && IsCustomSeparatorId(entry.id)) {
			push(RuEn("Удалить", "Delete"), ActionKind::DeleteCustomSeparator, true);
		}
		if (entry.id != QString::fromLatin1(
				Menu::Customization::SideMenuItemId::Settings)) {
			push(RuEn("Скрыть", "Hide"), ActionKind::ToggleVisible, true);
		}
		std::reverse(result.begin(), result.end());
		return result;
	}

	void paintActionButton(QPainter &p, const ActionButton &button) const {
		const auto hovered = (_hoveredRow >= 0)
			&& (button.kind == _hoveredKind)
			&& button.rect.intersects(rowRect(_hoveredRow));
		const auto active = button.enabled;
		const auto destructive = (button.kind == ActionKind::DeleteCustomSeparator);
		p.setPen(Qt::NoPen);
		p.setBrush(destructive
			? ThemedDestructiveButtonFill(hovered, active)
			: ThemedButtonFill(hovered, active));
		p.drawRoundedRect(button.rect, 14, 14);
		p.setPen(destructive
			? ThemedDestructiveButtonText(active)
			: ThemedButtonText(active));
		p.setFont(st::normalFont->f);
		p.drawText(button.rect, Qt::AlignCenter, button.label);
	}

	void paintDragHandle(Painter &p, const QRect &row, bool active) const {
		const auto handle = dragHandleRect(row);
		p.setPen(Qt::NoPen);
		p.setBrush(active
			? st::windowBgActive->c
			: anim::with_alpha(st::windowSubTextFg->c, 0.65));
		for (auto column = 0; column != 2; ++column) {
			for (auto line = 0; line != 3; ++line) {
				p.drawEllipse(
					QRect(
						handle.left() + (column * 6),
						handle.top() + 2 + (line * 8),
						3,
						3));
			}
		}
	}

	void paintRow(
			Painter &p,
			int index,
			const QRect &row,
			bool hovered,
			bool selected,
			bool floating) const {
		const auto &entry = _state->entries()[index];
		const auto meta = DescribeEntry(entry, _state->supportMode());
		const auto accent = ThemedDescriptorAccent(
			meta.color,
			hovered || selected || floating);
		p.setPen(QPen(ThemedRowBorder(hovered, selected)));
		p.setBrush(ThemedRowFill(hovered, selected, floating));
		p.drawRoundedRect(row, kRowRadius, kRowRadius);

		paintDragHandle(p, row, hovered || selected || floating);
		p.setBrush(accent);
		p.drawEllipse(QRect(row.left() + 36, row.top() + 14, 36, 36));
		if (meta.icon) {
			meta.icon->paintInCenter(
				p,
				QRect(row.left() + 36, row.top() + 14, 36, 36));
		} else {
			p.setPen(Qt::white);
			p.setFont(st::semiboldFont->f);
			p.drawText(
				QRect(row.left() + 36, row.top() + 14, 36, 36),
				Qt::AlignCenter,
				meta.glyph.left(1));
		}

		p.setPen(st::windowFg);
		p.setFont(st::semiboldTextStyle.font->f);
		p.drawText(
			QRect(row.left() + 84, row.top() + 12, row.width() - 242, 20),
			Qt::AlignLeft | Qt::AlignVCenter,
			meta.title);

		p.setPen(st::windowSubTextFg);
		p.setFont(st::defaultTextStyle.font->f);
		const auto subtitle = QFontMetrics(st::defaultTextStyle.font->f).elidedText(
			meta.subtitle,
			Qt::ElideRight,
			row.width() - 242);
		p.drawText(
			QRect(row.left() + 84, row.top() + 34, row.width() - 242, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			subtitle);

		if (!floating) {
			for (const auto &button : actionButtonsForRow(index)) {
				paintActionButton(p, button);
			}
		}
	}

	[[nodiscard]] int insertionLineY() const {
		if ((_draggingVisibleRow < 0) || (_dragTargetIndex < 0)) {
			return 0;
		}
		const auto visible = visibleEntryIndexes();
		auto slot = 0;
		for (auto visibleRow = 0; visibleRow != int(visible.size()); ++visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			if (slot == _dragTargetIndex) {
				return rowRectByVisibleRow(visibleRow).top() - (kRowGap / 2);
			}
			++slot;
		}
		for (auto visibleRow = int(visible.size()) - 1; visibleRow >= 0; --visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			return rowRectByVisibleRow(visibleRow).bottom() + (kRowGap / 2) + 1;
		}
		return baseRowRect(0).center().y();
	}

	void updateDragTarget(int y) {
		if (_draggingVisibleRow < 0) {
			return;
		}
		const auto visible = visibleEntryIndexes();
		auto slot = 0;
		for (auto visibleRow = 0; visibleRow != int(visible.size()); ++visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			if (y < rowRectByVisibleRow(visibleRow).center().y()) {
				_dragTargetIndex = slot;
				return;
			}
			++slot;
		}
		_dragTargetIndex = slot;
	}

	void startDrag(QPoint point) {
		if (_pressedRow < 0) {
			return;
		}
		_draggingRow = _pressedRow;
		_draggingVisibleRow = visibleRowForEntry(_pressedRow);
		if (_draggingVisibleRow < 0) {
			return;
		}
		_dragTargetIndex = _draggingVisibleRow;
		_dragCurrentY = point.y();
		_dragGrabOffsetY = point.y() - baseRowRect(_draggingVisibleRow).top();
		_pressedRow = -1;
		_pressedOnButton = false;
		grabMouse();
		setCursor(Qt::ClosedHandCursor);
		updateDragTarget(point.y());
		update();
	}

	void finishDrag() {
		if (_draggingVisibleRow < 0) {
			return;
		}
		const auto maxTarget = std::max(int(visibleEntryIndexes().size()) - 1, 0);
		const auto target = std::clamp(_dragTargetIndex, 0, maxTarget);
		_state->moveVisibleEntry(_draggingVisibleRow, target);
		_selected = _state->selectedEntryIndex();
		_draggingRow = -1;
		_draggingVisibleRow = -1;
		_dragTargetIndex = -1;
		_dragCurrentY = 0;
		_dragGrabOffsetY = 0;
		releaseMouse();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredOnHandle = false;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		clampSelection();
		unsetCursor();
		update();
	}

	void updateHover(QPoint point) {
		auto newHiddenEntry = -1;
		auto newRestoreAll = false;
		for (const auto &chip : hiddenChips(width())) {
			if (!chip.rect.contains(point)) {
				continue;
			}
			newRestoreAll = chip.restoreAll;
			newHiddenEntry = chip.restoreAll ? -1 : chip.index;
			break;
		}

		auto newRow = -1;
		auto newKind = ActionKind::None;
		auto newOnHandle = false;
		if (!newRestoreAll && (newHiddenEntry < 0)) {
			for (const auto index : visibleEntryIndexes()) {
				const auto row = rowRect(index);
				if (!row.contains(point)) {
					continue;
				}
				newRow = index;
				newOnHandle = dragHandleRect(row).contains(point);
				for (const auto &button : actionButtonsForRow(index)) {
					if (button.enabled && button.rect.contains(point)) {
						newKind = button.kind;
						break;
					}
				}
				break;
			}
		}

		if ((newRow == _hoveredRow)
			&& (newKind == _hoveredKind)
			&& (newOnHandle == _hoveredOnHandle)
			&& (newHiddenEntry == _hoveredHiddenEntry)
			&& (newRestoreAll == _hoveredRestoreAll)) {
			return;
		}
		_hoveredRow = newRow;
		_hoveredKind = newKind;
		_hoveredOnHandle = newOnHandle;
		_hoveredHiddenEntry = newHiddenEntry;
		_hoveredRestoreAll = newRestoreAll;
		if (_hoveredRestoreAll || (_hoveredHiddenEntry >= 0) || ((_hoveredRow >= 0) && (_hoveredKind != ActionKind::None))) {
			setCursor(Qt::PointingHandCursor);
		} else if (_hoveredOnHandle) {
			setCursor(Qt::OpenHandCursor);
		} else {
			unsetCursor();
		}
		update();
	}

	void handleAction(int index, ActionKind kind) {
		auto changed = false;
		switch (kind) {
		case ActionKind::ToggleVisible:
			changed = _state->toggleVisible(index);
			break;
		case ActionKind::DeleteCustomSeparator:
			changed = _state->removeCustomSeparator(index);
			break;
		case ActionKind::None:
			break;
		}
		if (changed) {
			_selected = _state->selectedEntryIndex();
			clampSelection();
		} else {
			update();
		}
	}

	void clampSelection() {
		const auto visible = visibleEntryIndexes();
		if (visible.empty()) {
			_selected = -1;
			_state->setSelectedEntryIndex(-1);
			return;
		}
		if ((_selected < 0)
			|| (_selected >= int(_state->entries().size()))
			|| !_state->entries()[_selected].visible) {
			_selected = visible.front();
		}
		_state->setSelectedEntryIndex(_selected);
	}

	const std::shared_ptr<SideMenuEditorState> _state;
	int _selected = -1;
	int _hoveredRow = -1;
	ActionKind _hoveredKind = ActionKind::None;
	bool _hoveredOnHandle = false;
	int _hoveredHiddenEntry = -1;
	bool _hoveredRestoreAll = false;
	int _pressedRow = -1;
	bool _pressedOnButton = false;
	bool _pressedOnHandle = false;
	QPoint _pressPoint;
	int _draggingRow = -1;
	int _draggingVisibleRow = -1;
	int _dragTargetIndex = -1;
	int _dragCurrentY = 0;
	int _dragGrabOffsetY = 0;
};

[[nodiscard]] std::vector<Menu::Customization::PeerMenuEntry>
DefaultThreeDotsMenuLayout() {
	using namespace Menu::Customization;
	return {
		{ QString::fromLatin1(PeerMenuItemId::NewWindow), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ToggleMute), true, false },
		{ QString::fromLatin1(PeerMenuItemId::TogglePin), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ToggleUnreadMark), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ToggleArchive), true, false },
		{ QString::fromLatin1(PeerMenuItemId::SeparatorPrimary), true, true },
		{ QString::fromLatin1(PeerMenuItemId::CreateTopic), true, false },
		{ QString::fromLatin1(PeerMenuItemId::SeparatorSecondary), true, true },
		{ QString::fromLatin1(PeerMenuItemId::Info), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ViewAsTopics), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ManageChat), true, false },
		{ QString::fromLatin1(PeerMenuItemId::NewMembers), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ViewStatistics), true, false },
		{ QString::fromLatin1(PeerMenuItemId::StoryArchive), true, false },
		{ QString::fromLatin1(PeerMenuItemId::SupportInfo), true, false },
		{ QString::fromLatin1(PeerMenuItemId::BoostChat), true, false },
		{ QString::fromLatin1(PeerMenuItemId::VideoChat), true, false },
		{ QString::fromLatin1(PeerMenuItemId::CreatePoll), true, false },
		{ QString::fromLatin1(PeerMenuItemId::CreateTodoList), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ThemeEdit), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ViewDiscussion), true, false },
		{ QString::fromLatin1(PeerMenuItemId::DirectMessages), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ExportChat), true, false },
		{ QString::fromLatin1(PeerMenuItemId::Translate), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ToggleFolder), true, false },
		{ QString::fromLatin1(PeerMenuItemId::DeletedMessages), true, false },
		{ QString::fromLatin1(PeerMenuItemId::BlockUser), true, false },
		{ QString::fromLatin1(PeerMenuItemId::SeparatorDanger), true, true },
		{ QString::fromLatin1(PeerMenuItemId::Report), true, false },
		{ QString::fromLatin1(PeerMenuItemId::ClearHistory), true, false },
		{ QString::fromLatin1(PeerMenuItemId::DeleteChat), true, false },
		{ QString::fromLatin1(PeerMenuItemId::LeaveChat), true, false },
		{ QString::fromLatin1(PeerMenuItemId::JoinChat), true, false },
	};
}

[[nodiscard]] const style::icon *ThreeDotsActionIcon(const QString &id) {
	namespace Id = Menu::Customization::PeerMenuItemId;
	if (id == QString::fromLatin1(Id::NewWindow)) {
		return &st::menuIconNewWindow;
	} else if (id == QString::fromLatin1(Id::ToggleMute)) {
		return &st::menuIconMute;
	} else if (id == QString::fromLatin1(Id::TogglePin)) {
		return &st::menuIconPin;
	} else if (id == QString::fromLatin1(Id::ToggleUnreadMark)) {
		return &st::menuIconMarkUnread;
	} else if (id == QString::fromLatin1(Id::ToggleArchive)) {
		return &st::menuIconArchive;
	} else if (id == QString::fromLatin1(Id::Info)) {
		return &st::menuIconInfo;
	} else if (id == QString::fromLatin1(Id::ViewAsMessages)) {
		return &st::menuIconAsMessages;
	} else if (id == QString::fromLatin1(Id::ViewAsTopics)) {
		return &st::menuIconAsTopics;
	} else if (id == QString::fromLatin1(Id::SearchTopics)) {
		return &st::menuIconSearch;
	} else if (id == QString::fromLatin1(Id::ManageChat)
		|| id == QString::fromLatin1(Id::ManageTopic)) {
		return &st::menuIconManage;
	} else if (id == QString::fromLatin1(Id::SupportInfo)) {
		return &st::menuIconInfo;
	} else if (id == QString::fromLatin1(Id::StoryArchive)) {
		return &st::menuIconStoriesArchiveSection;
	} else if (id == QString::fromLatin1(Id::BoostChat)) {
		return &st::menuIconBoosts;
	} else if (id == QString::fromLatin1(Id::VideoChat)) {
		return &st::menuIconVideoChat;
	} else if (id == QString::fromLatin1(Id::CreatePoll)) {
		return &st::menuIconCreatePoll;
	} else if (id == QString::fromLatin1(Id::CreateTodoList)) {
		return &st::menuIconCreateTodoList;
	} else if (id == QString::fromLatin1(Id::ThemeEdit)) {
		return &st::menuIconChangeColors;
	} else if (id == QString::fromLatin1(Id::ViewDiscussion)
		|| id == QString::fromLatin1(Id::CreateTopic)) {
		return &st::menuIconDiscussion;
	} else if (id == QString::fromLatin1(Id::DirectMessages)) {
		return &st::menuIconChatDiscuss;
	} else if (id == QString::fromLatin1(Id::ExportChat)) {
		return &st::menuIconExport;
	} else if (id == QString::fromLatin1(Id::Translate)) {
		return &st::menuIconTranslate;
	} else if (id == QString::fromLatin1(Id::DeletedMessages)) {
		return &st::menuIconArchive;
	} else if (id == QString::fromLatin1(Id::Report)) {
		return &st::menuIconReport;
	} else if (id == QString::fromLatin1(Id::NewContact)
		|| id == QString::fromLatin1(Id::BotToGroup)
		|| id == QString::fromLatin1(Id::NewMembers)
		|| id == QString::fromLatin1(Id::JoinChat)) {
		return &st::menuIconInvite;
	} else if (id == QString::fromLatin1(Id::ShareContact)) {
		return &st::menuIconShare;
	} else if (id == QString::fromLatin1(Id::EditContact)) {
		return &st::menuIconEdit;
	} else if (id == QString::fromLatin1(Id::DeleteContact)
		|| id == QString::fromLatin1(Id::DeleteTopic)
		|| id == QString::fromLatin1(Id::DeleteChat)) {
		return &st::menuIconDeleteAttention;
	} else if (id == QString::fromLatin1(Id::TopicLink)) {
		return &st::menuIconCopy;
	} else if (id == QString::fromLatin1(Id::ToggleTopicClosed)
		|| id == QString::fromLatin1(Id::BlockUser)) {
		return &st::menuIconBlock;
	} else if (id == QString::fromLatin1(Id::SendGift)) {
		return &st::menuIconGiftPremium;
	} else if (id == QString::fromLatin1(Id::ViewStatistics)) {
		return &st::menuIconStats;
	} else if (id == QString::fromLatin1(Id::ToggleFolder)
		|| id == QString::fromLatin1(Id::SetPersonalChannel)) {
		return &st::menuIconAddToFolder;
	} else if (id == QString::fromLatin1(Id::ClearHistory)) {
		return &st::menuIconClear;
	} else if (id == QString::fromLatin1(Id::LeaveChat)) {
		return &st::menuIconLeaveAttention;
	} else if (id == QString::fromLatin1(Id::HidePromotion)) {
		return &st::menuIconRemove;
	}
	return nullptr;
}

[[nodiscard]] QString HumanizePeerMenuId(const QString &id) {
	auto result = id;
	result.replace(u'_', u' ');
	result.replace(u'.', u' ');
	if (!result.isEmpty()) {
		result[0] = result[0].toUpper();
	}
	return result;
}

[[nodiscard]] EntryDescriptor DescribeThreeDotsEntry(
		const Menu::Customization::PeerMenuEntry &entry) {
	namespace Id = Menu::Customization::PeerMenuItemId;
	if (entry.separator) {
		if (entry.id == QString::fromLatin1(Id::SeparatorPrimary)) {
			return {
				.title = RuEn("Основной разделитель", "Primary divider"),
				.subtitle = RuEn(
					"Отделяет быстрые действия от остального меню.",
					"Separates quick actions from the rest of the menu."),
				.glyph = u"="_q,
				.icon = nullptr,
				.color = QColor(0x76, 0x86, 0x9B),
			};
		} else if (entry.id == QString::fromLatin1(Id::SeparatorSecondary)) {
			return {
				.title = RuEn("Второй разделитель", "Secondary divider"),
				.subtitle = RuEn(
					"Отделяет дополнительные действия внутри меню.",
					"Separates secondary actions inside the menu."),
				.glyph = u"="_q,
				.icon = nullptr,
				.color = QColor(0x4A, 0xB8, 0xD8),
			};
		} else if (entry.id == QString::fromLatin1(Id::SeparatorDanger)) {
			return {
				.title = RuEn("Опасный разделитель", "Danger divider"),
				.subtitle = RuEn(
					"Отделяет потенциально опасные действия.",
					"Separates potentially destructive actions."),
				.glyph = u"="_q,
				.icon = nullptr,
				.color = QColor(0xE3, 0x68, 0x62),
			};
		}
		return {
			.title = RuEn("Пользовательский разделитель", "Custom divider"),
			.subtitle = RuEn(
				"Свободный разделитель, который можно переставлять и удалять.",
				"Manual divider that can be moved or removed."),
			.glyph = u"="_q,
			.icon = nullptr,
			.color = QColor(0x35, 0xC3, 0x8F),
		};
	}

	const auto make = [&](const char *ruTitle,
			const char *enTitle,
			const char *ruSubtitle,
			const char *enSubtitle,
			const char *glyph,
			QColor color) {
		return EntryDescriptor{
			.title = RuEn(ruTitle, enTitle),
			.subtitle = RuEn(ruSubtitle, enSubtitle),
			.glyph = QString::fromLatin1(glyph),
			.icon = ThreeDotsActionIcon(entry.id),
			.color = color,
		};
	};

	if (entry.id == QString::fromLatin1(Id::NewWindow)) {
		return make("Новое окно", "New window", "Открывает чат в отдельном окне.", "Opens the chat in a separate window.", "N", QColor(0x4F, 0x8D, 0xFF));
	} else if (entry.id == QString::fromLatin1(Id::ToggleMute)) {
		return make("Уведомления", "Notifications", "Быстрое управление звуком и уведомлениями.", "Quick mute and notification controls.", "M", QColor(0x6C, 0x7A, 0xF9));
	} else if (entry.id == QString::fromLatin1(Id::TogglePin)) {
		return make("Закрепление", "Pin", "Закрепить или открепить чат.", "Pins or unpins the chat.", "P", QColor(0xF0, 0xA5, 0x2A));
	} else if (entry.id == QString::fromLatin1(Id::ToggleUnreadMark)) {
		return make("Метка непрочитанного", "Unread mark", "Переключает прочитано / непрочитано.", "Toggles read or unread state.", "U", QColor(0x2D, 0xC8, 0xB3));
	} else if (entry.id == QString::fromLatin1(Id::ToggleArchive)) {
		return make("Архив", "Archive", "Быстро отправляет чат в архив и обратно.", "Moves the chat to or from the archive.", "A", QColor(0x58, 0xC8, 0x66));
	} else if (entry.id == QString::fromLatin1(Id::CreateTopic)) {
		return make("Создать тему", "Create topic", "Создаёт новую тему форума.", "Creates a new forum topic.", "T", QColor(0x31, 0xB0, 0xE7));
	} else if (entry.id == QString::fromLatin1(Id::Info)) {
		return make("О чате", "About chat", "Открывает профиль чата, канала или пользователя.", "Opens the profile for the current chat, channel, or user.", "I", QColor(0x4F, 0x8D, 0xFF));
	} else if (entry.id == QString::fromLatin1(Id::ViewAsMessages)) {
		return make("Как сообщения", "View as messages", "Переключает форум в обычный список сообщений.", "Switches the forum to the messages view.", "M", QColor(0x5A, 0xAE, 0xF5));
	} else if (entry.id == QString::fromLatin1(Id::ViewAsTopics)) {
		return make("Как темы", "View as topics", "Переключает чат в режим тем.", "Switches the chat into the topics view.", "T", QColor(0x5A, 0xAE, 0xF5));
	} else if (entry.id == QString::fromLatin1(Id::SearchTopics)) {
		return make("Поиск по темам", "Search topics", "Открывает поиск внутри тем форума.", "Opens search inside forum topics.", "S", QColor(0x2D, 0xC8, 0xB3));
	} else if (entry.id == QString::fromLatin1(Id::ManageChat)) {
		return make("Управление чатом", "Manage chat", "Открывает настройки и управление чатом.", "Opens chat management and settings.", "M", QColor(0xF0, 0x9A, 0x36));
	} else if (entry.id == QString::fromLatin1(Id::SupportInfo)) {
		return make("Support info", "Support info", "Служебное действие для support-аккаунтов.", "Support-only info action.", "S", QColor(0x8B, 0x7A, 0xF6));
	} else if (entry.id == QString::fromLatin1(Id::StoryArchive)) {
		return make("Архив историй", "Stories archive", "Открывает архив историй канала.", "Opens the channel stories archive.", "S", QColor(0xE8, 0x7B, 0xB4));
	} else if (entry.id == QString::fromLatin1(Id::BoostChat)) {
		return make("Бусты", "Boosts", "Переход к экрану бустов.", "Opens the boosts screen.", "B", QColor(0xF2, 0xA4, 0x29));
	} else if (entry.id == QString::fromLatin1(Id::VideoChat)) {
		return make("Видеочат", "Video chat", "Запуск или управление групповым звонком.", "Starts or manages the group call.", "V", QColor(0x4F, 0x8D, 0xFF));
	} else if (entry.id == QString::fromLatin1(Id::CreatePoll)) {
		return make("Создать опрос", "Create poll", "Открывает создание нового опроса.", "Opens the poll creation flow.", "P", QColor(0x58, 0xC8, 0x66));
	} else if (entry.id == QString::fromLatin1(Id::CreateTodoList)) {
		return make("Создать To-Do", "Create to-do", "Открывает создание нового списка задач.", "Opens the to-do list creation flow.", "T", QColor(0x35, 0xC3, 0x8F));
	} else if (entry.id == QString::fromLatin1(Id::ThemeEdit)) {
		return make("Тема чата", "Chat theme", "Переход к оформлению чата.", "Opens chat theme settings.", "T", QColor(0x6C, 0x7A, 0xF9));
	} else if (entry.id == QString::fromLatin1(Id::ViewDiscussion)) {
		return make("Обсуждение", "Discussion", "Переход к связанному обсуждению канала.", "Opens the linked discussion chat.", "D", QColor(0x31, 0xB0, 0xE7));
	} else if (entry.id == QString::fromLatin1(Id::DirectMessages)) {
		return make("Личные сообщения", "Direct messages", "Открывает monoforum / direct messages.", "Opens the direct messages monoforum.", "D", QColor(0x35, 0xC3, 0x8F));
	} else if (entry.id == QString::fromLatin1(Id::ExportChat)) {
		return make("Экспорт чата", "Export chat", "Экспортирует историю чата или темы.", "Exports the chat or topic history.", "E", QColor(0x4A, 0xC6, 0x7A));
	} else if (entry.id == QString::fromLatin1(Id::Translate)) {
		return make("Перевести чат", "Translate chat", "Включает перевод текущего чата.", "Turns on chat translation.", "T", QColor(0x2D, 0xC8, 0xB3));
	} else if (entry.id == QString::fromLatin1(Id::DeletedMessages)) {
		return make("Удалённые сообщения", "Deleted messages", "Открывает журнал удалённых сообщений.", "Opens the deleted messages journal.", "D", QColor(0xF0, 0x9A, 0x36));
	} else if (entry.id == QString::fromLatin1(Id::Report)) {
		return make("Пожаловаться", "Report", "Отправляет жалобу на чат или канал.", "Reports the current chat or channel.", "R", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::ClearHistory)) {
		return make("Очистить историю", "Clear history", "Удаляет историю сообщений.", "Clears the message history.", "C", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::DeleteChat)) {
		return make("Удалить чат", "Delete chat", "Удаляет диалог или очищает его с выходом.", "Deletes the chat or clears it and exits.", "D", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::LeaveChat)) {
		return make("Покинуть чат", "Leave chat", "Выходит из группы или канала.", "Leaves the group or channel.", "L", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::JoinChat)) {
		return make("Вступить", "Join chat", "Вступает в канал или группу.", "Joins the current channel or group.", "J", QColor(0x4A, 0xC6, 0x7A));
	} else if (entry.id == QString::fromLatin1(Id::NewContact)) {
		return make("Добавить в контакты", "Add contact", "Создаёт новый контакт.", "Adds the peer to contacts.", "C", QColor(0x4A, 0xC6, 0x7A));
	} else if (entry.id == QString::fromLatin1(Id::ShareContact)) {
		return make("Поделиться контактом", "Share contact", "Открывает отправку контакта.", "Shares the contact in another chat.", "S", QColor(0x35, 0xC3, 0x8F));
	} else if (entry.id == QString::fromLatin1(Id::EditContact)) {
		return make("Изменить контакт", "Edit contact", "Редактирует карточку контакта.", "Edits the saved contact.", "E", QColor(0xF0, 0x9A, 0x36));
	} else if (entry.id == QString::fromLatin1(Id::BotToGroup)) {
		return make("Добавить бота", "Add bot", "Добавляет бота в группу или канал.", "Adds the bot to another chat.", "B", QColor(0x6C, 0x7A, 0xF9));
	} else if (entry.id == QString::fromLatin1(Id::NewMembers)) {
		return make("Добавить участников", "Add members", "Открывает приглашение новых участников.", "Invites new members.", "U", QColor(0x4A, 0xC6, 0x7A));
	} else if (entry.id == QString::fromLatin1(Id::DeleteContact)) {
		return make("Удалить контакт", "Delete contact", "Удаляет пользователя из контактов.", "Removes the user from contacts.", "D", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::DeleteTopic)) {
		return make("Удалить тему", "Delete topic", "Удаляет текущую тему.", "Deletes the current topic.", "D", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::TopicLink)) {
		return make("Ссылка на тему", "Topic link", "Копирует ссылку на тему.", "Copies the topic link.", "L", QColor(0x31, 0xB0, 0xE7));
	} else if (entry.id == QString::fromLatin1(Id::ManageTopic)) {
		return make("Изменить тему", "Manage topic", "Открывает редактирование темы.", "Opens topic editing.", "T", QColor(0xF0, 0x9A, 0x36));
	} else if (entry.id == QString::fromLatin1(Id::ToggleTopicClosed)) {
		return make("Закрыть тему", "Toggle topic", "Открывает или закрывает тему.", "Closes or reopens the topic.", "T", QColor(0xF0, 0x9A, 0x36));
	} else if (entry.id == QString::fromLatin1(Id::SendGift)) {
		return make("Подарок", "Send gift", "Открывает отправку подарка.", "Opens the gift flow.", "G", QColor(0xE8, 0x7B, 0xB4));
	} else if (entry.id == QString::fromLatin1(Id::ViewStatistics)) {
		return make("Статистика", "Statistics", "Переход к статистике и бустам.", "Opens statistics and boosts.", "S", QColor(0x31, 0xB0, 0xE7));
	} else if (entry.id == QString::fromLatin1(Id::ToggleFolder)) {
		return make("Папки", "Folders", "Открывает добавление чата в папки.", "Opens the add-to-folder menu.", "F", QColor(0x35, 0xC3, 0x8F));
	} else if (entry.id == QString::fromLatin1(Id::BlockUser)) {
		return make("Блокировка", "Block user", "Блокирует или разблокирует пользователя.", "Blocks or unblocks the user.", "B", QColor(0xE3, 0x68, 0x62));
	} else if (entry.id == QString::fromLatin1(Id::HidePromotion)) {
		return make("Скрыть промо", "Hide promotion", "Убирает текущий промо-блок.", "Hides the current promo block.", "H", QColor(0x8A, 0x93, 0xA3));
	} else if (entry.id == QString::fromLatin1(Id::SetPersonalChannel)) {
		return make("Личный канал", "Personal channel", "Привязывает личный канал.", "Sets the personal channel.", "C", QColor(0x6C, 0x7A, 0xF9));
	} else if (entry.id == QString::fromLatin1(Id::Ttl)) {
		return make("Автоудаление", "Auto-delete", "Настраивает TTL сообщений.", "Configures message auto-delete.", "T", QColor(0xF0, 0x9A, 0x36));
	}

	return {
		.title = HumanizePeerMenuId(entry.id),
		.subtitle = RuEn(
			"Сохранён в раскладке backend three_dots и будет оставлен как есть.",
			"Persisted in the three_dots backend layout and kept as-is."),
		.glyph = entry.id.left(1).toUpper(),
		.icon = ThreeDotsActionIcon(entry.id),
		.color = QColor(0x99, 0xA1, 0xAD),
	};
}

class ThreeDotsMenuEditorState final {
public:
	ThreeDotsMenuEditorState()
	: _entries(Menu::Customization::LoadThreeDotMenuLayout(
		DefaultThreeDotsMenuLayout())) {
		normalizeSelectedEntry();
	}

	[[nodiscard]] const std::vector<Menu::Customization::PeerMenuEntry> &entries() const {
		return _entries;
	}

	[[nodiscard]] int selectedEntryIndex() const {
		if (_selectedEntryId.isEmpty()) {
			return -1;
		}
		for (auto i = 0; i != int(_entries.size()); ++i) {
			if (_entries[i].id == _selectedEntryId) {
				return i;
			}
		}
		return -1;
	}

	void setSelectedEntryIndex(int index) {
		const auto updated = hasIndex(index) ? _entries[index].id : QString();
		if (_selectedEntryId == updated) {
			return;
		}
		_selectedEntryId = updated;
		_changes.fire({});
	}

	[[nodiscard]] QString layoutPath() const {
		return Menu::Customization::SideMenuLayoutPath();
	}

	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

	[[nodiscard]] bool resetToDefaults() {
		return applyEntries(DefaultThreeDotsMenuLayout());
	}

	[[nodiscard]] bool toggleVisible(int index) {
		if (!hasIndex(index)) {
			return false;
		}
		auto updated = _entries;
		updated[index].visible = !updated[index].visible;
		return applyEntries(updated);
	}

	[[nodiscard]] bool restoreEntry(int index) {
		if (!hasIndex(index)) {
			return false;
		} else if (_entries[index].visible) {
			setSelectedEntryIndex(index);
			return true;
		}
		auto updated = _entries;
		updated[index].visible = true;
		_selectedEntryId = updated[index].id;
		return applyEntries(updated);
	}

	[[nodiscard]] bool moveVisibleEntry(int fromVisible, int toVisible) {
		auto visibleIndexes = std::vector<int>();
		auto visibleEntries = std::vector<Menu::Customization::PeerMenuEntry>();
		for (auto i = 0; i != int(_entries.size()); ++i) {
			if (!_entries[i].visible) {
				continue;
			}
			visibleIndexes.push_back(i);
			visibleEntries.push_back(_entries[i]);
		}
		if ((fromVisible < 0) || (fromVisible >= int(visibleEntries.size()))) {
			return false;
		}
		const auto maxTarget = std::max(int(visibleEntries.size()) - 1, 0);
		const auto insertAt = std::clamp(toVisible, 0, maxTarget);
		if (fromVisible == insertAt) {
			return true;
		}
		auto reordered = visibleEntries;
		const auto moved = reordered[fromVisible];
		reordered.erase(reordered.begin() + fromVisible);
		reordered.insert(reordered.begin() + insertAt, moved);
		auto updated = _entries;
		for (auto i = 0; i != int(visibleIndexes.size()); ++i) {
			updated[visibleIndexes[i]] = reordered[i];
		}
		return applyEntries(updated);
	}

	[[nodiscard]] int addCustomSeparatorAfter(int index) {
		auto updated = _entries;
		const auto insertAt = std::clamp(index + 1, 0, int(updated.size()));
		updated.insert(
			updated.begin() + insertAt,
			Menu::Customization::PeerMenuEntry{
				NewCustomSeparatorId(),
				true,
				true,
			});
		_selectedEntryId = updated[insertAt].id;
		return applyEntries(updated) ? insertAt : -1;
	}

	[[nodiscard]] bool removeCustomSeparator(int index) {
		if (!hasIndex(index) || !isCustomSeparator(_entries[index])) {
			return false;
		}
		auto updated = _entries;
		updated.erase(updated.begin() + index);
		return applyEntries(updated);
	}

	[[nodiscard]] bool restoreAllHidden() {
		auto updated = _entries;
		auto changed = false;
		for (auto &entry : updated) {
			if (!entry.visible) {
				entry.visible = true;
				changed = true;
			}
		}
		return changed ? applyEntries(updated) : true;
	}

private:
	[[nodiscard]] bool applyEntries(
			const std::vector<Menu::Customization::PeerMenuEntry> &updated) {
		if (!Menu::Customization::SaveThreeDotMenuLayout(updated)) {
			return false;
		}
		_entries = updated;
		normalizeSelectedEntry();
		_changes.fire({});
		return true;
	}

	void normalizeSelectedEntry() {
		if (selectedEntryIndex() >= 0) {
			return;
		}
		for (const auto &entry : _entries) {
			if (!entry.visible) {
				continue;
			}
			_selectedEntryId = entry.id;
			return;
		}
		_selectedEntryId = _entries.empty() ? QString() : _entries.front().id;
	}

	[[nodiscard]] bool hasIndex(int index) const {
		return (index >= 0) && (index < int(_entries.size()));
	}

	[[nodiscard]] static bool isCustomSeparator(
			const Menu::Customization::PeerMenuEntry &entry) {
		return entry.separator && IsCustomSeparatorId(entry.id);
	}

	std::vector<Menu::Customization::PeerMenuEntry> _entries;
	QString _selectedEntryId;
	mutable rpl::event_stream<> _changes;
};

class ThreeDotsMenuEntryList final : public Ui::RpWidget {
public:
	ThreeDotsMenuEntryList(
		QWidget *parent,
		std::shared_ptr<ThreeDotsMenuEditorState> state)
	: RpWidget(parent)
	, _state(std::move(state)) {
		setMouseTracking(true);
		const auto refreshFromState = [=] {
			const auto selected = _state->selectedEntryIndex();
			if ((selected >= 0)
				&& (selected < int(_state->entries().size()))
				&& _state->entries()[selected].visible) {
				_selected = selected;
			}
			clampSelection();
			update();
			updateGeometry();
		};
		refreshFromState();
		_state->changes() | rpl::on_next([=] {
			refreshFromState();
		}, lifetime());
	}

	[[nodiscard]] int selectedIndex() const {
		return _selected;
	}

	void setSelectedIndex(int index) {
		_selected = index;
		clampSelection();
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto visibleCount = std::max(int(visibleEntryIndexes().size()), 1);
		const auto base = 18 + (visibleCount * (kRowHeight + kRowGap));
		return base + hiddenSectionHeight(newWidth);
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto visible = visibleEntryIndexes();
		if (visible.empty()) {
			const auto emptyRect = QRect(0, 8, width(), kRowHeight + 8);
			p.setPen(QPen(ThemedRowBorder(false, false)));
			p.setBrush(st::boxBg);
			p.drawRoundedRect(emptyRect, kRowRadius, kRowRadius);
			p.setPen(st::windowFg);
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 12, -18, -18),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Все пункты меню на 3 точки сейчас скрыты",
					"All three-dots actions are hidden right now"));
			p.setPen(st::windowSubTextFg);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 34, -18, -12),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Ниже можно вернуть любой скрытый пункт одним нажатием или сразу восстановить всё меню.",
					"You can restore any hidden action below with one click or bring back the whole menu at once."));
		} else {
			for (const auto index : visible) {
				if (index == _draggingRow) {
					continue;
				}
				paintRow(
					p,
					index,
					rowRect(index),
					(index == _hoveredRow),
					(index == _selected),
					false);
			}
		}

		if (_draggingRow >= 0) {
			p.setPen(QPen(st::windowBgActive->c, 3));
			const auto y = insertionLineY();
			p.drawLine(QPoint(8, y), QPoint(width() - 8, y));
			paintRow(
				p,
				_draggingRow,
				floatingRowRect(),
				false,
				true,
				true);
		}

		const auto chips = hiddenChips(width());
		if (!chips.empty()) {
			const auto hiddenCount = std::count_if(
				_state->entries().begin(),
				_state->entries().end(),
				[](const auto &entry) { return !entry.visible; });
			p.setPen(st::windowSubTextFg);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(0, hiddenSectionTop(), width(), 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				RuEn(
					"Скрытые пункты (%1)",
					"Hidden actions (%1)").arg(hiddenCount));
			for (const auto &chip : chips) {
				const auto hovered = chip.restoreAll
					? _hoveredRestoreAll
					: (chip.index == _hoveredHiddenEntry);
				const auto fillColor = chip.restoreAll
					? ThemedButtonFill(hovered, true)
					: anim::with_alpha(
						ThemedDescriptorAccent(chip.color, hovered),
						hovered ? 0.22 : 0.12);
				p.setPen(Qt::NoPen);
				p.setBrush(fillColor);
				p.drawRoundedRect(chip.rect, 14, 14);
				p.setPen(chip.restoreAll
					? ThemedButtonText(true)
					: (hovered ? st::windowFgActive->c : st::windowFg->c));
				p.setFont(st::normalFont->f);
				p.drawText(
					chip.rect.adjusted(12, 0, -12, 0),
					Qt::AlignLeft | Qt::AlignVCenter,
					chip.label);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto point = e->pos();
		if (_draggingRow >= 0) {
			_dragCurrentY = point.y();
			updateDragTarget(point.y());
			_hoveredRow = -1;
			_hoveredKind = ActionKind::None;
			_hoveredOnHandle = false;
			_hoveredHiddenEntry = -1;
			_hoveredRestoreAll = false;
			update();
			return;
		}
		if ((_pressedRow >= 0)
			&& _pressedOnHandle
			&& !_pressedOnButton
			&& ((point - _pressPoint).manhattanLength() >= 8)) {
			startDrag(point);
			return;
		}
		updateHover(point);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto point = e->pos();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;

		for (const auto &chip : hiddenChips(width())) {
			if (!chip.rect.contains(point)) {
				continue;
			}
			if (chip.restoreAll) {
				_state->restoreAllHidden();
				clampSelection();
			} else if (_state->restoreEntry(chip.index)) {
				_selected = chip.index;
				clampSelection();
			}
			updateHover(point);
			return;
		}

		for (const auto index : visibleEntryIndexes()) {
			const auto row = rowRect(index);
			if (!row.contains(point)) {
				continue;
			}
			_selected = index;
			_state->setSelectedEntryIndex(_selected);
			_pressPoint = point;
			const auto visibleRow = visibleRowForEntry(index);
			_dragGrabOffsetY = point.y() - baseRowRect(visibleRow).top();
			_pressedOnHandle = dragHandleRect(row).contains(point);
			for (const auto &button : actionButtonsForRow(index)) {
				if (!button.enabled || !button.rect.contains(point)) {
					continue;
				}
				_pressedOnButton = true;
				handleAction(index, button.kind);
				updateHover(point);
				return;
			}
			_pressedRow = index;
			updateHover(point);
			update();
			return;
		}
		updateHover(point);
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		if (_draggingRow >= 0) {
			finishDrag();
			return;
		}
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
		updateHover(e->pos());
	}

	void leaveEventHook(QEvent *e) override {
		Q_UNUSED(e);
		if (_draggingRow >= 0) {
			return;
		}
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredOnHandle = false;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		unsetCursor();
		update();
	}

private:
	enum class ActionKind {
		None,
		ToggleVisible,
		DeleteCustomSeparator,
	};

	struct ActionButton {
		QRect rect;
		QString label;
		ActionKind kind = ActionKind::None;
		bool enabled = true;
	};

	struct HiddenChip {
		QRect rect;
		int index = -1;
		QString label;
		QColor color;
		bool restoreAll = false;
	};

	[[nodiscard]] std::vector<int> visibleEntryIndexes() const {
		auto result = std::vector<int>();
		for (auto i = 0; i != int(_state->entries().size()); ++i) {
			if (_state->entries()[i].visible) {
				result.push_back(i);
			}
		}
		return result;
	}

	[[nodiscard]] QRect baseRowRect(int visibleRow) const {
		return QRect(
			0,
			8 + visibleRow * (kRowHeight + kRowGap),
			width(),
			kRowHeight);
	}

	[[nodiscard]] int visibleRowForEntry(int index) const {
		const auto visible = visibleEntryIndexes();
		const auto i = std::find(visible.begin(), visible.end(), index);
		return (i == visible.end()) ? -1 : int(i - visible.begin());
	}

	[[nodiscard]] QRect rowRectByVisibleRow(int visibleRow) const {
		auto result = baseRowRect(visibleRow);
		if ((_draggingVisibleRow < 0)
			|| (_dragTargetIndex < 0)
			|| (visibleRow == _draggingVisibleRow)) {
			return result;
		}
		const auto delta = kRowHeight + kRowGap;
		if (_draggingVisibleRow < _dragTargetIndex) {
			if ((visibleRow > _draggingVisibleRow) && (visibleRow <= _dragTargetIndex)) {
				result.translate(0, -delta);
			}
		} else if (_draggingVisibleRow > _dragTargetIndex) {
			if ((visibleRow >= _dragTargetIndex) && (visibleRow < _draggingVisibleRow)) {
				result.translate(0, delta);
			}
		}
		return result;
	}

	[[nodiscard]] QRect rowRect(int index) const {
		const auto visibleRow = visibleRowForEntry(index);
		return (visibleRow >= 0) ? rowRectByVisibleRow(visibleRow) : QRect();
	}

	[[nodiscard]] QRect floatingRowRect() const {
		auto result = baseRowRect(_draggingVisibleRow);
		result.moveTop(_dragCurrentY - _dragGrabOffsetY);
		return result;
	}

	[[nodiscard]] QRect dragHandleRect(const QRect &row) const {
		return QRect(row.left() + 10, row.top() + 14, 18, row.height() - 28);
	}

	[[nodiscard]] int hiddenSectionTop() const {
		const auto visibleCount = std::max(int(visibleEntryIndexes().size()), 1);
		return 18 + (visibleCount * (kRowHeight + kRowGap)) + 4;
	}

	[[nodiscard]] std::vector<HiddenChip> hiddenChips(int availableWidth) const {
		auto result = std::vector<HiddenChip>();
		const auto usableWidth = std::max(availableWidth, 120);
		const auto fm = QFontMetrics(st::normalFont->f);
		auto pushChip = [&](int index, QString label, QColor color, bool restoreAll) {
			label = fm.elidedText(label, Qt::ElideRight, 170);
			auto chipWidth = std::min(
				std::max(82, fm.horizontalAdvance(label) + 24),
				usableWidth);
			auto x = 0;
			auto y = hiddenSectionTop() + 24;
			if (!result.empty()) {
				const auto &last = result.back();
				x = last.rect.right() + 9;
				y = last.rect.top();
				if (x + chipWidth > usableWidth) {
					x = 0;
					y += 36;
				}
			}
			result.push_back(HiddenChip{
				.rect = QRect(x, y, chipWidth, 28),
				.index = index,
				.label = std::move(label),
				.color = color,
				.restoreAll = restoreAll,
			});
		};
		auto hiddenIndexes = std::vector<int>();
		for (auto i = 0; i != int(_state->entries().size()); ++i) {
			if (!_state->entries()[i].visible) {
				hiddenIndexes.push_back(i);
			}
		}
		if (hiddenIndexes.empty()) {
			return result;
		}
		pushChip(
			-1,
			RuEn("Вернуть всё", "Restore all"),
			st::windowBgActive->c,
			true);
		for (const auto index : hiddenIndexes) {
			const auto meta = DescribeThreeDotsEntry(_state->entries()[index]);
			pushChip(index, meta.title, meta.color, false);
		}
		return result;
	}

	[[nodiscard]] int hiddenSectionHeight(int availableWidth) const {
		const auto chips = hiddenChips(availableWidth);
		if (chips.empty()) {
			return 0;
		}
		auto bottom = hiddenSectionTop() + 18;
		for (const auto &chip : chips) {
			bottom = std::max(bottom, chip.rect.bottom());
		}
		return (bottom - hiddenSectionTop()) + 16;
	}

	[[nodiscard]] std::vector<ActionButton> actionButtonsForRow(int index) const {
		const auto &entries = _state->entries();
		if ((index < 0) || (index >= int(entries.size())) || !entries[index].visible) {
			return {};
		}
		const auto row = rowRect(index);
		const auto &entry = entries[index];
		const auto fm = QFontMetrics(st::normalFont->f);
		const auto height = 28;
		auto right = row.right() - kRowPadding;
		auto result = std::vector<ActionButton>();
		const auto push = [&](QString label, ActionKind kind, bool enabled) {
			const auto buttonWidth = std::max(44, fm.horizontalAdvance(label) + 22);
			right -= buttonWidth;
			result.push_back(ActionButton{
				.rect = QRect(right, row.top() + 18, buttonWidth, height),
				.label = std::move(label),
				.kind = kind,
				.enabled = enabled,
			});
			right -= 8;
		};

		if (entry.separator && IsCustomSeparatorId(entry.id)) {
			push(RuEn("Удалить", "Delete"), ActionKind::DeleteCustomSeparator, true);
		}
		push(RuEn("Скрыть", "Hide"), ActionKind::ToggleVisible, true);
		std::reverse(result.begin(), result.end());
		return result;
	}

	void paintActionButton(QPainter &p, const ActionButton &button) const {
		const auto hovered = (_hoveredRow >= 0)
			&& (button.kind == _hoveredKind)
			&& button.rect.intersects(rowRect(_hoveredRow));
		const auto active = button.enabled;
		const auto destructive = (button.kind == ActionKind::DeleteCustomSeparator);
		p.setPen(Qt::NoPen);
		p.setBrush(destructive
			? ThemedDestructiveButtonFill(hovered, active)
			: ThemedButtonFill(hovered, active));
		p.drawRoundedRect(button.rect, 14, 14);
		p.setPen(destructive
			? ThemedDestructiveButtonText(active)
			: ThemedButtonText(active));
		p.setFont(st::normalFont->f);
		p.drawText(button.rect, Qt::AlignCenter, button.label);
	}

	void paintDragHandle(Painter &p, const QRect &row, bool active) const {
		const auto handle = dragHandleRect(row);
		p.setPen(Qt::NoPen);
		p.setBrush(active
			? st::windowBgActive->c
			: anim::with_alpha(st::windowSubTextFg->c, 0.65));
		for (auto column = 0; column != 2; ++column) {
			for (auto line = 0; line != 3; ++line) {
				p.drawEllipse(
					QRect(
						handle.left() + (column * 6),
						handle.top() + 2 + (line * 8),
						3,
						3));
			}
		}
	}

	void paintRow(
			Painter &p,
			int index,
			const QRect &row,
			bool hovered,
			bool selected,
			bool floating) const {
		const auto &entry = _state->entries()[index];
		const auto meta = DescribeThreeDotsEntry(entry);
		const auto accent = ThemedDescriptorAccent(
			meta.color,
			hovered || selected || floating);
		p.setPen(QPen(ThemedRowBorder(hovered, selected)));
		p.setBrush(ThemedRowFill(hovered, selected, floating));
		p.drawRoundedRect(row, kRowRadius, kRowRadius);

		paintDragHandle(p, row, hovered || selected || floating);
		p.setBrush(accent);
		p.drawEllipse(QRect(row.left() + 36, row.top() + 14, 36, 36));
		if (meta.icon) {
			meta.icon->paintInCenter(
				p,
				QRect(row.left() + 36, row.top() + 14, 36, 36));
		} else {
			p.setPen(Qt::white);
			p.setFont(st::semiboldFont->f);
			p.drawText(
				QRect(row.left() + 36, row.top() + 14, 36, 36),
				Qt::AlignCenter,
				meta.glyph.left(1));
		}

		p.setPen(st::windowFg);
		p.setFont(st::semiboldTextStyle.font->f);
		p.drawText(
			QRect(row.left() + 84, row.top() + 12, row.width() - 242, 20),
			Qt::AlignLeft | Qt::AlignVCenter,
			meta.title);

		p.setPen(st::windowSubTextFg);
		p.setFont(st::defaultTextStyle.font->f);
		const auto subtitle = QFontMetrics(st::defaultTextStyle.font->f).elidedText(
			meta.subtitle,
			Qt::ElideRight,
			row.width() - 242);
		p.drawText(
			QRect(row.left() + 84, row.top() + 34, row.width() - 242, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			subtitle);

		if (!floating) {
			for (const auto &button : actionButtonsForRow(index)) {
				paintActionButton(p, button);
			}
		}
	}

	[[nodiscard]] int insertionLineY() const {
		if ((_draggingVisibleRow < 0) || (_dragTargetIndex < 0)) {
			return 0;
		}
		const auto visible = visibleEntryIndexes();
		auto slot = 0;
		for (auto visibleRow = 0; visibleRow != int(visible.size()); ++visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			if (slot == _dragTargetIndex) {
				return rowRectByVisibleRow(visibleRow).top() - (kRowGap / 2);
			}
			++slot;
		}
		for (auto visibleRow = int(visible.size()) - 1; visibleRow >= 0; --visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			return rowRectByVisibleRow(visibleRow).bottom() + (kRowGap / 2) + 1;
		}
		return baseRowRect(0).center().y();
	}

	void updateDragTarget(int y) {
		if (_draggingVisibleRow < 0) {
			return;
		}
		const auto visible = visibleEntryIndexes();
		auto slot = 0;
		for (auto visibleRow = 0; visibleRow != int(visible.size()); ++visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			if (y < rowRectByVisibleRow(visibleRow).center().y()) {
				_dragTargetIndex = slot;
				return;
			}
			++slot;
		}
		_dragTargetIndex = slot;
	}

	void startDrag(QPoint point) {
		if (_pressedRow < 0) {
			return;
		}
		_draggingRow = _pressedRow;
		_draggingVisibleRow = visibleRowForEntry(_pressedRow);
		if (_draggingVisibleRow < 0) {
			return;
		}
		_dragTargetIndex = _draggingVisibleRow;
		_dragCurrentY = point.y();
		_dragGrabOffsetY = point.y() - baseRowRect(_draggingVisibleRow).top();
		_pressedRow = -1;
		_pressedOnButton = false;
		grabMouse();
		setCursor(Qt::ClosedHandCursor);
		updateDragTarget(point.y());
		update();
	}

	void finishDrag() {
		if (_draggingVisibleRow < 0) {
			return;
		}
		const auto maxTarget = std::max(int(visibleEntryIndexes().size()) - 1, 0);
		const auto target = std::clamp(_dragTargetIndex, 0, maxTarget);
		_state->moveVisibleEntry(_draggingVisibleRow, target);
		_selected = _state->selectedEntryIndex();
		_draggingRow = -1;
		_draggingVisibleRow = -1;
		_dragTargetIndex = -1;
		_dragCurrentY = 0;
		_dragGrabOffsetY = 0;
		releaseMouse();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredOnHandle = false;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		clampSelection();
		unsetCursor();
		update();
	}

	void updateHover(QPoint point) {
		auto newHiddenEntry = -1;
		auto newRestoreAll = false;
		for (const auto &chip : hiddenChips(width())) {
			if (!chip.rect.contains(point)) {
				continue;
			}
			newRestoreAll = chip.restoreAll;
			newHiddenEntry = chip.restoreAll ? -1 : chip.index;
			break;
		}

		auto newRow = -1;
		auto newKind = ActionKind::None;
		auto newOnHandle = false;
		if (!newRestoreAll && (newHiddenEntry < 0)) {
			for (const auto index : visibleEntryIndexes()) {
				const auto row = rowRect(index);
				if (!row.contains(point)) {
					continue;
				}
				newRow = index;
				newOnHandle = dragHandleRect(row).contains(point);
				for (const auto &button : actionButtonsForRow(index)) {
					if (button.enabled && button.rect.contains(point)) {
						newKind = button.kind;
						break;
					}
				}
				break;
			}
		}

		if ((newRow == _hoveredRow)
			&& (newKind == _hoveredKind)
			&& (newOnHandle == _hoveredOnHandle)
			&& (newHiddenEntry == _hoveredHiddenEntry)
			&& (newRestoreAll == _hoveredRestoreAll)) {
			return;
		}
		_hoveredRow = newRow;
		_hoveredKind = newKind;
		_hoveredOnHandle = newOnHandle;
		_hoveredHiddenEntry = newHiddenEntry;
		_hoveredRestoreAll = newRestoreAll;
		if (_hoveredRestoreAll
			|| (_hoveredHiddenEntry >= 0)
			|| ((_hoveredRow >= 0) && (_hoveredKind != ActionKind::None))) {
			setCursor(Qt::PointingHandCursor);
		} else if (_hoveredOnHandle) {
			setCursor(Qt::OpenHandCursor);
		} else {
			unsetCursor();
		}
		update();
	}

	void handleAction(int index, ActionKind kind) {
		auto changed = false;
		switch (kind) {
		case ActionKind::ToggleVisible:
			changed = _state->toggleVisible(index);
			break;
		case ActionKind::DeleteCustomSeparator:
			changed = _state->removeCustomSeparator(index);
			break;
		case ActionKind::None:
			break;
		}
		if (changed) {
			_selected = _state->selectedEntryIndex();
			clampSelection();
		} else {
			update();
		}
	}

	void clampSelection() {
		const auto visible = visibleEntryIndexes();
		if (visible.empty()) {
			_selected = -1;
			_state->setSelectedEntryIndex(-1);
			return;
		}
		if ((_selected < 0)
			|| (_selected >= int(_state->entries().size()))
			|| !_state->entries()[_selected].visible) {
			_selected = visible.front();
		}
		_state->setSelectedEntryIndex(_selected);
	}

	const std::shared_ptr<ThreeDotsMenuEditorState> _state;
	int _selected = -1;
	int _hoveredRow = -1;
	ActionKind _hoveredKind = ActionKind::None;
	bool _hoveredOnHandle = false;
	int _hoveredHiddenEntry = -1;
	bool _hoveredRestoreAll = false;
	int _pressedRow = -1;
	bool _pressedOnButton = false;
	bool _pressedOnHandle = false;
	QPoint _pressPoint;
	int _draggingRow = -1;
	int _draggingVisibleRow = -1;
	int _dragTargetIndex = -1;
	int _dragCurrentY = 0;
	int _dragGrabOffsetY = 0;
};

enum class ContextEditorLane {
	Menu,
	Strip,
};

[[nodiscard]] ContextEditorLane OtherContextLane(ContextEditorLane lane) {
	return (lane == ContextEditorLane::Strip)
		? ContextEditorLane::Menu
		: ContextEditorLane::Strip;
}

struct TagChip {
	QString text;
	QColor fill;
	QColor fg;
};

struct PaintedTagChip {
	TagChip chip;
	QRect rect;
};

struct ContextActionDescriptor {
	QString title;
	QString subtitle;
	QString glyph;
	const style::icon *icon = nullptr;
	QColor color;
};

[[nodiscard]] TagChip MakeTagChip(
		QString text,
		const QColor &seed,
		bool emphasized = false) {
	const auto fill = anim::with_alpha(
		ThemedDescriptorAccent(seed, emphasized),
		emphasized ? 0.22 : 0.14);
	const auto fg = emphasized
		? st::windowFgActive->c
		: BlendThemeColor(seed, st::windowFg->c, 0.42);
	return TagChip{
		.text = std::move(text),
		.fill = fill,
		.fg = fg,
	};
}

[[nodiscard]] std::vector<TagChip> ContextEntryTags(
		HistoryView::ContextMenuSurface surface,
		ContextEditorLane lane,
		const QString &id) {
	auto result = std::vector<TagChip>();
	result.push_back((lane == ContextEditorLane::Strip)
		? MakeTagChip(
			RuEn("Подменю", "Submenu"),
			QColor(0x35, 0xC3, 0x8F),
			true)
		: MakeTagChip(
			RuEn("Основное", "Main"),
			QColor(0x4F, 0x8D, 0xFF),
			true));
	result.push_back((surface == HistoryView::ContextMenuSurface::Selection)
		? MakeTagChip(
			RuEn("Только при выделении", "Selection only"),
			QColor(0x73, 0x62, 0xE8))
		: MakeTagChip(
			RuEn("Обычное", "Regular"),
			QColor(0x22, 0x9D, 0x68)));

	namespace Id = Menu::Customization::ContextMenuItemId;
	if ((id == QString::fromLatin1(Id::MessageSendNow))
		|| (id == QString::fromLatin1(Id::SelectionSendNow))
		|| (id == QString::fromLatin1(Id::MessageReschedule))) {
		result.push_back(MakeTagChip(
			RuEn("Отложенные", "Scheduled"),
			QColor(0xF0, 0x9A, 0x36)));
	} else if ((id == QString::fromLatin1(Id::LinkCopy))
		|| (id == QString::fromLatin1(Id::MessageCopyPostLink))) {
		result.push_back(MakeTagChip(
			RuEn("Ссылка", "Link"),
			QColor(0x31, 0xB0, 0xE7)));
	} else if (id == QString::fromLatin1(Id::SelectionDownloadFiles)) {
		result.push_back(MakeTagChip(
			RuEn("Файлы", "Files"),
			QColor(0xF1, 0xA4, 0x2B)));
	} else if ((id == QString::fromLatin1(Id::MessageTodoAdd))
		|| (id == QString::fromLatin1(Id::MessageTodoEdit))) {
		result.push_back(MakeTagChip(
			RuEn("To-Do", "To-do"),
			QColor(0x4A, 0xC6, 0x7A)));
	} else if (id == QString::fromLatin1(Id::MessageViewReplies)) {
		result.push_back(MakeTagChip(
			RuEn("Ответы", "Replies"),
			QColor(0x58, 0xC8, 0x66)));
	} else if (IsCustomSeparatorId(id)) {
		result.push_back(MakeTagChip(
			RuEn("Разделитель", "Divider"),
			QColor(0x8A, 0x93, 0xA3)));
	}
	return result;
}

[[nodiscard]] std::vector<PaintedTagChip> LayoutTagChips(
		const std::vector<TagChip> &tags,
		int right,
		int top,
		int minimumLeft) {
	auto result = std::vector<PaintedTagChip>();
	const auto metrics = QFontMetrics(st::normalFont->f);
	auto currentRight = right;
	for (const auto &tag : tags) {
		const auto width = std::max(64, metrics.horizontalAdvance(tag.text) + 18);
		const auto left = currentRight - width;
		if (left < minimumLeft) {
			break;
		}
		result.push_back(PaintedTagChip{
			.chip = tag,
			.rect = QRect(left, top, width, 20),
		});
		currentRight = left - 6;
	}
	std::reverse(result.begin(), result.end());
	return result;
}

void PaintTagChips(
		Painter &p,
		const std::vector<PaintedTagChip> &chips) {
	p.setFont(st::normalFont->f);
	for (const auto &tag : chips) {
		p.setPen(Qt::NoPen);
		p.setBrush(tag.chip.fill);
		p.drawRoundedRect(tag.rect, 10, 10);
		p.setPen(tag.chip.fg);
		p.drawText(tag.rect, Qt::AlignCenter, tag.chip.text);
	}
}

[[nodiscard]] const style::icon *ContextActionIcon(const QString &id) {
	namespace Id = Menu::Customization::ContextMenuItemId;
	if (id == QString::fromLatin1(Id::SelectionCopy)
		|| id == QString::fromLatin1(Id::MessageCopyText)
		|| id == QString::fromLatin1(Id::MessageCopyPostLink)
		|| id == QString::fromLatin1(Id::LinkCopy)
		|| id == QString::fromLatin1(Id::MessageCopyIdsTime)) {
		return &st::menuIconCopy;
	} else if (id == QString::fromLatin1(Id::SelectionTranslate)
		|| id == QString::fromLatin1(Id::MessageTranslate)) {
		return &st::menuIconTranslate;
	} else if (id == QString::fromLatin1(Id::SelectionSearch)) {
		return &st::menuIconSearch;
	} else if (id == QString::fromLatin1(Id::SelectionForward)
		|| id == QString::fromLatin1(Id::SelectionForwardWithoutAuthor)
		|| id == QString::fromLatin1(Id::MessageForward)
		|| id == QString::fromLatin1(Id::MessageForwardWithoutAuthor)) {
		return &st::menuIconForward;
	} else if (id == QString::fromLatin1(Id::SelectionForwardSaved)
		|| id == QString::fromLatin1(Id::MessageForwardSaved)) {
		return &st::menuIconFave;
	} else if (id == QString::fromLatin1(Id::SelectionSendNow)
		|| id == QString::fromLatin1(Id::MessageSendNow)) {
		return &st::menuIconSend;
	} else if (id == QString::fromLatin1(Id::SelectionDelete)
		|| id == QString::fromLatin1(Id::MessageDelete)) {
		return &st::menuIconDelete;
	} else if (id == QString::fromLatin1(Id::SelectionDownloadFiles)) {
		return &st::menuIconDownload;
	} else if (id == QString::fromLatin1(Id::SelectionClear)
		|| id == QString::fromLatin1(Id::MessageSelect)) {
		return &st::menuIconSelect;
	} else if (id == QString::fromLatin1(Id::MessageGoTo)) {
		return &st::menuIconShowInChat;
	} else if (id == QString::fromLatin1(Id::MessageViewReplies)) {
		return &st::menuIconViewReplies;
	} else if (id == QString::fromLatin1(Id::MessageReply)) {
		return &st::menuIconReply;
	} else if (id == QString::fromLatin1(Id::MessageTodoEdit)
		|| id == QString::fromLatin1(Id::MessageEdit)
		|| id == QString::fromLatin1(Id::MessageEditHistory)) {
		return &st::menuIconEdit;
	} else if (id == QString::fromLatin1(Id::MessageTodoAdd)) {
		return &st::menuIconAdd;
	} else if (id == QString::fromLatin1(Id::MessageFactcheck)) {
		return &st::menuIconFactcheck;
	} else if (id == QString::fromLatin1(Id::MessagePin)) {
		return &st::menuIconPin;
	} else if (id == QString::fromLatin1(Id::MessageReport)) {
		return &st::menuIconReport;
	} else if (id == QString::fromLatin1(Id::MessageReschedule)) {
		return &st::menuIconReschedule;
	}
	return nullptr;
}

[[nodiscard]] QString ContextSurfaceTitle(
		HistoryView::ContextMenuSurface surface) {
	return (surface == HistoryView::ContextMenuSurface::Selection)
		? RuEn(
			"Только при выделении",
			"Selection only")
		: RuEn(
			"Обычные сообщения",
			"Regular messages");
}

[[nodiscard]] QString ContextLaneTitle(ContextEditorLane lane) {
	return (lane == ContextEditorLane::Strip)
		? RuEn("Подменю", "Submenu")
		: RuEn("Основное", "Main");
}

[[nodiscard]] const std::vector<HistoryView::ContextMenuLayoutEntry> &ContextEntries(
		const HistoryView::ContextMenuCustomizationLayout &layout,
		HistoryView::ContextMenuSurface surface,
		ContextEditorLane lane) {
	if (surface == HistoryView::ContextMenuSurface::Selection) {
		return (lane == ContextEditorLane::Strip)
			? layout.selection.strip
			: layout.selection.menu;
	}
	return (lane == ContextEditorLane::Strip)
		? layout.message.strip
		: layout.message.menu;
}

[[nodiscard]] std::vector<HistoryView::ContextMenuLayoutEntry> &ContextEntries(
		HistoryView::ContextMenuCustomizationLayout &layout,
		HistoryView::ContextMenuSurface surface,
		ContextEditorLane lane) {
	if (surface == HistoryView::ContextMenuSurface::Selection) {
		return (lane == ContextEditorLane::Strip)
			? layout.selection.strip
			: layout.selection.menu;
	}
	return (lane == ContextEditorLane::Strip)
		? layout.message.strip
		: layout.message.menu;
}

[[nodiscard]] int VisibleEntryCount(
		const std::vector<HistoryView::ContextMenuLayoutEntry> &entries) {
	auto result = 0;
	for (const auto &entry : entries) {
		if (entry.visible) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] ContextActionDescriptor DescribeContextAction(const QString &id) {
	namespace Id = Menu::Customization::ContextMenuItemId;
	const auto make = [&](const char *ruTitle,
			const char *enTitle,
			const char *ruSubtitle,
			const char *enSubtitle,
			const char *glyph,
			QColor color) {
		return ContextActionDescriptor{
			.title = RuEn(ruTitle, enTitle),
			.subtitle = RuEn(ruSubtitle, enSubtitle),
			.glyph = QString::fromLatin1(glyph),
			.icon = ContextActionIcon(id),
			.color = color,
		};
	};

	if (IsCustomSeparatorId(id)) {
		return make(
			"Разделитель",
			"Divider",
			"Ручной разделитель для контекстного меню.",
			"Manual divider for the context menu.",
			"—",
			QColor(0x8A, 0x93, 0xA3));
	} else if (id == QString::fromLatin1(Id::SelectionCopy)) {
		return make(
			"Копировать выделение",
			"Copy selection",
			"Копирует выделенный текст или выбранные сообщения.",
			"Copies selected text or picked messages.",
			"C",
			QColor(0x4F, 0x8D, 0xFF));
	} else if (id == QString::fromLatin1(Id::SelectionTranslate)) {
		return make(
			"Перевести выделение",
			"Translate selection",
			"Открывает перевод выделенного текста.",
			"Opens a translation for the selected text.",
			"T",
			QColor(0x35, 0xC3, 0x8F));
	} else if (id == QString::fromLatin1(Id::SelectionSearch)) {
		return make(
			"Искать выделенное",
			"Search selection",
			"Открывает поиск по выделенному тексту.",
			"Searches the selected text on the web.",
			"S",
			QColor(0xF0, 0x9A, 0x36));
	} else if (id == QString::fromLatin1(Id::SelectionForward)
		|| id == QString::fromLatin1(Id::MessageForward)) {
		return make(
			"Переслать",
			"Forward",
			"Обычная пересылка в другой чат.",
			"Regular forward into another chat.",
			"F",
			QColor(0x31, 0xB0, 0xE7));
	} else if (id == QString::fromLatin1(Id::SelectionForwardWithoutAuthor)
		|| id == QString::fromLatin1(Id::MessageForwardWithoutAuthor)) {
		return make(
			"Переслать без автора",
			"Forward without author",
			"Ресенд без цитаты и указания автора.",
			"Resends without quote and without author.",
			"A",
			QColor(0x35, 0xC3, 0x8F));
	} else if (id == QString::fromLatin1(Id::SelectionForwardSaved)
		|| id == QString::fromLatin1(Id::MessageForwardSaved)) {
		return make(
			"В Избранное",
			"Forward to Saved",
			"Сразу пересылает в Saved Messages.",
			"Sends directly to Saved Messages.",
			"S",
			QColor(0x37, 0xA6, 0xF0));
	} else if (id == QString::fromLatin1(Id::SelectionSendNow)
		|| id == QString::fromLatin1(Id::MessageSendNow)) {
		return make(
			"Отправить сейчас",
			"Send now",
			"Мгновенная отправка для отложенных сообщений.",
			"Instant send for scheduled messages.",
			"N",
			QColor(0x39, 0xB9, 0x8D));
	} else if (id == QString::fromLatin1(Id::SelectionDelete)
		|| id == QString::fromLatin1(Id::MessageDelete)) {
		return make(
			"Удалить",
			"Delete",
			"Удаление сообщения или выбранных сообщений.",
			"Deletes the message or selected messages.",
			"D",
			QColor(0xD7, 0x59, 0x59));
	} else if (id == QString::fromLatin1(Id::SelectionDownloadFiles)) {
		return make(
			"Скачать файлы",
			"Download files",
			"Сохраняет файлы из выбранных сообщений.",
			"Saves files from selected messages.",
			"L",
			QColor(0xF1, 0xA4, 0x2B));
	} else if (id == QString::fromLatin1(Id::SelectionClear)) {
		return make(
			"Снять выделение",
			"Clear selection",
			"Выходит из режима выделения.",
			"Leaves the selection mode.",
			"X",
			QColor(0x8A, 0x93, 0xA3));
	} else if (id == QString::fromLatin1(Id::MessageGoTo)) {
		return make(
			"Перейти к сообщению",
			"Go to message",
			"Открывает исходное сообщение в чате.",
			"Opens the source message in chat.",
			"G",
			QColor(0x68, 0x79, 0xFF));
	} else if (id == QString::fromLatin1(Id::MessageViewReplies)) {
		return make(
			"Открыть ответы",
			"View replies",
			"Показывает ветку ответов или тему.",
			"Shows the replies thread or topic.",
			"R",
			QColor(0x58, 0xC8, 0x66));
	} else if (id == QString::fromLatin1(Id::MessageReply)) {
		return make(
			"Ответить",
			"Reply",
			"Обычный ответ или quote reply.",
			"Regular reply or quoted reply.",
			"R",
			QColor(0x31, 0xB0, 0xE7));
	} else if (id == QString::fromLatin1(Id::MessageTodoEdit)) {
		return make(
			"Изменить To-Do",
			"Edit to-do",
			"Редактирует todo-сообщение.",
			"Edits the to-do message.",
			"E",
			QColor(0xF0, 0x9A, 0x36));
	} else if (id == QString::fromLatin1(Id::MessageTodoAdd)) {
		return make(
			"Добавить задачу",
			"Add task",
			"Добавляет задачу в todo-list.",
			"Adds a task to the to-do list.",
			"+",
			QColor(0x4A, 0xC6, 0x7A));
	} else if (id == QString::fromLatin1(Id::MessageEdit)) {
		return make(
			"Изменить сообщение",
			"Edit message",
			"Запускает обычное редактирование.",
			"Starts regular message editing.",
			"E",
			QColor(0x4F, 0x8D, 0xFF));
	} else if (id == QString::fromLatin1(Id::MessageEditHistory)) {
		return make(
			"История правок",
			"Edit history",
			"Показывает локально сохранённые ревизии.",
			"Shows locally saved revisions.",
			"H",
			QColor(0x8A, 0x93, 0xA3));
	} else if (id == QString::fromLatin1(Id::MessageCopyIdsTime)) {
		return make(
			"Скопировать ID и время",
			"Copy IDs and time",
			"Копирует chat id, message id и timestamp.",
			"Copies chat id, message id and timestamp.",
			"I",
			QColor(0x73, 0x62, 0xE8));
	} else if (id == QString::fromLatin1(Id::MessageFactcheck)) {
		return make(
			"Фактчек",
			"Factcheck",
			"Редактирование fact-check блока.",
			"Edits the fact-check block.",
			"F",
			QColor(0x2D, 0xC8, 0xB3));
	} else if (id == QString::fromLatin1(Id::MessagePin)) {
		return make(
			"Закрепить",
			"Pin message",
			"Закрепляет или открепляет сообщение.",
			"Pins or unpins the message.",
			"P",
			QColor(0x5A, 0xAE, 0xF5));
	} else if (id == QString::fromLatin1(Id::MessageCopyPostLink)
		|| id == QString::fromLatin1(Id::LinkCopy)) {
		return make(
			"Скопировать ссылку",
			"Copy link",
			"Копирует ссылку на пост или выбранный URL.",
			"Copies a post link or the selected URL.",
			"L",
			QColor(0x37, 0xA6, 0xF0));
	} else if (id == QString::fromLatin1(Id::MessageCopyText)) {
		return make(
			"Копировать текст",
			"Copy text",
			"Копирует текст сообщения целиком.",
			"Copies the full message text.",
			"C",
			QColor(0x4F, 0x8D, 0xFF));
	} else if (id == QString::fromLatin1(Id::MessageTranslate)) {
		return make(
			"Перевести",
			"Translate",
			"Открывает перевод текста сообщения.",
			"Opens a translation for the message text.",
			"T",
			QColor(0x35, 0xC3, 0x8F));
	} else if (id == QString::fromLatin1(Id::MessageReport)) {
		return make(
			"Пожаловаться",
			"Report",
			"Жалоба на сообщение.",
			"Reports the message.",
			"!",
			QColor(0xD7, 0x59, 0x59));
	} else if (id == QString::fromLatin1(Id::MessageSelect)) {
		return make(
			"Выбрать",
			"Select",
			"Включает режим выделения сообщений.",
			"Enters message selection mode.",
			"S",
			QColor(0x8A, 0x93, 0xA3));
	} else if (id == QString::fromLatin1(Id::MessageReschedule)) {
		return make(
			"Перенести",
			"Reschedule",
			"Меняет время отложенной отправки.",
			"Changes the scheduled send time.",
			"R",
			QColor(0xF0, 0x9A, 0x36));
	}

	return make(
		"Неизвестное действие",
		"Unknown action",
		"Сохранено в layout-файле и будет оставлено как есть.",
		"Persisted in the layout file and kept as-is.",
		"?",
		QColor(0x99, 0xA1, 0xAD));
}

class ContextMenuEditorState final {
public:
	ContextMenuEditorState()
	: _layout(HistoryView::LoadContextMenuCustomizationLayout()) {
		normalizeDuplicates(_layout);
	}

	[[nodiscard]] const HistoryView::ContextMenuCustomizationLayout &layout() const {
		return _layout;
	}

	[[nodiscard]] const std::vector<HistoryView::ContextMenuLayoutEntry> &entries(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane) const {
		return ContextEntries(_layout, surface, lane);
	}

	[[nodiscard]] QString layoutPath() const {
		return HistoryView::ContextMenuCustomizationLayoutPath();
	}

	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

	[[nodiscard]] bool reloadFromDisk() {
		_layout = HistoryView::LoadContextMenuCustomizationLayout();
		normalizeDuplicates(_layout);
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool resetToDefaults() {
		return applyLayout(HistoryView::DefaultContextMenuCustomizationLayout());
	}

	[[nodiscard]] int addCustomSeparatorAfter(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) {
		if (lane != ContextEditorLane::Menu) {
			return -1;
		}
		auto updated = _layout;
		auto &entries = ContextEntries(updated, surface, lane);
		const auto insertAt = std::clamp(index + 1, 0, int(entries.size()));
		entries.insert(entries.begin() + insertAt, {
			.id = NewCustomSeparatorId(),
			.visible = true,
		});
		return applyLayout(updated) ? insertAt : -1;
	}

	[[nodiscard]] bool removeCustomSeparator(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) {
		if (!hasIndex(surface, lane, index)) {
			return false;
		}
		auto updated = _layout;
		auto &entries = ContextEntries(updated, surface, lane);
		if (!IsCustomSeparatorId(entries[index].id)) {
			return false;
		}
		entries.erase(entries.begin() + index);
		return applyLayout(updated);
	}

	[[nodiscard]] bool toggleVisible(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) {
		if (!hasIndex(surface, lane, index)) {
			return false;
		}
		auto updated = _layout;
		auto &entries = ContextEntries(updated, surface, lane);
		const auto turnOn = !entries[index].visible;
		if (turnOn
			&& lane == ContextEditorLane::Strip
			&& VisibleEntryCount(entries) >= 4) {
			return false;
		}
		entries[index].visible = turnOn;
		if (turnOn) {
			hideCounterpart(updated, surface, lane, entries[index].id);
		}
		return applyLayout(updated);
	}

	[[nodiscard]] bool canMoveToLane(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) const {
		if (!hasIndex(surface, lane, index)) {
			return false;
		}
		const auto &source = entries(surface, lane);
		if (IsCustomSeparatorId(source[index].id)) {
			return false;
		}
		const auto &target = entries(surface, OtherContextLane(lane));
		return findIndexById(target, source[index].id) >= 0;
	}

	[[nodiscard]] bool switchLane(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane fromLane,
			int index) {
		if (!canMoveToLane(surface, fromLane, index)) {
			return false;
		}
		auto updated = _layout;
		auto &source = ContextEntries(updated, surface, fromLane);
		const auto toLane = OtherContextLane(fromLane);
		auto &target = ContextEntries(updated, surface, toLane);
		const auto targetIndex = findIndexById(target, source[index].id);
		if (targetIndex < 0) {
			return false;
		}
		if ((toLane == ContextEditorLane::Strip)
			&& !target[targetIndex].visible
			&& (VisibleEntryCount(target) >= kContextStripPreviewLimit)) {
			return false;
		}
		source[index].visible = false;
		target[targetIndex].visible = true;
		hideCounterpart(updated, surface, toLane, target[targetIndex].id);
		return applyLayout(updated);
	}

	[[nodiscard]] bool moveUp(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) {
		if (!hasIndex(surface, lane, index) || (index <= 0)) {
			return false;
		}
		auto updated = _layout;
		auto &entries = ContextEntries(updated, surface, lane);
		std::swap(entries[index], entries[index - 1]);
		return applyLayout(updated);
	}

	[[nodiscard]] bool moveDown(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) {
		if (!hasIndex(surface, lane, index)) {
			return false;
		}
		auto updated = _layout;
		auto &entries = ContextEntries(updated, surface, lane);
		if (index + 1 >= int(entries.size())) {
			return false;
		}
		std::swap(entries[index], entries[index + 1]);
		return applyLayout(updated);
	}

	[[nodiscard]] bool moveEntry(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int from,
			int to) {
		if (!hasIndex(surface, lane, from)) {
			return false;
		}
		auto updated = _layout;
		auto &entries = ContextEntries(updated, surface, lane);
		const auto insertAt = std::clamp(to, 0, std::max(int(entries.size()) - 1, 0));
		if (from == insertAt) {
			return true;
		}
		const auto moved = entries[from];
		entries.erase(entries.begin() + from);
		entries.insert(entries.begin() + insertAt, moved);
		return applyLayout(updated);
	}

private:
	[[nodiscard]] bool applyLayout(
			const HistoryView::ContextMenuCustomizationLayout &updated) {
		auto normalized = updated;
		normalizeDuplicates(normalized);
		if (!HistoryView::SaveContextMenuCustomizationLayout(normalized)) {
			return false;
		}
		_layout = HistoryView::LoadContextMenuCustomizationLayout();
		normalizeDuplicates(_layout);
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool hasIndex(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int index) const {
		const auto &list = entries(surface, lane);
		return (index >= 0) && (index < int(list.size()));
	}

	[[nodiscard]] static int findIndexById(
			const std::vector<HistoryView::ContextMenuLayoutEntry> &entries,
			const QString &id) {
		for (auto i = 0; i != int(entries.size()); ++i) {
			if (entries[i].id == id) {
				return i;
			}
		}
		return -1;
	}

	static void hideCounterpart(
			HistoryView::ContextMenuCustomizationLayout &layout,
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			const QString &id) {
		auto &other = ContextEntries(layout, surface, OtherContextLane(lane));
		const auto index = findIndexById(other, id);
		if (index >= 0) {
			other[index].visible = false;
		}
	}

	static void normalizeDuplicates(
			HistoryView::ContextMenuCustomizationLayout &layout) {
		const auto normalizeEntries = [](
				std::vector<HistoryView::ContextMenuLayoutEntry> &entries) {
			auto seen = base::flat_set<QString>();
			for (auto &entry : entries) {
				if (!entry.participatesInUnifiedLayout()) {
					continue;
				}
				const auto key = entry.duplicateKey();
				if (seen.contains(key)) {
					entry.visible = false;
					continue;
				}
				seen.emplace(key);
			}
		};
		for (const auto surface : {
				HistoryView::ContextMenuSurface::Message,
				HistoryView::ContextMenuSurface::Selection,
		}) {
			auto &menu = ContextEntries(layout, surface, ContextEditorLane::Menu);
			auto &strip = ContextEntries(layout, surface, ContextEditorLane::Strip);
			normalizeEntries(strip);
			auto stripKeys = base::flat_set<QString>();
			for (const auto &entry : strip) {
				if (entry.participatesInUnifiedLayout()) {
					stripKeys.emplace(entry.duplicateKey());
				}
			}
			auto seenMenu = base::flat_set<QString>();
			for (auto &menuEntry : menu) {
				if (!menuEntry.participatesInUnifiedLayout()) {
					continue;
				}
				const auto key = menuEntry.duplicateKey();
				if (stripKeys.contains(key) || seenMenu.contains(key)) {
					menuEntry.visible = false;
					continue;
				}
				seenMenu.emplace(key);
			}
		}
	}

	HistoryView::ContextMenuCustomizationLayout _layout;
	mutable rpl::event_stream<> _changes;
};


class CustomizationStatusDeck final : public Ui::RpWidget {
public:
	CustomizationStatusDeck(
		QWidget *parent,
		std::shared_ptr<SideMenuEditorState> state,
		std::shared_ptr<ContextMenuEditorState> contextState)
	: RpWidget(parent)
	, _state(std::move(state))
	, _contextState(std::move(contextState)) {
		update();
		updateGeometry();
		_state->changes() | rpl::on_next([=] {
			update();
			updateGeometry();
		}, lifetime());
		_contextState->changes() | rpl::on_next([=] {
			update();
			updateGeometry();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto columns = (newWidth > 920) ? 4 : (newWidth > 560) ? 2 : 1;
		if (columns == 2) {
			return kCustomizationStatusDeckHeight;
		}
		const auto rows = (4 + columns - 1) / columns;
		return 18 + (rows * 68) + ((rows - 1) * 12) + 18;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);

		const auto side = ComputeSideMenuLayoutStats(_state->entries());
		const auto message = ComputeContextLayoutStats(
			_contextState->layout(),
			HistoryView::ContextMenuSurface::Message);
		const auto selection = ComputeContextLayoutStats(
			_contextState->layout(),
			HistoryView::ContextMenuSurface::Selection);
		const auto columns = (width() > 920) ? 4 : (width() > 560) ? 2 : 1;
		const auto gap = 12;
		const auto margin = 6;
		const auto cardHeight = 68;
		const auto cardWidth = std::max(
			160,
			(width() - (margin * 2) - ((columns - 1) * gap)) / columns);

		auto drawCard = [&](int index,
				QColor top,
				QColor bottom,
				QColor accent,
				const QString &eyebrow,
				const QString &title,
				const QString &body) {
			const auto row = index / columns;
			const auto column = index % columns;
			const auto rect = QRect(
				margin + column * (cardWidth + gap),
				18 + row * (cardHeight + gap),
				cardWidth,
				cardHeight);
			auto gradient = QLinearGradient(
				QPointF(rect.left(), rect.top()),
				QPointF(rect.right(), rect.bottom()));
			gradient.setColorAt(0., top);
			gradient.setColorAt(1., bottom);
			p.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(), 88), 1.));
			p.setBrush(gradient);
			p.drawRoundedRect(rect.adjusted(0, 0, -1, -1), 18, 18);
			p.setPen(accent);
			p.setFont(st::normalFont->f);
			p.drawText(
				QRect(rect.left() + 14, rect.top() + 10, rect.width() - 28, 14),
				Qt::AlignLeft | Qt::AlignVCenter,
				eyebrow);
			p.setPen(QColor(0x20, 0x2d, 0x3a));
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				QRect(rect.left() + 14, rect.top() + 26, rect.width() - 28, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				title);
			p.setPen(QColor(0x5f, 0x6d, 0x7c));
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(rect.left() + 14, rect.top() + 44, rect.width() - 28, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				QFontMetrics(st::defaultTextStyle.font->f).elidedText(
					body,
					Qt::ElideRight,
					rect.width() - 28));
		};

		drawCard(
			0,
			QColor(0xEE, 0xF9, 0xF2),
			QColor(0xE5, 0xF5, 0xEC),
			QColor(0x22, 0x9D, 0x68),
			RuEn("Side menu runtime", "Side menu runtime"),
			RuEn(
				"%1 видимых · %2 скрыто",
				"%1 visible · %2 hidden").arg(side.visibleActions).arg(side.hiddenActions),
			RuEn(
				"Footer %1 · профиль %2",
				"Footer %1 · profile %2")
					.arg(_state->showFooterText() ? RuEn("on", "on") : RuEn("off", "off"))
					.arg(_state->profileAtBottom() ? RuEn("снизу", "bottom") : RuEn("сверху", "top")));

		drawCard(
			1,
			QColor(0xEF, 0xF4, 0xFF),
			QColor(0xE6, 0xEF, 0xFF),
			QColor(0x4F, 0x8D, 0xFF),
			RuEn("Hooks Astrogram", "Astrogram hooks"),
			RuEn(
				"Plugins %1 · Ghost %2",
				"Plugins %1 · Ghost %2")
					.arg(side.pluginsVisible ? RuEn("в меню", "shown") : RuEn("скрыт", "hidden"))
					.arg(side.ghostModeVisible ? RuEn("в меню", "shown") : RuEn("скрыт", "hidden")),
			RuEn(
				"Show Logs %1",
				"Show Logs %1")
					.arg(side.showLogsVisible ? RuEn("подхвачен", "hooked") : RuEn("не показан", "not shown")));

		drawCard(
			2,
			QColor(0xFF, 0xF7, 0xEA),
			QColor(0xFF, 0xF1, 0xDD),
			QColor(0xE3, 0x93, 0x23),
			RuEn("Dividers & layout", "Dividers & layout"),
			RuEn(
				"%1 разделителей · %2 custom",
				"%1 dividers · %2 custom").arg(side.visibleSeparators).arg(side.customSeparators),
			RuEn(
				"Expanded %1 · left-edge %2",
				"Expanded %1 · left-edge %2")
					.arg(_state->expandedSidePanel() ? RuEn("on", "on") : RuEn("off", "off"))
					.arg(_state->leftEdgeSettings() ? RuEn("on", "on") : RuEn("off", "off")));

		drawCard(
			3,
			QColor(0xF2, 0xF8, 0xFF),
			QColor(0xE9, 0xF2, 0xFF),
			QColor(0x4C, 0x7E, 0xE6),
			RuEn("Подменю", "Submenu"),
			RuEn(
				"msg %1/%2 · sel %3/%4",
				"msg %1/%2 · sel %3/%4")
					.arg(message.visibleStrip)
					.arg(kContextStripPreviewLimit)
					.arg(selection.visibleStrip)
					.arg(kContextStripPreviewLimit),
			(message.forwardWithoutAuthorVisible || selection.forwardWithoutAuthorVisible)
				? RuEn(
					"Кнопка «без автора» уже заведена в runtime layout",
					"The no-author action is already wired into the runtime layout")
				: RuEn(
					"Иконки можно двигать и возвращать снизу",
					"Icons can be reordered and restored from below"));
	}

private:
	const std::shared_ptr<SideMenuEditorState> _state;
	const std::shared_ptr<ContextMenuEditorState> _contextState;
};

class ContextMenuPreview final : public Ui::RpWidget {
public:
	ContextMenuPreview(
		QWidget *parent,
		std::shared_ptr<ContextMenuEditorState> state)
	: RpWidget(parent)
	, _state(std::move(state)) {
		update();
		_state->changes() | rpl::on_next([=] {
			update();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return (newWidth > 760)
			? kContextPreviewHeight
			: (kContextPreviewHeight + 110);
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);

		const auto outer = rect().adjusted(0, 0, -1, -1);
		auto path = QPainterPath();
		path.addRoundedRect(outer, kPreviewRadius, kPreviewRadius);
		auto background = QLinearGradient(
			QPointF(0., 0.),
			QPointF(width(), height()));
		background.setColorAt(0., QColor(0xF0, 0xFA, 0xF5));
		background.setColorAt(1., QColor(0xF7, 0xFB, 0xFD));
		p.fillPath(path, background);
		p.setClipPath(path);

		const auto gap = 18;
		const auto stacked = (width() <= 760);
		const auto paneWidth = stacked
			? (width() - 32)
			: ((width() - 50) / 2);
		const auto first = QRect(16, 16, paneWidth, 136);
		const auto second = stacked
			? QRect(16, first.bottom() + gap, paneWidth, 136)
			: QRect(first.right() + gap, 16, paneWidth, 136);
		paintSurface(&p, first, HistoryView::ContextMenuSurface::Message, false);
		paintSurface(&p, second, HistoryView::ContextMenuSurface::Selection, true);

		p.setClipping(false);
		p.setPen(QColor(0xD3, 0xDE, 0xE8));
		p.drawRoundedRect(outer, kPreviewRadius, kPreviewRadius);
	}

private:
	void paintSurface(
			QPainter *p,
			const QRect &rect,
			HistoryView::ContextMenuSurface surface,
			bool selectionMode) const {
		p->setPen(Qt::NoPen);
		p->setBrush(QColor(0xE4, 0xF2, 0xEA));
		p->drawRoundedRect(rect, 22, 22);

		const auto bubbleLeft = QRect(rect.left() + 16, rect.top() + 18, 138, 34);
		const auto bubbleRight = QRect(rect.right() - 154, rect.bottom() - 46, 136, 30);
		p->setBrush(QColor(255, 255, 255, 220));
		p->drawRoundedRect(bubbleLeft, 16, 16);
		p->setBrush(selectionMode ? QColor(0xD7, 0xEC, 0xFF) : QColor(0xD3, 0xF1, 0xE0));
		p->drawRoundedRect(bubbleRight, 16, 16);
		if (selectionMode) {
			p->setBrush(QColor(0x4F, 0x8D, 0xFF, 52));
			p->drawRoundedRect(
				QRect(rect.left() + 26, rect.top() + 62, rect.width() - 140, 24),
				12,
				12);
		}

		const auto popup = QRect(rect.left() + 94, rect.top() + 26, rect.width() - 118, 84);
		p->setBrush(QColor(255, 255, 255, 246));
		p->drawRoundedRect(popup, 18, 18);

		p->setBrush(QColor(0xE7, 0xF8, 0xEF));
		p->drawRoundedRect(QRect(popup.left() + 12, popup.top() + 10, 128, 22), 11, 11);
		p->setPen(QColor(0x1C, 0x8B, 0x62));
		p->setFont(st::normalFont->f);
		p->drawText(
			QRect(popup.left() + 12, popup.top() + 10, 128, 22),
			Qt::AlignCenter,
			ContextSurfaceTitle(surface));

		const auto stats = ComputeContextLayoutStats(_state->layout(), surface);
		auto badgeRight = rect.right() - 14;
		auto drawBadge = [&](QString text, QColor fill, QColor fg) {
			const auto fm = QFontMetrics(st::normalFont->f);
			const auto badgeWidth = std::max(68, fm.horizontalAdvance(text) + 22);
			const auto badge = QRect(badgeRight - badgeWidth, rect.top() + 12, badgeWidth, 22);
			badgeRight = badge.left() - 6;
			p->setPen(Qt::NoPen);
			p->setBrush(fill);
			p->drawRoundedRect(badge, 11, 11);
			p->setPen(fg);
			p->setFont(st::normalFont->f);
			p->drawText(badge, Qt::AlignCenter, text);
		};
		drawBadge(
			RuEn("Strip %1/%2", "Strip %1/%2")
				.arg(stats.visibleStrip)
				.arg(kContextStripPreviewLimit),
			QColor(0xE8, 0xF6, 0xEE),
			QColor(0x1C, 0x8B, 0x62));
		if (stats.forwardWithoutAuthorVisible) {
			drawBadge(
				RuEn("Без автора", "No author"),
				QColor(0xEE, 0xF4, 0xFF),
				QColor(0x3F, 0x74, 0xD7));
		}

		const auto visibleMenu = visibleEntries(surface, ContextEditorLane::Menu, 3);
		auto rowTop = popup.top() + 40;
		for (const auto &entry : visibleMenu) {
			const auto meta = DescribeContextAction(entry.id);
			p->setPen(Qt::NoPen);
			p->setBrush(meta.color);
			p->drawEllipse(QRect(popup.left() + 14, rowTop + 2, 16, 16));
			p->setPen(Qt::white);
			p->setFont(st::normalFont->f);
			p->drawText(
				QRect(popup.left() + 14, rowTop + 2, 16, 16),
				Qt::AlignCenter,
				meta.glyph.left(1));
			p->setPen(QColor(0x23, 0x2F, 0x3C));
			p->setFont(st::normalFont->f);
			p->drawText(
				QRect(popup.left() + 38, rowTop, popup.width() - 52, 20),
				Qt::AlignLeft | Qt::AlignVCenter,
				meta.title);
			rowTop += 20;
		}

		const auto visibleStrip = visibleEntries(surface, ContextEditorLane::Strip, kContextStripPreviewLimit);
		if (!visibleStrip.empty()) {
			p->setPen(Qt::NoPen);
			p->setBrush(QColor(0xC7, 0xDA, 0xE8));
			p->drawRect(QRect(popup.left() + 18, popup.bottom() + 4, popup.width() - 36, 1));
			const auto strip = QRect(popup.left() + 10, popup.bottom() + 8, popup.width() - 20, 28);
			p->setPen(Qt::NoPen);
			p->setBrush(QColor(0xE7, 0xF8, 0xEF));
			p->drawRoundedRect(strip, 14, 14);
			auto left = strip.left() + 12;
			for (const auto &entry : visibleStrip) {
				const auto meta = DescribeContextAction(entry.id);
				p->setBrush(meta.color);
				p->drawEllipse(QRect(left, strip.top() + 4, 20, 20));
				p->setPen(Qt::white);
				p->setFont(st::normalFont->f);
				p->drawText(
					QRect(left, strip.top() + 4, 20, 20),
					Qt::AlignCenter,
					meta.glyph.left(1));
				left += 30;
			}
		}
	}

	[[nodiscard]] std::vector<HistoryView::ContextMenuLayoutEntry> visibleEntries(
			HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane,
			int limit) const {
		auto result = std::vector<HistoryView::ContextMenuLayoutEntry>();
		for (const auto &entry : _state->entries(surface, lane)) {
			if (!entry.visible) {
				continue;
			}
			result.push_back(entry);
			if (int(result.size()) >= limit) {
				break;
			}
		}
		return result;
	}

	const std::shared_ptr<ContextMenuEditorState> _state;
};

class ContextMenuEntryList final : public Ui::RpWidget {
public:
	ContextMenuEntryList(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<ContextMenuEditorState> state,
		HistoryView::ContextMenuSurface surface,
		ContextEditorLane lane)
	: RpWidget(parent)
	, _controller(controller)
	, _state(std::move(state))
	, _surface(surface)
	, _lane(lane) {
		setMouseTracking(true);
		const auto refreshSurface = [=] {
			clampSelection();
			_hoveredRow = -1;
			_hoveredKind = ActionKind::None;
			_hoveredOnHandle = false;
			update();
			updateGeometry();
		};
		refreshSurface();
		_state->changes() | rpl::on_next([=] {
			refreshSurface();
		}, lifetime());
	}

	[[nodiscard]] int selectedIndex() const {
		return _selected;
	}

	void setSelectedIndex(int index) {
		_selected = index;
		clampSelection();
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto visibleCount = std::max(int(visibleEntryIndexes().size()), 1);
		const auto base = 18 + (visibleCount * (kRowHeight + kRowGap));
		return base + hiddenSectionHeight(newWidth);
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto visible = visibleEntryIndexes();
		if (visible.empty()) {
			const auto emptyRect = QRect(0, 8, width(), kRowHeight + 8);
			p.setPen(QPen(ThemedRowBorder(false, false)));
			p.setBrush(st::boxBg);
			p.drawRoundedRect(emptyRect, kRowRadius, kRowRadius);
			p.setPen(st::windowFg);
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 12, -18, -18),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Все действия скрыты",
					"All actions are hidden"));
			p.setPen(st::windowSubTextFg);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 34, -18, -12),
				Qt::AlignLeft | Qt::AlignTop,
				(_lane == ContextEditorLane::Strip)
					? RuEn(
						"Ниже остаются маленькие таблетки скрытых иконок: нажми, чтобы вернуть действие обратно в подменю.",
						"Small pills for hidden icons stay below: tap one to bring the action back into the submenu.")
					: RuEn(
						"Ниже остаются маленькие таблетки скрытых действий: нажми, чтобы вернуть нужный пункт обратно в меню.",
						"Small pills for hidden actions stay below: tap one to bring the action back into the menu."));
		} else {
			for (const auto index : visible) {
				if (index == _draggingIndex) {
					continue;
				}
				paintRow(
					p,
					index,
					rowRect(index),
					(index == _hoveredRow),
					(index == _selected),
					false);
			}
		}

		if (_draggingIndex >= 0) {
			p.setPen(QPen(st::windowBgActive->c, 3));
			const auto y = insertionLineY();
			p.drawLine(QPoint(8, y), QPoint(width() - 8, y));
			paintRow(
				p,
				_draggingIndex,
				floatingRowRect(),
				false,
				true,
				true);
		}

		const auto chips = hiddenChips(width());
		if (!chips.empty()) {
			const auto hiddenCount = std::count_if(
				_state->entries(_surface, _lane).begin(),
				_state->entries(_surface, _lane).end(),
				[](const auto &entry) { return !entry.visible; });
			p.setPen(st::windowSubTextFg);
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(0, hiddenSectionTop(), width(), 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				(_lane == ContextEditorLane::Strip)
					? RuEn(
						"Скрытые иконки (%1)",
						"Hidden strip icons (%1)").arg(hiddenCount)
					: RuEn(
						"Скрытые действия (%1)",
						"Hidden actions (%1)").arg(hiddenCount));
			for (const auto &chip : chips) {
				const auto hovered = chip.restoreAll
					? _hoveredRestoreAll
					: (chip.index == _hoveredHiddenEntry);
				const auto fillColor = chip.restoreAll
					? ThemedButtonFill(hovered, true)
					: anim::with_alpha(
						ThemedDescriptorAccent(chip.color, hovered),
						hovered ? 0.22 : 0.12);
				p.setPen(Qt::NoPen);
				p.setBrush(fillColor);
				p.drawRoundedRect(chip.rect, 14, 14);
				p.setPen(chip.restoreAll
					? ThemedButtonText(true)
					: (hovered ? st::windowFgActive->c : st::windowFg->c));
				p.setFont(st::normalFont->f);
				p.drawText(
					chip.rect.adjusted(12, 0, -12, 0),
					Qt::AlignLeft | Qt::AlignVCenter,
					chip.label);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto point = e->pos();
		if (_draggingIndex >= 0) {
			_dragCurrentY = point.y();
			updateDragTarget(point.y());
			_hoveredRow = -1;
			_hoveredKind = ActionKind::None;
			_hoveredOnHandle = false;
			_hoveredHiddenEntry = -1;
			_hoveredRestoreAll = false;
			update();
			return;
		}
		if ((_pressedRow >= 0)
			&& _pressedOnHandle
			&& !_pressedOnButton
			&& ((point - _pressPoint).manhattanLength() >= 8)) {
			startDrag(point);
			return;
		}
		updateHover(point);
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto point = e->pos();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;

		for (const auto &chip : hiddenChips(width())) {
			if (!chip.rect.contains(point)) {
				continue;
			}
			if (chip.restoreAll) {
				auto changed = false;
				const auto entries = _state->entries(_surface, _lane);
				for (auto i = 0; i != int(entries.size()); ++i) {
					if (!entries[i].visible) {
						changed = _state->toggleVisible(_surface, _lane, i) || changed;
					}
				}
				if (!changed && _lane == ContextEditorLane::Strip) {
					_controller->window().showToast(RuEn(
						"В подменю можно держать максимум 4 иконки.",
						"The submenu can show at most 4 icons."));
				}
				clampSelection();
			} else if (_state->toggleVisible(_surface, _lane, chip.index)) {
				_selected = chip.index;
				clampSelection();
			} else if (_lane == ContextEditorLane::Strip) {
				_controller->window().showToast(RuEn(
					"В подменю можно держать максимум 4 иконки.",
					"The submenu can show at most 4 icons."));
			}
			updateHover(point);
			return;
		}

		for (const auto index : visibleEntryIndexes()) {
			const auto row = rowRect(index);
			if (!row.contains(point)) {
				continue;
			}
			_selected = index;
			_pressPoint = point;
			_pressedOnHandle = dragHandleRect(row).contains(point);
			const auto visibleRow = visibleRowForEntry(index);
			_dragGrabOffsetY = point.y() - baseRowRect(visibleRow).top();
			for (const auto &button : actionButtonsForRow(index)) {
				if (!button.enabled || !button.rect.contains(point)) {
					continue;
				}
				_pressedOnButton = true;
				handleAction(index, button.kind);
				updateHover(point);
				return;
			}
			_pressedRow = index;
			updateHover(point);
			update();
			return;
		}
		updateHover(point);
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		if (_draggingIndex >= 0) {
			finishDrag();
			return;
		}
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
		updateHover(e->pos());
	}

	void leaveEventHook(QEvent *e) override {
		Q_UNUSED(e);
		if (_draggingIndex >= 0) {
			return;
		}
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredOnHandle = false;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		unsetCursor();
		update();
	}

private:
	enum class ActionKind {
		None,
		ToggleVisible,
		DeleteCustomSeparator,
		SwitchLane,
	};

	struct ActionButton {
		QRect rect;
		QString label;
		ActionKind kind = ActionKind::None;
		bool enabled = true;
	};

	struct HiddenChip {
		QRect rect;
		int index = -1;
		QString label;
		QColor color;
		bool restoreAll = false;
	};

	[[nodiscard]] std::vector<int> visibleEntryIndexes() const {
		auto result = std::vector<int>();
		for (auto i = 0; i != int(_state->entries(_surface, _lane).size()); ++i) {
			if (_state->entries(_surface, _lane)[i].visible) {
				result.push_back(i);
			}
		}
		return result;
	}

	[[nodiscard]] QRect baseRowRect(int index) const {
		return QRect(
			0,
			8 + index * (kRowHeight + kRowGap),
			width(),
			kRowHeight);
	}

	[[nodiscard]] int visibleRowForEntry(int index) const {
		const auto visible = visibleEntryIndexes();
		const auto i = std::find(visible.begin(), visible.end(), index);
		return (i == visible.end()) ? -1 : int(i - visible.begin());
	}

	[[nodiscard]] QRect rowRectByVisibleRow(int visibleRow) const {
		auto result = baseRowRect(visibleRow);
		if ((_draggingVisibleRow < 0)
			|| (_dragTargetIndex < 0)
			|| (visibleRow == _draggingVisibleRow)) {
			return result;
		}
		const auto delta = kRowHeight + kRowGap;
		if (_draggingVisibleRow < _dragTargetIndex) {
			if ((visibleRow > _draggingVisibleRow) && (visibleRow <= _dragTargetIndex)) {
				result.translate(0, -delta);
			}
		} else if (_draggingVisibleRow > _dragTargetIndex) {
			if ((visibleRow >= _dragTargetIndex) && (visibleRow < _draggingVisibleRow)) {
				result.translate(0, delta);
			}
		}
		return result;
	}

	[[nodiscard]] QRect rowRect(int index) const {
		const auto visibleRow = visibleRowForEntry(index);
		return (visibleRow >= 0) ? rowRectByVisibleRow(visibleRow) : QRect();
	}

	[[nodiscard]] QRect floatingRowRect() const {
		auto result = baseRowRect(_draggingVisibleRow);
		result.moveTop(_dragCurrentY - _dragGrabOffsetY);
		return result;
	}

	[[nodiscard]] QRect dragHandleRect(const QRect &row) const {
		return QRect(row.left() + 10, row.top() + 14, 18, row.height() - 28);
	}

	[[nodiscard]] int hiddenSectionTop() const {
		const auto visibleCount = std::max(int(visibleEntryIndexes().size()), 1);
		return 18 + (visibleCount * (kRowHeight + kRowGap)) + 4;
	}

	[[nodiscard]] std::vector<HiddenChip> hiddenChips(int availableWidth) const {
		auto result = std::vector<HiddenChip>();
		const auto usableWidth = std::max(availableWidth, 120);
		const auto fm = QFontMetrics(st::normalFont->f);
		auto pushChip = [&](int index, QString label, QColor color, bool restoreAll) {
			label = fm.elidedText(label, Qt::ElideRight, 170);
			const auto chipWidth = std::min(
				std::max(76, fm.horizontalAdvance(label) + 22),
				usableWidth);
			auto x = 0;
			auto y = hiddenSectionTop() + 24;
			if (!result.empty()) {
				const auto &last = result.back();
				x = last.rect.right() + 9;
				y = last.rect.top();
				if (x + chipWidth > usableWidth) {
					x = 0;
					y += 36;
				}
			}
			result.push_back(HiddenChip{
				.rect = QRect(x, y, chipWidth, 28),
				.index = index,
				.label = std::move(label),
				.color = color,
				.restoreAll = restoreAll,
			});
		};
		auto hiddenIndexes = std::vector<int>();
		for (auto i = 0; i != int(_state->entries(_surface, _lane).size()); ++i) {
			if (!_state->entries(_surface, _lane)[i].visible) {
				hiddenIndexes.push_back(i);
			}
		}
		if (hiddenIndexes.empty()) {
			return result;
		}
		pushChip(-1, RuEn("Вернуть всё", "Restore all"), st::windowBgActive->c, true);
		for (const auto index : hiddenIndexes) {
			const auto meta = DescribeContextAction(_state->entries(_surface, _lane)[index].id);
			pushChip(index, meta.title, meta.color, false);
		}
		return result;
	}

	[[nodiscard]] int hiddenSectionHeight(int availableWidth) const {
		const auto chips = hiddenChips(availableWidth);
		if (chips.empty()) {
			return 0;
		}
		auto bottom = hiddenSectionTop() + 18;
		for (const auto &chip : chips) {
			bottom = std::max(bottom, chip.rect.bottom());
		}
		return (bottom - hiddenSectionTop()) + 16;
	}

	[[nodiscard]] std::vector<ActionButton> actionButtonsForRow(int index) const {
		const auto &entries = _state->entries(_surface, _lane);
		if ((index < 0) || (index >= int(entries.size())) || !entries[index].visible) {
			return {};
		}
		const auto row = rowRect(index);
		const auto fm = QFontMetrics(st::normalFont->f);
		const auto height = 28;
		auto right = row.right() - kRowPadding;
		auto result = std::vector<ActionButton>();
		const auto push = [&](QString label, ActionKind kind, bool enabled) {
			const auto width = std::max(44, fm.horizontalAdvance(label) + 22);
			right -= width;
			result.push_back(ActionButton{
				.rect = QRect(right, row.top() + 18, width, height),
				.label = std::move(label),
				.kind = kind,
				.enabled = enabled,
			});
			right -= 8;
		};

		if (_state->canMoveToLane(_surface, _lane, index)) {
			push(
				(_lane == ContextEditorLane::Strip)
					? RuEn("В основное", "To main")
					: RuEn("В подменю", "To submenu"),
				ActionKind::SwitchLane,
				true);
		}
		push(
			entries[index].visible
				? RuEn("Скрыть", "Hide")
				: RuEn("Показать", "Show"),
			ActionKind::ToggleVisible,
			true);
		if ((_lane == ContextEditorLane::Menu)
			&& IsCustomSeparatorId(entries[index].id)) {
			push(
				RuEn("Удалить", "Delete"),
				ActionKind::DeleteCustomSeparator,
				true);
		}
		std::reverse(result.begin(), result.end());
		return result;
	}

	void paintDragHandle(Painter &p, const QRect &row, bool active) const {
		const auto handle = dragHandleRect(row);
		p.setPen(Qt::NoPen);
		p.setBrush(active
			? st::windowBgActive->c
			: anim::with_alpha(st::windowSubTextFg->c, 0.65));
		for (auto column = 0; column != 2; ++column) {
			for (auto line = 0; line != 3; ++line) {
				p.drawEllipse(
					QRect(
						handle.left() + (column * 6),
						handle.top() + 2 + (line * 8),
						3,
						3));
			}
		}
	}

	void paintRow(
			Painter &p,
			int index,
			const QRect &row,
			bool hovered,
			bool selected,
			bool floating) const {
		const auto &entry = _state->entries(_surface, _lane)[index];
		const auto meta = DescribeContextAction(entry.id);
		const auto buttons = floating
			? std::vector<ActionButton>()
			: actionButtonsForRow(index);
		const auto tags = LayoutTagChips(
			ContextEntryTags(_surface, _lane, entry.id),
			buttons.empty() ? (row.right() - kRowPadding) : (buttons.front().rect.left() - 8),
			row.top() + 11,
			row.left() + 176);
		const auto contentRight = tags.empty()
			? (buttons.empty() ? (row.right() - 18) : (buttons.front().rect.left() - 10))
			: (tags.front().rect.left() - 10);
		const auto accent = ThemedDescriptorAccent(
			meta.color,
			hovered || selected || floating);

		p.setPen(QPen(ThemedRowBorder(hovered, selected)));
		p.setBrush(ThemedRowFill(hovered, selected, floating));
		p.drawRoundedRect(row, kRowRadius, kRowRadius);

		paintDragHandle(p, row, hovered || selected || floating);
		p.setBrush(accent);
		p.drawEllipse(QRect(row.left() + 36, row.top() + 14, 36, 36));
		if (IsCustomSeparatorId(entry.id)) {
			p.setPen(QPen(Qt::white, 2));
			p.drawLine(
				row.left() + 45,
				row.center().y(),
				row.left() + 63,
				row.center().y());
		} else if (meta.icon) {
			meta.icon->paintInCenter(
				p,
				QRect(row.left() + 36, row.top() + 14, 36, 36));
		} else {
			p.setPen(Qt::white);
			p.setFont(st::semiboldFont->f);
			p.drawText(
				QRect(row.left() + 36, row.top() + 14, 36, 36),
				Qt::AlignCenter,
				meta.glyph.left(1));
		}

		p.setPen(st::windowFg);
		p.setFont(st::semiboldTextStyle.font->f);
		const auto titleWidth = std::max(88, contentRight - (row.left() + 84));
		p.drawText(
			QRect(row.left() + 84, row.top() + 12, titleWidth, 20),
			Qt::AlignLeft | Qt::AlignVCenter,
			QFontMetrics(st::semiboldTextStyle.font->f).elidedText(
				meta.title,
				Qt::ElideRight,
				titleWidth));
		PaintTagChips(p, tags);

		const auto stateText = entry.visible
			? meta.subtitle
			: RuEn("Скрыто", "Hidden");
		const auto subtitle = QFontMetrics(st::defaultTextStyle.font->f).elidedText(
			stateText,
			Qt::ElideRight,
			titleWidth);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::defaultTextStyle.font->f);
		p.drawText(
			QRect(row.left() + 84, row.top() + 34, titleWidth, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			subtitle);

		if (!floating) {
			for (const auto &button : buttons) {
				paintActionButton(p, button);
			}
		}
	}

	void paintActionButton(QPainter &p, const ActionButton &button) const {
		const auto hovered = (_hoveredRow >= 0)
			&& (button.kind == _hoveredKind)
			&& button.rect.intersects(rowRect(_hoveredRow));
		p.setPen(Qt::NoPen);
		p.setBrush(ThemedButtonFill(hovered, button.enabled));
		p.drawRoundedRect(button.rect, 14, 14);
		p.setPen(ThemedButtonText(button.enabled));
		p.setFont(st::normalFont->f);
		p.drawText(button.rect, Qt::AlignCenter, button.label);
	}

	[[nodiscard]] int insertionLineY() const {
		if ((_draggingVisibleRow < 0) || (_dragTargetIndex < 0)) {
			return 0;
		}
		const auto visible = visibleEntryIndexes();
		auto slot = 0;
		for (auto visibleRow = 0; visibleRow != int(visible.size()); ++visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			if (slot == _dragTargetIndex) {
				return rowRectByVisibleRow(visibleRow).top() - (kRowGap / 2);
			}
			++slot;
		}
		for (auto visibleRow = int(visible.size()) - 1; visibleRow >= 0; --visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			return rowRectByVisibleRow(visibleRow).bottom() + (kRowGap / 2) + 1;
		}
		return baseRowRect(0).center().y();
	}

	void updateDragTarget(int y) {
		if (_draggingVisibleRow < 0) {
			return;
		}
		const auto visible = visibleEntryIndexes();
		auto slot = 0;
		for (auto visibleRow = 0; visibleRow != int(visible.size()); ++visibleRow) {
			if (visibleRow == _draggingVisibleRow) {
				continue;
			}
			if (y < rowRectByVisibleRow(visibleRow).center().y()) {
				_dragTargetIndex = slot;
				return;
			}
			++slot;
		}
		_dragTargetIndex = slot;
	}

	void startDrag(QPoint point) {
		if (_pressedRow < 0) {
			return;
		}
		_draggingIndex = _pressedRow;
		_draggingVisibleRow = visibleRowForEntry(_pressedRow);
		if (_draggingVisibleRow < 0) {
			return;
		}
		_dragTargetIndex = _draggingVisibleRow;
		_dragCurrentY = point.y();
		_dragGrabOffsetY = point.y() - baseRowRect(_draggingVisibleRow).top();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
		grabMouse();
		setCursor(Qt::ClosedHandCursor);
		updateDragTarget(point.y());
		update();
	}

	void finishDrag() {
		if (_draggingVisibleRow < 0) {
			return;
		}
		const auto maxTarget = std::max(int(visibleEntryIndexes().size()) - 1, 0);
		const auto target = std::clamp(_dragTargetIndex, 0, maxTarget);
		if (_state->moveEntry(_surface, _lane, _draggingIndex, target)) {
			_selected = _draggingIndex;
		}
		_draggingIndex = -1;
		_draggingVisibleRow = -1;
		_dragTargetIndex = -1;
		_dragCurrentY = 0;
		_dragGrabOffsetY = 0;
		releaseMouse();
		_pressedRow = -1;
		_pressedOnButton = false;
		_pressedOnHandle = false;
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredOnHandle = false;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		unsetCursor();
		clampSelection();
		update();
	}

	void updateHover(QPoint point) {
		auto newHiddenEntry = -1;
		auto newRestoreAll = false;
		for (const auto &chip : hiddenChips(width())) {
			if (!chip.rect.contains(point)) {
				continue;
			}
			newRestoreAll = chip.restoreAll;
			newHiddenEntry = chip.restoreAll ? -1 : chip.index;
			break;
		}

		auto newRow = -1;
		auto newKind = ActionKind::None;
		auto newOnHandle = false;
		if (!newRestoreAll && (newHiddenEntry < 0)) {
			for (const auto index : visibleEntryIndexes()) {
				const auto row = rowRect(index);
				if (!row.contains(point)) {
					continue;
				}
				newRow = index;
				newOnHandle = dragHandleRect(row).contains(point);
				for (const auto &button : actionButtonsForRow(index)) {
					if (button.enabled && button.rect.contains(point)) {
						newKind = button.kind;
						break;
					}
				}
				break;
			}
		}
		if ((newRow == _hoveredRow)
			&& (newKind == _hoveredKind)
			&& (newOnHandle == _hoveredOnHandle)
			&& (newHiddenEntry == _hoveredHiddenEntry)
			&& (newRestoreAll == _hoveredRestoreAll)) {
			return;
		}
		_hoveredRow = newRow;
		_hoveredKind = newKind;
		_hoveredOnHandle = newOnHandle;
		_hoveredHiddenEntry = newHiddenEntry;
		_hoveredRestoreAll = newRestoreAll;
		if (_hoveredRestoreAll || (_hoveredHiddenEntry >= 0) || ((_hoveredRow >= 0) && (_hoveredKind != ActionKind::None))) {
			setCursor(Qt::PointingHandCursor);
		} else if (_hoveredOnHandle) {
			setCursor(Qt::OpenHandCursor);
		} else {
			unsetCursor();
		}
		update();
	}

	void handleAction(int index, ActionKind kind) {
		auto changed = false;
		switch (kind) {
		case ActionKind::ToggleVisible:
			changed = _state->toggleVisible(_surface, _lane, index);
			break;
		case ActionKind::DeleteCustomSeparator:
			changed = _state->removeCustomSeparator(_surface, _lane, index);
			break;
		case ActionKind::SwitchLane:
			changed = _state->switchLane(_surface, _lane, index);
			break;
		case ActionKind::None:
			break;
		}
		if (changed) {
			_selected = index;
			clampSelection();
		} else {
			if (((kind == ActionKind::ToggleVisible)
				&& (_lane == ContextEditorLane::Strip))
				|| ((kind == ActionKind::SwitchLane)
					&& (_lane == ContextEditorLane::Menu))) {
				_controller->window().showToast(RuEn(
					"В подменю можно держать максимум 4 иконки.",
					"The submenu can show at most 4 icons."));
			} else {
				update();
			}
		}
	}

	void clampSelection() {
		const auto visible = visibleEntryIndexes();
		if (visible.empty()) {
			_selected = -1;
		} else if ((_selected < 0)
			|| (_selected >= int(_state->entries(_surface, _lane).size()))
			|| !_state->entries(_surface, _lane)[_selected].visible) {
			_selected = visible.front();
		}
	}

	const not_null<Window::SessionController*> _controller;
	const std::shared_ptr<ContextMenuEditorState> _state;
	const HistoryView::ContextMenuSurface _surface;
	const ContextEditorLane _lane = ContextEditorLane::Menu;
	int _selected = 0;
	int _hoveredRow = -1;
	ActionKind _hoveredKind = ActionKind::None;
	bool _hoveredOnHandle = false;
	int _hoveredHiddenEntry = -1;
	bool _hoveredRestoreAll = false;
	int _pressedRow = -1;
	bool _pressedOnButton = false;
	bool _pressedOnHandle = false;
	QPoint _pressPoint;
	int _draggingIndex = -1;
	int _draggingVisibleRow = -1;
	int _dragTargetIndex = -1;
	int _dragCurrentY = 0;
	int _dragGrabOffsetY = 0;
};

void AddPreviewToggle(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &description,
		bool initial,
		Fn<bool(bool)> apply) {
	const auto stream = container->lifetime().make_state<rpl::event_stream<bool>>();
	const auto button = container->add(object_ptr<Button>(
		container,
		rpl::single(title),
		st::settingsButtonNoIcon))->toggleOn(
			stream->events_starting_with_copy(initial));
	button->toggledChanges() | rpl::on_next([=](bool toggled) {
		if (!apply(toggled)) {
			stream->fire_copy(!toggled);
			controller->window().showToast(RuEn(
				"Не удалось сохранить настройку меню.",
				"Could not save the menu setting."));
			return;
		}
		stream->fire_copy(toggled);
	}, button->lifetime());
	if (!description.isEmpty()) {
		Ui::AddDividerText(container, rpl::single(description));
	}
}

void AddEditorSectionHeader(
		not_null<Ui::VerticalLayout*> container,
		const QString &title) {
	Ui::AddDivider(container);
	const auto label = Ui::AddSubsectionTitle(
		container,
		rpl::single(title),
		QMargins(
			st::boxRowPadding.left() - st::defaultSubsectionTitlePadding.left(),
			0,
			0,
			0));
	label->setTextColorOverride(st::windowActiveTextFg->c);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
}

void AddEditorLaneHeader(
		not_null<Ui::VerticalLayout*> container,
		const QString &title) {
	const auto label = Ui::AddSubsectionTitle(
		container,
		rpl::single(title),
		QMargins(
			st::boxRowPadding.left() - st::defaultSubsectionTitlePadding.left(),
			0,
			0,
			0));
	label->setTextColorOverride(st::windowActiveTextFg->c);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 4);
}

void AddEditorInlineCaption(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &description) {
	AddEditorLaneHeader(container, title);
	if (!description.isEmpty()) {
		Ui::AddDividerText(container, rpl::single(description));
	}
}

} // namespace

void AddMenuCustomizationEditor(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto includeShowLogs = !Core::App().plugins().actionsFor(
		u"astro.show_logs"_q).empty();
	const auto state = std::make_shared<SideMenuEditorState>(
		controller->session().supportMode(),
		includeShowLogs);
	const auto contextState = std::make_shared<ContextMenuEditorState>();

	AddEditorSectionHeader(
		container,
		RuEn("Боковая панель", "Side panel"));
	const auto list = container->add(
		object_ptr<SideMenuEntryList>(container, state),
		style::margins(6, 0, 6, 0),
		style::al_top);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Добавить разделитель",
			"Add divider")),
		st::settingsButton,
		{ &st::menuIconAdd }
	)->addClickHandler([=] {
		const auto inserted = state->addCustomSeparatorAfter(list->selectedIndex());
		if (inserted < 0) {
			controller->window().showToast(RuEn(
				"Не удалось добавить разделитель.",
				"Could not add the divider."));
			return;
		}
		list->setSelectedIndex(inserted);
		controller->window().showToast(RuEn(
			"Разделитель добавлен в layout.",
			"Divider added to the layout."));
	});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Сбросить layout бокового меню к дефолту",
			"Reset the side menu layout to defaults")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		if (!state->resetToDefaults()) {
			controller->window().showToast(RuEn(
				"Не удалось сбросить layout.",
				"Could not reset the layout."));
			return;
		}
		list->setSelectedIndex(0);
		controller->window().showToast(RuEn(
			"Боковое меню сброшено к дефолту.",
			"Side menu layout reset to defaults."));
	});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Вернуть все скрытые пункты бокового меню",
			"Restore all hidden side menu items")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		if (!state->restoreAllHidden()) {
			controller->window().showToast(RuEn(
				"Не удалось вернуть скрытые пункты.",
				"Could not restore hidden items."));
			return;
		}
		controller->window().showToast(RuEn(
			"Все скрытые пункты возвращены в меню.",
			"All hidden items were restored."));
	});

	AddEditorLaneHeader(
		container,
		RuEn("Поведение", "Behaviour"));

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Показывать Astrogram Desktop и версию внизу меню",
			"Show Astrogram Desktop and version at the bottom"),
		QString(),
		state->showFooterText(),
		[=](bool value) {
			return state->setShowFooterText(value);
		});

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Профиль и список аккаунтов внизу бокового меню",
			"Move the profile block and account list to the bottom"),
		QString(),
		state->profileAtBottom(),
		[=](bool value) {
			return state->setProfileBlockPosition(value
				? QString::fromLatin1(Menu::Customization::SideMenuProfileBlockPositionId::Bottom)
				: QString::fromLatin1(Menu::Customization::SideMenuProfileBlockPositionId::Top));
		});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Сбросить вид боковой панели к дефолту",
			"Reset the side panel presentation")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		if (!state->resetOptionsToDefaults()) {
			controller->window().showToast(RuEn(
				"Не удалось сбросить вид боковой панели.",
				"Could not reset the side panel presentation."));
			return;
		}
		controller->window().showToast(RuEn(
			"Вид боковой панели сброшен к дефолту.",
			"The side panel presentation was reset to defaults."));
	});

	AddEditorSectionHeader(
		container,
		RuEn("Контекстное меню", "Context menu"));
	Ui::AddDividerText(container, rpl::single(RuEn(
		"Теги справа показывают, где кнопка видна: в основном меню, в подменю или только при выделении.",
		"The tags on the right show where the action appears: in the main menu, in the submenu or only for selections.")));

	const auto addContextSurfaceEditor = [&](HistoryView::ContextMenuSurface surface) {
		AddEditorInlineCaption(
			container,
			ContextSurfaceTitle(surface) + u" · "_q + ContextLaneTitle(ContextEditorLane::Menu),
			QString());
		const auto menuList = container->add(
			object_ptr<ContextMenuEntryList>(
				container,
				controller,
				contextState,
				surface,
				ContextEditorLane::Menu),
			style::margins(6, 0, 6, 0),
			style::al_top);
		Ui::AddSkip(container, st::settingsCheckboxesSkip / 4);
		AddButtonWithIcon(
			container,
			rpl::single(RuEn(
				"Добавить разделитель в основное меню",
				"Add divider to the main menu")),
			st::settingsButton,
			{ &st::menuIconAdd }
		)->addClickHandler([=] {
			const auto inserted = contextState->addCustomSeparatorAfter(
				surface,
				ContextEditorLane::Menu,
				menuList->selectedIndex());
			if (inserted < 0) {
				controller->window().showToast(RuEn(
					"Не удалось добавить разделитель.",
					"Could not add the divider."));
				return;
			}
			menuList->setSelectedIndex(inserted);
		});
		Ui::AddSkip(container, st::settingsCheckboxesSkip / 4);
		AddEditorInlineCaption(
			container,
			ContextSurfaceTitle(surface) + u" · "_q + ContextLaneTitle(ContextEditorLane::Strip),
			QString());
		container->add(
			object_ptr<ContextMenuEntryList>(
				container,
				controller,
				contextState,
				surface,
				ContextEditorLane::Strip),
			style::margins(6, 0, 6, 0),
			style::al_top);
		Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	};

	addContextSurfaceEditor(HistoryView::ContextMenuSurface::Message);
	Ui::AddDivider(container);
	addContextSurfaceEditor(HistoryView::ContextMenuSurface::Selection);

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Сбросить раскладку контекстного меню к дефолту",
			"Reset the context menu layout to defaults")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		if (!contextState->resetToDefaults()) {
			controller->window().showToast(RuEn(
				"Не удалось сбросить context layout.",
				"Could not reset the context layout."));
			return;
		}
		controller->window().showToast(RuEn(
			"Контекстное меню сброшено к дефолту.",
			"Context menu layout reset to defaults."));
	});
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

void AddThreeDotsMenuCustomizationEditor(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto state = std::make_shared<ThreeDotsMenuEditorState>();
	const auto showSaveError = [=] {
		controller->window().showToast(RuEn(
			"Не удалось сохранить layout меню на 3 точки.",
			"Could not save the three-dots menu layout."));
	};
	AddEditorSectionHeader(
		container,
		RuEn("Меню на 3 точки", "Three-dots menu"));

	const auto list = container->add(
		object_ptr<ThreeDotsMenuEntryList>(container, state),
		style::margins(6, 0, 6, 0),
		style::al_top);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Добавить разделитель",
			"Add divider")),
		st::settingsButton,
		{ &st::menuIconAdd }
	)->addClickHandler([=] {
		const auto entriesCount = int(state->entries().size());
		const auto baseIndex = (list->selectedIndex() >= 0)
			? list->selectedIndex()
			: (entriesCount - 1);
		const auto inserted = state->addCustomSeparatorAfter(baseIndex);
		if (inserted < 0) {
			showSaveError();
			return;
		}
		list->setSelectedIndex(inserted);
	});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Вернуть все скрытые пункты",
			"Restore all hidden items")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		if (!state->restoreAllHidden()) {
			showSaveError();
		}
	});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Сбросить меню на 3 точки к дефолту",
			"Reset the three-dots menu to defaults")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		if (!state->resetToDefaults()) {
			showSaveError();
			return;
		}
		list->setSelectedIndex(0);
	});
}

} // namespace Settings
