/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_menu_customization_editor.h"

#include "settings/settings_common.h"
#include "history/view/history_view_context_menu.h"
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
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return result;
	}
	const auto object = document.object();
	result.immersiveAnimation = object.value(
		QStringLiteral("immersive_animation")).toBool(result.immersiveAnimation);
	result.expandedSidePanel = object.value(
		QStringLiteral("expanded_side_panel")).toBool(result.expandedSidePanel);
	result.leftEdgeSettings = object.value(
		QStringLiteral("left_edge_settings")).toBool(result.leftEdgeSettings);
	result.wideSettingsPane = object.value(
		QStringLiteral("wide_settings_pane")).toBool(result.wideSettingsPane);
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
		{ QStringLiteral("immersive_animation"), prefs.immersiveAnimation },
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
constexpr auto kRowHeight = 64;
constexpr auto kRowGap = 10;
constexpr auto kRowPadding = 14;
constexpr auto kRowRadius = 18;
constexpr auto kPreviewRadius = 24;

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
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

struct EntryDescriptor {
	QString title;
	QString subtitle;
	QString glyph;
	QColor color;
};

[[nodiscard]] EntryDescriptor DescribeEntry(
		const Menu::Customization::SideMenuEntry &entry,
		bool supportMode) {
	using Id = Menu::Customization::SideMenuItemId;
	if (entry.separator) {
		if (entry.id == QString::fromLatin1(Id::SeparatorPrimary)) {
			return {
				.title = RuEn("Основной разделитель", "Primary divider"),
				.subtitle = RuEn(
					"Отделяет верхний блок меню от быстрых действий.",
					"Separates the top profile block from quick actions."),
				.glyph = u"="_q,
				.color = QColor(0x76, 0x86, 0x9B),
			};
		} else if (entry.id == QString::fromLatin1(Id::SeparatorSystem)) {
			return {
				.title = RuEn("Системный разделитель", "System divider"),
				.subtitle = RuEn(
					"Отделяет системные и Astrogram-пункты.",
					"Separates system and Astrogram actions."),
				.glyph = u"="_q,
				.color = QColor(0x4D, 0xB7, 0x88),
			};
		}
		return {
			.title = RuEn("Пользовательский разделитель", "Custom divider"),
			.subtitle = RuEn(
				"Свободный разделитель, который можно переставлять или удалить.",
				"Free divider that can be moved or removed."),
			.glyph = u"="_q,
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
		_state->changes() | rpl::start_with_next([=] {
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
		for (const auto &entry : _state->entries()) {
			if (!entry.visible) {
				++hiddenCount;
			}
		}

		auto top = profileAtBottom ? 24 : (profileRect.bottom() + 16);
		const auto rowsBottom = profileAtBottom
			? (profileRect.top() - 14)
			: (height() - footerHeight - 18);
		auto shownRows = 0;
		for (const auto &entry : _state->entries()) {
			if (!entry.visible) {
				continue;
			}
			if (top > (rowsBottom - 30)) {
				break;
			}
			const auto meta = DescribeEntry(entry, _state->supportMode());
			if (entry.separator) {
				p.fillRect(
					QRect(26, top + 13, menuWidth - 52, 1),
					QColor(255, 255, 255, 44));
				top += 18;
				continue;
			}
			const auto rowRect = QRect(18, top, menuWidth - 36, expanded ? 38 : 34);
			p.setBrush(shownRows == 0
				? QColor(255, 255, 255, 18)
				: QColor(255, 255, 255, 8));
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(rowRect, 14, 14);

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
					rowRect.width() - 50,
					rowRect.height()),
				Qt::AlignVCenter | Qt::AlignLeft,
				meta.title);
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
		if (immersive) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x35, 0xC3, 0x8F, 48));
			p.drawRoundedRect(
				QRect(chatRect.left() + 12, 12, 112, 28),
				14,
				14);
			p.setPen(QColor(0x13, 0x2B, 0x3F));
			p.setFont(st::normalFont->f);
			p.drawText(
				QRect(chatRect.left() + 22, 12, 96, 28),
				Qt::AlignCenter,
				RuEn("Иммерсивно", "Immersive"));
		}
		if (expanded) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x35, 0xC3, 0x8F, 38));
			p.drawRoundedRect(
				QRect(24, height() - 64, 154, 28),
				14,
				14);
			p.setPen(QColor(255, 255, 255, 220));
			p.setFont(st::normalFont->f);
			p.drawText(
				QRect(24, height() - 64, 154, 28),
				Qt::AlignCenter,
				RuEn("Расширенная панель", "Expanded panel"));
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
		_state->changes() | rpl::start_with_next([=] {
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

		p.setPen(QColor(0x5F, 0x6D, 0x7C));
		p.setFont(st::normalFont->f);
		p.drawText(
			QRect(scene.left() + 18, scene.bottom() - 20, scene.width() - 36, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			RuEn(
				"Архитектура уже разложена под native preview для side / peer / context / strip. Следующий шаг: action catalog + drag handles поверх этих сцен.",
				"The architecture is already laid out for native previews of side / peer / context / strip. The next step is action catalogs plus drag handles on top of these scenes."));

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
		_state->changes() | rpl::start_with_next([=] {
			const auto selected = _state->selectedEntryIndex();
			if ((selected >= 0)
				&& (selected < int(_state->entries().size()))
				&& _state->entries()[selected].visible) {
				_selected = selected;
			}
			clampSelection();
			update();
			updateGeometry();
		}, lifetime());
		clampSelection();
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
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xF7, 0xFB, 0xFD));
			p.drawRoundedRect(emptyRect, kRowRadius, kRowRadius);
			p.setPen(QColor(0x23, 0x2F, 0x3C));
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 12, -18, -18),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Все пункты сейчас скрыты из меню",
					"All actions are currently hidden"));
			p.setPen(QColor(0x67, 0x75, 0x84));
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				emptyRect.adjusted(18, 34, -18, -12),
				Qt::AlignLeft | Qt::AlignTop,
				RuEn(
					"Ниже остаётся restore-tray: можно вернуть любой скрытый пункт одним нажатием или сразу восстановить всё меню.",
					"The restore tray stays below: you can bring back any hidden item with one click or restore the whole menu at once."));
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
			p.setPen(QPen(QColor(0x35, 0xC3, 0x8F), 3));
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
			p.setPen(QColor(0x5B, 0x6A, 0x79));
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(0, hiddenSectionTop(), width(), 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				RuEn(
					"Скрытые элементы (%1): restore-tray" ,
					"Hidden items (%1): restore tray").arg(hiddenCount));
			for (const auto &chip : chips) {
				const auto hovered = chip.restoreAll
					? _hoveredRestoreAll
					: (chip.index == _hoveredHiddenEntry);
				p.setPen(Qt::NoPen);
				p.setBrush(chip.restoreAll
					? (hovered ? QColor(0xD8, 0xF1, 0xE5) : QColor(0xEB, 0xF7, 0xF1))
					: (hovered
						? QColor(chip.color.red(), chip.color.green(), chip.color.blue(), 48)
						: QColor(0xEC, 0xF5, 0xEF)));
				p.drawRoundedRect(chip.rect, 14, 14);
				p.setPen(chip.restoreAll
					? QColor(0x1C, 0x8B, 0x62)
					: (hovered ? QColor(0x1C, 0x8B, 0x62) : QColor(0x2A, 0x4B, 0x57)));
				p.setFont(st::normalFont->f);
				p.drawText(
					chip.rect.adjusted(12, 0, -12, 0),
					Qt::AlignLeft | Qt::AlignVCenter,
					chip.label);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto point = e->position().toPoint();
		if (_draggingRow >= 0) {
			_dragCurrentY = point.y();
			updateDragTarget(point.y());
			_hoveredRow = -1;
			_hoveredKind = ActionKind::None;
			_hoveredHiddenEntry = -1;
			_hoveredRestoreAll = false;
			update();
			return;
		}
		if ((_pressedRow >= 0)
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
		const auto point = e->position().toPoint();
		_pressedRow = -1;
		_pressedOnButton = false;

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
		updateHover(e->position().toPoint());
	}

	void leaveEventHook(QEvent *e) override {
		Q_UNUSED(e);
		if (_draggingRow >= 0) {
			return;
		}
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		_hoveredHiddenEntry = -1;
		_hoveredRestoreAll = false;
		unsetCursor();
		update();
	}

private:
	enum class ActionKind {
		None,
		ToggleVisible,
		MoveUp,
		MoveDown,
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
			auto chipWidth = std::min(
				std::max(96, fm.horizontalAdvance(label) + 28),
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
			QColor(0x35, 0xC3, 0x8F),
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
		const auto visibleRow = visibleRowForEntry(index);
		const auto visibleCount = int(visibleEntryIndexes().size());

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
		push(RuEn("Ниже", "Down"), ActionKind::MoveDown, (visibleRow + 1) < visibleCount);
		push(RuEn("Выше", "Up"), ActionKind::MoveUp, visibleRow > 0);
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
		p.setBrush(!active
			? QColor(0xE8, 0xEE, 0xF3)
			: destructive
				? (hovered ? QColor(0xF8, 0xD9, 0xD9) : QColor(0xFE, 0xEC, 0xEC))
				: (hovered ? QColor(0xD8, 0xF1, 0xE5) : QColor(0xEB, 0xF7, 0xF1)));
		p.drawRoundedRect(button.rect, 14, 14);
		p.setPen(!active
			? QColor(0xA0, 0xAD, 0xB8)
			: destructive
				? QColor(0xC2, 0x4C, 0x4C)
				: QColor(0x1C, 0x8B, 0x62));
		p.setFont(st::normalFont->f);
		p.drawText(button.rect, Qt::AlignCenter, button.label);
	}

	void paintDragHandle(Painter &p, const QRect &row, bool active) const {
		const auto handle = dragHandleRect(row);
		p.setPen(Qt::NoPen);
		p.setBrush(active ? QColor(0x35, 0xC3, 0x8F) : QColor(0xA7, 0xB3, 0xBE));
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
		p.setPen(Qt::NoPen);
		p.setBrush(floating
			? QColor(0xD9, 0xF4, 0xE8)
			: selected
				? QColor(0xE7, 0xF8, 0xEF)
				: hovered
					? QColor(0xF5, 0xF9, 0xFC)
					: QColor(0xFA, 0xFC, 0xFE));
		p.drawRoundedRect(row, kRowRadius, kRowRadius);

		paintDragHandle(p, row, hovered || selected || floating);
		p.setBrush(meta.color);
		p.drawEllipse(QRect(row.left() + 36, row.top() + 14, 36, 36));
		p.setPen(Qt::white);
		p.setFont(st::semiboldFont->f);
		p.drawText(
			QRect(row.left() + 36, row.top() + 14, 36, 36),
			Qt::AlignCenter,
			meta.glyph.left(1));

		p.setPen(QColor(0x23, 0x2F, 0x3C));
		p.setFont(st::semiboldTextStyle.font->f);
		p.drawText(
			QRect(row.left() + 84, row.top() + 12, row.width() - 242, 20),
			Qt::AlignLeft | Qt::AlignVCenter,
			meta.title);

		p.setPen(QColor(0x67, 0x75, 0x84));
		p.setFont(st::defaultTextStyle.font->f);
		p.drawText(
			QRect(row.left() + 84, row.top() + 34, row.width() - 242, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			entry.separator
				? RuEn("Виден как разделитель", "Visible as divider")
				: RuEn("Виден в меню", "Visible in menu"));

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
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
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
		if (!newRestoreAll && (newHiddenEntry < 0)) {
			for (const auto index : visibleEntryIndexes()) {
				if (!rowRect(index).contains(point)) {
					continue;
				}
				newRow = index;
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
			&& (newHiddenEntry == _hoveredHiddenEntry)
			&& (newRestoreAll == _hoveredRestoreAll)) {
			return;
		}
		_hoveredRow = newRow;
		_hoveredKind = newKind;
		_hoveredHiddenEntry = newHiddenEntry;
		_hoveredRestoreAll = newRestoreAll;
		if (_hoveredRestoreAll || (_hoveredHiddenEntry >= 0) || ((_hoveredRow >= 0) && (_hoveredKind != ActionKind::None))) {
			setCursor(Qt::PointingHandCursor);
		} else if (_hoveredRow >= 0) {
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
		case ActionKind::MoveUp: {
			const auto visibleRow = visibleRowForEntry(index);
			changed = (visibleRow > 0) && _state->moveVisibleEntry(visibleRow, visibleRow - 1);
			break;
		}
		case ActionKind::MoveDown: {
			const auto visibleRow = visibleRowForEntry(index);
			changed = (visibleRow >= 0)
				&& _state->moveVisibleEntry(visibleRow, visibleRow + 1);
			break;
		}
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
	int _hoveredHiddenEntry = -1;
	bool _hoveredRestoreAll = false;
	int _pressedRow = -1;
	bool _pressedOnButton = false;
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

struct ContextActionDescriptor {
	QString title;
	QString subtitle;
	QString glyph;
	QColor color;
};

[[nodiscard]] QString ContextSurfaceTitle(
		HistoryView::ContextMenuSurface surface) {
	return (surface == HistoryView::ContextMenuSurface::Selection)
		? RuEn(
			"Контекстное меню при выделении",
			"Selection context menu")
		: RuEn(
			"Обычное контекстное меню",
			"Message context menu");
}

[[nodiscard]] QString ContextLaneTitle(ContextEditorLane lane) {
	return (lane == ContextEditorLane::Strip)
		? RuEn("Нижняя полоска иконок", "Bottom icon strip")
		: RuEn("Основной список действий", "Main action list");
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
	using Id = Menu::Customization::ContextMenuItemId;
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
			.color = color,
		};
	};

	if (id == QString::fromLatin1(Id::SelectionCopy)) {
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
		_changes.fire({});
		return true;
	}

	[[nodiscard]] bool resetToDefaults() {
		return applyLayout(HistoryView::DefaultContextMenuCustomizationLayout());
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

private:
	[[nodiscard]] bool applyLayout(
			const HistoryView::ContextMenuCustomizationLayout &updated) {
		if (!HistoryView::SaveContextMenuCustomizationLayout(updated)) {
			return false;
		}
		_layout = HistoryView::LoadContextMenuCustomizationLayout();
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

	HistoryView::ContextMenuCustomizationLayout _layout;
	mutable rpl::event_stream<> _changes;
};

class ContextMenuPreview final : public Ui::RpWidget {
public:
	ContextMenuPreview(
		QWidget *parent,
		std::shared_ptr<ContextMenuEditorState> state)
	: RpWidget(parent)
	, _state(std::move(state)) {
		_state->changes() | rpl::start_with_next([=] {
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

		const auto visibleStrip = visibleEntries(surface, ContextEditorLane::Strip, 4);
		if (!visibleStrip.empty()) {
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
		_state->changes() | rpl::start_with_next([=] {
			clampSelection();
			update();
			updateGeometry();
		}, lifetime());
		clampSelection();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		const auto count = std::max(int(_state->entries(_surface, _lane).size()), 1);
		return 18 + (count * (kRowHeight + kRowGap));
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto &entries = _state->entries(_surface, _lane);
		if (entries.empty()) {
			return;
		}

		for (auto i = 0; i != int(entries.size()); ++i) {
			const auto row = rowRect(i);
			const auto hovered = (i == _hoveredRow);
			const auto selected = (i == _selected);
			const auto meta = DescribeContextAction(entries[i].id);

			p.setPen(Qt::NoPen);
			p.setBrush(selected
				? QColor(0xE7, 0xF8, 0xEF)
				: hovered
					? QColor(0xF5, 0xF9, 0xFC)
					: QColor(0xFA, 0xFC, 0xFE));
			p.drawRoundedRect(row, kRowRadius, kRowRadius);

			p.setBrush(meta.color);
			p.drawEllipse(QRect(row.left() + 14, row.top() + 14, 36, 36));
			p.setPen(Qt::white);
			p.setFont(st::semiboldFont->f);
			p.drawText(
				QRect(row.left() + 14, row.top() + 14, 36, 36),
				Qt::AlignCenter,
				meta.glyph.left(1));

			p.setPen(QColor(0x23, 0x2F, 0x3C));
			p.setFont(st::semiboldTextStyle.font->f);
			p.drawText(
				QRect(row.left() + 62, row.top() + 12, row.width() - 220, 20),
				Qt::AlignLeft | Qt::AlignVCenter,
				meta.title);

			const auto stateText = entries[i].visible
				? (_lane == ContextEditorLane::Strip
					? RuEn("Показывается в нижней полоске", "Shown in the bottom strip")
					: RuEn("Показывается в контекстном меню", "Shown in the context menu"))
				: (_lane == ContextEditorLane::Strip
					? RuEn("Скрыто из нижней полоски", "Hidden from the bottom strip")
					: RuEn("Скрыто из контекстного меню", "Hidden from the context menu"));
			p.setPen(QColor(0x67, 0x75, 0x84));
			p.setFont(st::defaultTextStyle.font->f);
			p.drawText(
				QRect(row.left() + 62, row.top() + 34, row.width() - 220, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				stateText);

			for (const auto &button : actionButtonsForRow(i)) {
				paintActionButton(p, button);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		updateHover(e->position().toPoint());
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto point = e->position().toPoint();
		const auto &entries = _state->entries(_surface, _lane);
		for (auto i = 0; i != int(entries.size()); ++i) {
			const auto row = rowRect(i);
			if (!row.contains(point)) {
				continue;
			}
			_selected = i;
			for (const auto &button : actionButtonsForRow(i)) {
				if (!button.enabled || !button.rect.contains(point)) {
					continue;
				}
				handleAction(i, button.kind);
				updateHover(point);
				return;
			}
			update();
			return;
		}
	}

	void leaveEventHook(QEvent *e) override {
		Q_UNUSED(e);
		_hoveredRow = -1;
		_hoveredKind = ActionKind::None;
		update();
	}

private:
	enum class ActionKind {
		None,
		ToggleVisible,
		MoveUp,
		MoveDown,
	};

	struct ActionButton {
		QRect rect;
		QString label;
		ActionKind kind = ActionKind::None;
		bool enabled = true;
	};

	[[nodiscard]] QRect rowRect(int index) const {
		return QRect(
			0,
			8 + index * (kRowHeight + kRowGap),
			width(),
			kRowHeight);
	}

	[[nodiscard]] std::vector<ActionButton> actionButtonsForRow(int index) const {
		const auto &entries = _state->entries(_surface, _lane);
		if ((index < 0) || (index >= int(entries.size()))) {
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

		push(RuEn("Ниже", "Down"), ActionKind::MoveDown, index + 1 < int(entries.size()));
		push(RuEn("Выше", "Up"), ActionKind::MoveUp, index > 0);
		push(
			entries[index].visible
				? RuEn("Скрыть", "Hide")
				: RuEn("Показать", "Show"),
			ActionKind::ToggleVisible,
			true);
		std::reverse(result.begin(), result.end());
		return result;
	}

	void paintActionButton(QPainter &p, const ActionButton &button) const {
		const auto hovered = (_hoveredRow >= 0)
			&& (button.kind == _hoveredKind)
			&& button.rect.intersects(rowRect(_hoveredRow));
		p.setPen(Qt::NoPen);
		p.setBrush(!button.enabled
			? QColor(0xE8, 0xEE, 0xF3)
			: (hovered ? QColor(0xD8, 0xF1, 0xE5) : QColor(0xEB, 0xF7, 0xF1)));
		p.drawRoundedRect(button.rect, 14, 14);
		p.setPen(!button.enabled
			? QColor(0xA0, 0xAD, 0xB8)
			: QColor(0x1C, 0x8B, 0x62));
		p.setFont(st::normalFont->f);
		p.drawText(button.rect, Qt::AlignCenter, button.label);
	}

	void updateHover(QPoint point) {
		auto newRow = -1;
		auto newKind = ActionKind::None;
		const auto &entries = _state->entries(_surface, _lane);
		for (auto i = 0; i != int(entries.size()); ++i) {
			if (!rowRect(i).contains(point)) {
				continue;
			}
			newRow = i;
			for (const auto &button : actionButtonsForRow(i)) {
				if (button.enabled && button.rect.contains(point)) {
					newKind = button.kind;
					break;
				}
			}
			break;
		}
		if ((newRow == _hoveredRow) && (newKind == _hoveredKind)) {
			return;
		}
		_hoveredRow = newRow;
		_hoveredKind = newKind;
		update();
	}

	void handleAction(int index, ActionKind kind) {
		auto changed = false;
		switch (kind) {
		case ActionKind::ToggleVisible:
			changed = _state->toggleVisible(_surface, _lane, index);
			break;
		case ActionKind::MoveUp:
			changed = _state->moveUp(_surface, _lane, index);
			if (changed) {
				_selected = std::max(0, index - 1);
			}
			break;
		case ActionKind::MoveDown:
			changed = _state->moveDown(_surface, _lane, index);
			if (changed) {
				_selected = index + 1;
			}
			break;
		case ActionKind::None:
			break;
		}
		if (!changed) {
			if (kind == ActionKind::ToggleVisible
				&& _lane == ContextEditorLane::Strip) {
				_controller->window().showToast(RuEn(
					"В нижней полоске можно держать максимум 4 иконки.",
					"The bottom strip can show at most 4 icons."));
			} else {
				update();
			}
		}
	}

	void clampSelection() {
		const auto &entries = _state->entries(_surface, _lane);
		if (entries.empty()) {
			_selected = -1;
		} else if ((_selected < 0) || (_selected >= int(entries.size()))) {
			_selected = std::clamp(_selected, 0, int(entries.size()) - 1);
		}
	}

	const not_null<Window::SessionController*> _controller;
	const std::shared_ptr<ContextMenuEditorState> _state;
	const HistoryView::ContextMenuSurface _surface;
	const ContextEditorLane _lane = ContextEditorLane::Menu;
	int _selected = 0;
	int _hoveredRow = -1;
	ActionKind _hoveredKind = ActionKind::None;
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
			stream->events_starting_with(initial));
	button->toggledChanges() | rpl::on_next([=](bool toggled) {
		if (!apply(toggled)) {
			stream->fire_copy(!toggled);
			controller->window().showToast(RuEn(
				"Не удалось сохранить preview-настройку.",
				"Could not save the preview setting."));
			return;
		}
		stream->fire_copy(toggled);
	}, button->lifetime());
	if (!description.isEmpty()) {
		Ui::AddDividerText(container, rpl::single(description));
	}
}

} // namespace

void AddMenuCustomizationEditor(
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container) {
	const auto hasLogsAction = !Core::App().plugins().actionsFor(
		QStringLiteral("astro.show_logs")).empty();
	const auto state = std::make_shared<SideMenuEditorState>(
		controller->session().supportMode(),
		hasLogsAction);
	const auto contextState = std::make_shared<ContextMenuEditorState>();

	Ui::AddDivider(container);
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(RuEn(
				"Visual editor меню",
				"Menu visual editor")),
			st::boxLabel),
		st::defaultBoxDividerLabelPadding);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Экспериментальный editor уже редактирует боковое меню напрямую через `menu_layout.json`: можно скрывать пункты, переставлять их, добавлять свои разделители и сразу видеть это на fake Telegram preview.",
			"This experimental editor already works directly with `menu_layout.json`: you can hide items, reorder them, add custom dividers and immediately see the result on a fake Telegram preview.")));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);

	container->add(
		object_ptr<SideMenuPreview>(container, state),
		style::margins(6, 0, 6, 0),
		style::al_top);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);

	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Редактор бокового меню",
			"Side menu editor")));
	const auto list = container->add(
		object_ptr<SideMenuEntryList>(container, state),
		style::margins(6, 0, 6, 0),
		style::al_top);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Клик выбирает пункт, а зажатие и движение мышью реально переставляет его. Скрытые элементы ниже можно вернуть одним нажатием, справа остаются быстрые hide/show и fallback-кнопки вверх/вниз.",
			"Click selects an item, while press-and-drag now actually reorders it. Hidden items can be restored below with a single click, and the right side still keeps quick hide/show and fallback up/down buttons.")));
	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);

	AddButtonWithLabel(
		container,
		rpl::single(RuEn(
			"Файл раскладки бокового меню",
			"Side menu layout file")),
		rpl::single(QDir::toNativeSeparators(state->layoutPath())),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		QGuiApplication::clipboard()->setText(
			QDir::toNativeSeparators(state->layoutPath()));
		controller->window().showToast(RuEn(
			"Путь к layout-файлу скопирован.",
			"Layout file path copied."));
	});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Добавить пользовательский разделитель после выбранного пункта",
			"Add a custom divider after the selected item")),
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
			"Перечитать layout и preview-настройки с диска",
			"Reload layout and preview settings from disk")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		state->reloadFromDisk();
		list->setSelectedIndex(list->selectedIndex());
		controller->window().showToast(RuEn(
			"Editor перечитал файл с диска.",
			"Editor reloaded the file from disk."));
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
		{ &st::menuIconShow }
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

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Вид боковой панели",
			"Side panel presentation")));

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Показывать нижний footer-текст",
			"Show the footer text"),
		RuEn(
			"Применяется и в preview, и в реальном боковом меню: можно убрать нижний текстовый хвост, чтобы меню выглядело чище.",
			"Applies to both the preview and the real side menu: the bottom text footer can now be hidden for a cleaner shell."),
		state->showFooterText(),
		[=](bool value) {
			return state->setShowFooterText(value);
		});

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Профильный блок внизу бокового меню",
			"Move the profile block to the bottom"),
		RuEn(
			"Это уже реальный runtime-переключатель: профильный header можно держать сверху или переносить вниз, ближе к нижней части меню.",
			"This is now a real runtime switch: the profile header can stay at the top or move down closer to the lower part of the side menu."),
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

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Экспериментальные режимы оболочки",
			"Experimental shell modes")));

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Расширенная боковая панель",
			"Expanded side panel"),
		RuEn(
			"Runtime-хук уже подключён: реальное боковое меню становится шире. Preview тоже сразу повторяет это состояние.",
			"The runtime hook is now live: the real side menu becomes wider, and the preview mirrors that state immediately."),
		state->expandedSidePanel(),
		[=](bool value) {
			return state->setExpandedSidePanel(value);
		});

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Левоторцевые настройки",
			"Left-edge settings"),
		RuEn(
			"Честный MVP уже в runtime: settings/info layers выравниваются к левому краю вместо центрирования. Полное drawer-продолжение боковой панели всё ещё потребует отдельного рефактора.",
			"An honest runtime MVP is live: settings/info layers align to the left edge instead of staying centered. A full drawer-style continuation of the side menu still needs a separate refactor."),
		state->leftEdgeSettings(),
		[=](bool value) {
			return state->setLeftEdgeSettings(value);
		});

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Иммерсивная анимация бокового меню",
			"Immersive side menu animation"),
		RuEn(
			"Runtime-хук уже есть: основная секция клиента уезжает вправо вместе с открытием бокового меню. Полный drawer-level рефактор анимаций всего окна здесь пока не делается.",
			"The runtime hook is live: the main client section now shifts right together with the side menu opening. A full drawer-level refactor of the entire window animation is still out of scope here."),
		state->immersiveAnimation(),
		[=](bool value) {
			return state->setImmersiveAnimation(value);
		});

	AddPreviewToggle(
		controller,
		container,
		RuEn(
			"Более широкая панель настроек",
			"Wider settings pane"),
		RuEn(
			"Runtime-хук уже подключён для settings/info layers: у настроек появляется более широкий контейнер, чтобы длинные пункты не упирались в узкую колонку.",
			"The runtime hook is live for settings/info layers: settings now get a wider container so longer rows do not collapse into an overly narrow column."),
		state->wideSettingsPane(),
		[=](bool value) {
			return state->setWideSettingsPane(value);
		});

	AddButtonWithLabel(
		container,
		rpl::single(RuEn(
			"Файл preview-настроек editor-а",
			"Editor preview settings file")),
		rpl::single(QDir::toNativeSeparators(state->previewPrefsPath())),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		QGuiApplication::clipboard()->setText(
			QDir::toNativeSeparators(state->previewPrefsPath()));
		controller->window().showToast(RuEn(
			"Путь к preview-настройкам скопирован.",
			"Preview settings path copied."));
	});

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Следующие поверхности editor-а",
			"Next editor surfaces")));
	container->add(
		object_ptr<FutureSurfacesPreview>(container, state),
		style::margins(6, 0, 6, 0),
		style::al_top);
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Peer и 3-dot здесь пока остаются preview-only. Зато ниже уже живой runtime-editor контекстного меню: он пишет в `context_menu_layout.json`, разделяет обычное состояние и состояние выделения и отдельно управляет нижней полоской до 4 иконок.",
			"Peer and 3-dot remain preview-only here. But the section below is now a live runtime editor for the context menu: it writes into `context_menu_layout.json`, separates message and selection states and manages the bottom strip with up to 4 icons.")));

	Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(RuEn(
			"Runtime editor контекстного меню",
			"Runtime context menu editor")));
	Ui::AddDividerText(
		container,
		rpl::single(RuEn(
			"Эта часть уже реально влияет на клиент: сохранённая раскладка применяется к runtime context menu и к нижней полоске иконок при каждом открытии меню.",
			"This section already affects the real client: the saved layout is applied to the runtime context menu and the bottom icon strip every time the menu opens.")));
	container->add(
		object_ptr<ContextMenuPreview>(container, contextState),
		style::margins(6, 0, 6, 0),
		style::al_top);
	Ui::AddSkip(container, st::settingsCheckboxesSkip);

	const auto addContextEditor = [&](HistoryView::ContextMenuSurface surface,
			ContextEditorLane lane) {
		Ui::AddSubsectionTitle(
			container,
			rpl::single(ContextSurfaceTitle(surface) + u" · "_q + ContextLaneTitle(lane)));
		container->add(
			object_ptr<ContextMenuEntryList>(
				container,
				controller,
				contextState,
				surface,
				lane),
			style::margins(6, 0, 6, 0),
			style::al_top);
		Ui::AddDividerText(
			container,
			rpl::single(
				(lane == ContextEditorLane::Strip)
					? RuEn(
						"Hide/Show управляет попаданием в нижнюю полоску. Runtime жёстко держит лимит в 4 видимых иконки.",
						"Hide/Show controls whether the action appears in the bottom strip. Runtime strictly keeps the limit at 4 visible icons.")
					: RuEn(
						"Hide/Show и порядок здесь напрямую меняют обычный popup-список действий для этой поверхности.",
						"Hide/Show and ordering here directly change the popup action list for this surface.")));
		Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
	};

	addContextEditor(HistoryView::ContextMenuSurface::Message, ContextEditorLane::Menu);
	addContextEditor(HistoryView::ContextMenuSurface::Message, ContextEditorLane::Strip);
	addContextEditor(HistoryView::ContextMenuSurface::Selection, ContextEditorLane::Menu);
	addContextEditor(HistoryView::ContextMenuSurface::Selection, ContextEditorLane::Strip);

	AddButtonWithLabel(
		container,
		rpl::single(RuEn(
			"Файл раскладки контекстного меню",
			"Context menu layout file")),
		rpl::single(QDir::toNativeSeparators(contextState->layoutPath())),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->addClickHandler([=] {
		QGuiApplication::clipboard()->setText(
			QDir::toNativeSeparators(contextState->layoutPath()));
		controller->window().showToast(RuEn(
			"Путь к context layout скопирован.",
			"Context layout path copied."));
	});

	AddButtonWithIcon(
		container,
		rpl::single(RuEn(
			"Перечитать раскладку контекстного меню с диска",
			"Reload the context menu layout from disk")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->addClickHandler([=] {
		contextState->reloadFromDisk();
		controller->window().showToast(RuEn(
			"Контекстная раскладка перечитана с диска.",
			"Context layout reloaded from disk."));
	});

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

} // namespace Settings
