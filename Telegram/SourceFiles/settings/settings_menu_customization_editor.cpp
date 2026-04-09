/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_menu_customization_editor.h"

#include "settings/settings_common.h"
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
	, _preview(LoadShellModePreferences()) {
	}

	[[nodiscard]] const std::vector<Menu::Customization::SideMenuEntry> &entries()
	const {
		return _entries;
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

	[[nodiscard]] bool reloadFromDisk() {
		_entries = Menu::Customization::LoadSideMenuLayout(
			_supportMode,
			_includeShowLogs);
		_preview = LoadShellModePreferences();
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

	[[nodiscard]] bool moveUp(int index) {
		if (!hasIndex(index) || (index <= 0)) {
			return false;
		}
		auto updated = _entries;
		std::swap(updated[index], updated[index - 1]);
		return applyEntries(updated);
	}

	[[nodiscard]] bool moveDown(int index) {
		if (!hasIndex(index) || (index + 1 >= int(_entries.size()))) {
			return false;
		}
		auto updated = _entries;
		std::swap(updated[index], updated[index + 1]);
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

private:
	[[nodiscard]] bool applyEntries(
			const std::vector<Menu::Customization::SideMenuEntry> &updated) {
		if (!Menu::Customization::SaveSideMenuLayout(updated)) {
			return false;
		}
		_entries = updated;
		_changes.fire({});
		return true;
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
	ShellModePreferences _preview;
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

		p.setBrush(QColor(255, 255, 255, 20));
		p.drawRoundedRect(QRect(18, 18, menuWidth - 36, 68), 18, 18);
		p.setBrush(QColor(0x35, 0xC3, 0x8F));
		p.drawEllipse(QRect(30, 28, 42, 42));
		p.setPen(Qt::white);
		p.setFont(st::semiboldFont->f);
		p.drawText(QRect(30, 28, 42, 42), Qt::AlignCenter, u"A"_q);
		p.setPen(QColor(255, 255, 255, 230));
		p.setFont(st::semiboldTextStyle.font->f);
		p.drawText(88, 42, RuEn("Astrogram", "Astrogram"));
		p.setPen(QColor(255, 255, 255, 150));
		p.setFont(st::defaultTextStyle.font->f);
		p.drawText(88, 64, RuEn("Preview menu", "Preview menu"));

		auto hiddenCount = 0;
		for (const auto &entry : _state->entries()) {
			if (!entry.visible) {
				++hiddenCount;
			}
		}

		auto top = 102;
		auto shownRows = 0;
		for (const auto &entry : _state->entries()) {
			if (!entry.visible) {
				continue;
			}
			if (top > (height() - 56)) {
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
			p.setPen(QColor(255, 255, 255, 160));
			p.setFont(st::normalFont->f);
			p.drawText(
				QRect(24, height() - 34, menuWidth - 48, 18),
				Qt::AlignLeft | Qt::AlignVCenter,
				RuEn(
					"%1 скрыто в editor-е",
					"%1 hidden in the editor").arg(hiddenCount));
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
		if (_state->entries().empty()) {
			_selected = -1;
		} else {
			_selected = std::clamp(index, 0, int(_state->entries().size()) - 1);
		}
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		const auto count = std::max(int(_state->entries().size()), 1);
		return 18 + (count * (kRowHeight + kRowGap));
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);

		auto p = Painter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto &entries = _state->entries();
		if (entries.empty()) {
			return;
		}

		for (auto i = 0; i != int(entries.size()); ++i) {
			const auto row = rowRect(i);
			const auto hovered = (i == _hoveredRow);
			const auto selected = (i == _selected);
			const auto meta = DescribeEntry(entries[i], _state->supportMode());

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
				? (entries[i].separator
					? RuEn("Виден как разделитель", "Visible as divider")
					: RuEn("Виден в меню", "Visible in menu"))
				: (entries[i].separator
					? RuEn("Скрыт как разделитель", "Hidden divider")
					: RuEn("Скрыт из меню", "Hidden from menu"));
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
		for (auto i = 0; i != int(_state->entries().size()); ++i) {
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
		DeleteCustomSeparator,
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
		const auto &entries = _state->entries();
		if ((index < 0) || (index >= int(entries.size()))) {
			return {};
		}
		const auto row = rowRect(index);
		const auto &entry = entries[index];
		const auto fm = QFontMetrics(st::normalFont->f);
		const auto height = 28;
		auto right = row.right() - kRowPadding;
		auto result = std::vector<ActionButton>();

		const auto push = [&](QString label, ActionKind kind, bool enabled) {
			const auto width = std::max(
				44,
				fm.horizontalAdvance(label) + 22);
			right -= width;
			result.push_back(ActionButton{
				.rect = QRect(right, row.top() + 18, width, height),
				.label = std::move(label),
				.kind = kind,
				.enabled = enabled,
			});
			right -= 8;
		};

		if (entry.separator && IsCustomSeparatorId(entry.id)) {
			push(RuEn("Удалить", "Delete"), ActionKind::DeleteCustomSeparator, true);
		}
		push(RuEn("Ниже", "Down"), ActionKind::MoveDown, index + 1 < int(entries.size()));
		push(RuEn("Выше", "Up"), ActionKind::MoveUp, index > 0);
		push(
			entry.visible
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

	void updateHover(QPoint point) {
		auto newRow = -1;
		auto newKind = ActionKind::None;
		for (auto i = 0; i != int(_state->entries().size()); ++i) {
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
			changed = _state->toggleVisible(index);
			break;
		case ActionKind::MoveUp:
			changed = _state->moveUp(index);
			if (changed) {
				_selected = std::max(0, index - 1);
			}
			break;
		case ActionKind::MoveDown:
			changed = _state->moveDown(index);
			if (changed) {
				_selected = index + 1;
			}
			break;
		case ActionKind::DeleteCustomSeparator:
			changed = _state->removeCustomSeparator(index);
			if (changed) {
				_selected = std::min(
					_selected,
					int(_state->entries().size()) - 1);
			}
			break;
		case ActionKind::None:
			break;
		}
		if (!changed) {
			update();
		}
	}

	void clampSelection() {
		if (_state->entries().empty()) {
			_selected = -1;
		} else if ((_selected < 0) || (_selected >= int(_state->entries().size()))) {
			_selected = std::clamp(_selected, 0, int(_state->entries().size()) - 1);
		}
	}

	const std::shared_ptr<SideMenuEditorState> _state;
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
			"Клик по строке выбирает пункт. Справа доступны hide/show, перемещение вверх/вниз и удаление пользовательского разделителя.",
			"Click a row to select the item. The right side exposes hide/show, move up/down and custom divider removal.")));
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
			"Peer / 3-dot / context / bottom strip уже получили отдельный UI-slot в experimental settings, но пока остаются read-only. Для реального drag/drop и скрытия кнопок здесь нужны runtime hooks из `window_peer_menu.cpp`, `history_view_context_menu.cpp` и consumer-слой для нижней icon-strip.",
			"Peer / 3-dot / context / bottom strip now have a dedicated UI slot in experimental settings, but remain read-only. Real drag/drop and button hiding still need runtime hooks from `window_peer_menu.cpp`, `history_view_context_menu.cpp` and a consumer layer for the bottom icon strip.")));
	Ui::AddSkip(container, st::settingsCheckboxesSkip);
}

} // namespace Settings
