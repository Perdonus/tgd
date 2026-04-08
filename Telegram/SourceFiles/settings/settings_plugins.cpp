/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_plugins.h"
#include "settings/settings_common.h"
#include "settings/cloud_password/settings_cloud_password_common.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "logs.h"
#include "lang/lang_keys.h"
#include "lang/lang_text_entity.h"
#include "plugins/plugins_manager.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtCore/QTimer>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Settings {
namespace {

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

constexpr auto kPluginCardRadius = 20.;
constexpr auto kPluginCardVerticalMargin = 12;
constexpr auto kPluginCardContentInsetLeft = 14;
constexpr auto kPluginCardContentInsetRight = 10;
constexpr auto kPluginListRefreshDelayMs = 180;

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
	auto result = TextWithEntities{ trimmed };
	const auto offset = 0;
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

[[nodiscard]] TextWithEntities PluginCardMetaText(
		const ::Plugins::PluginState &state) {
	auto result = TextWithEntities();
	const auto version = state.info.version.trimmed();
	if (!version.isEmpty()) {
		result.text += version;
	}
	const auto author = PluginAuthorText(state.info.author);
	if (!author.text.isEmpty()) {
		if (!result.text.isEmpty()) {
			result.text += u" • "_q;
		}
		const auto offset = result.text.size();
		result.text += author.text;
		for (const auto &entity : author.entities) {
			result.entities.push_back(EntityInText(
				entity.type(),
				entity.offset() + offset,
				entity.length(),
				entity.data()));
		}
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

void AddPluginMetaText(
		not_null<Ui::VerticalLayout*> container,
		const TextWithEntities &text) {
	const auto label = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(text),
			st::defaultFlatLabel),
		style::margins(kPluginCardContentInsetLeft, 0, kPluginCardContentInsetRight, 0),
		style::al_top);
	WireExternalLinks(label);
}

void AddPluginMetaText(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	if (text.trimmed().isEmpty()) {
		return;
	}
	AddPluginMetaText(container, TextWithEntities{ text.trimmed() });
}

void AddPluginDescriptionText(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	if (text.trimmed().isEmpty()) {
		return;
	}
	const auto label = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(TextWithEntities{ text.trimmed() }),
			st::defaultFlatLabel),
		style::margins(
			kPluginCardContentInsetLeft,
			0,
			kPluginCardContentInsetRight,
			0),
		style::al_top);
	WireExternalLinks(label);
}

QString FormatPluginTitle(const ::Plugins::PluginState &state) {
	const auto &info = state.info;
	return !info.name.isEmpty()
		? info.name
		: (!info.id.isEmpty()
			? info.id
			: PluginUiText(u"Plugin"_q, u"Плагин"_q));
}

enum class PluginSourceBadgeMode {
	Card,
	Details,
};

QString PluginSourceBadgeText(const ::Plugins::PluginState &state) {
	return state.sourceVerified
		? PluginUiText(
			u"Verified source"_q,
			u"Подтверждённый источник"_q)
		: PluginUiText(
			u"Unverified source"_q,
			u"Неподтверждённый источник"_q);
}

QString PluginSourceOriginText(const ::Plugins::PluginState &state) {
	if (!state.sourceChannelId || (state.sourceMessageId <= 0)) {
		return QString();
	}
	return PluginUiText(
		u"Trusted channel %1 · post %2"_q,
		u"Доверенный канал %1 · пост %2"_q)
			.arg(QString::number(state.sourceChannelId))
			.arg(QString::number(state.sourceMessageId));
}

QString PluginSourceOriginKey(const ::Plugins::PluginState &state) {
	if (!state.sourceChannelId || (state.sourceMessageId <= 0)) {
		return QString();
	}
	return QString::number(state.sourceChannelId)
		+ u":"_q
		+ QString::number(state.sourceMessageId);
}

QString PluginSourceHashText(const ::Plugins::PluginState &state) {
	return state.sha256.isEmpty()
		? QString()
		: (PluginUiText(
			u"Exact SHA-256: "_q,
			u"Точный SHA-256: "_q) + state.sha256);
}

QString PluginSourceReasonCode(const ::Plugins::PluginState &state) {
	if (!state.sourceTrustReason.trimmed().isEmpty()) {
		return state.sourceTrustReason.trimmed();
	}
	return state.sourceVerified
		? QString()
		: state.sourceTrustDetails.trimmed();
}

QString PluginSourceRecordLabelText(const ::Plugins::PluginState &state) {
	if (!state.sourceVerified) {
		return QString();
	}
	const auto label = state.sourceTrustDetails.trimmed();
	if (label.isEmpty() || (label == PluginSourceOriginKey(state))) {
		return QString();
	}
	return PluginUiText(
		u"Trusted record label: %1"_q,
		u"Метка доверенной записи: %1"_q).arg(label);
}

QString PluginSourceExactMatchText(PluginSourceBadgeMode mode) {
	return (mode == PluginSourceBadgeMode::Card)
		? PluginUiText(
			u"Exact SHA-256 matches a trusted Astrogram source record."_q,
			u"Точный SHA-256 совпал с доверенной записью источника Astrogram."_q)
		: PluginUiText(
			u"This exact plugin binary matches a trusted Astrogram source record by SHA-256."_q,
			u"Точный бинарник этого плагина совпал с доверенной записью источника Astrogram по SHA-256."_q);
}

QString PluginSourceBadgeDetailText(
		const ::Plugins::PluginState &state,
		PluginSourceBadgeMode mode) {
	const auto addLine = [](QString base, const QString &line) {
		if (line.trimmed().isEmpty()) {
			return base;
		}
		return base.isEmpty()
			? line.trimmed()
			: (base + u"\n"_q + line.trimmed());
	};
	if (state.sourceVerified) {
		auto result = PluginSourceExactMatchText(mode);
		if (const auto label = PluginSourceRecordLabelText(state); !label.isEmpty()) {
			result = addLine(result, label);
		}
		if (const auto origin = PluginSourceOriginText(state); !origin.isEmpty()) {
			result = addLine(result, origin);
		}
		return result;
	}
	const auto reason = PluginSourceReasonCode(state);
	if (reason == u"sha256-unavailable"_q) {
		return PluginUiText(
			u"Could not compute the plugin SHA-256 hash."_q,
			u"Не удалось вычислить SHA-256 хеш плагина."_q);
	}
	if (reason == u"no-active-session"_q) {
		return PluginUiText(
			u"Trusted source records will become available after the active session finishes loading."_q,
			u"Доверенные записи источников станут доступны после полной загрузки активной сессии."_q);
	}
	if (reason == u"no-trusted-records"_q) {
		return (mode == PluginSourceBadgeMode::Card)
			? PluginUiText(
				u"The trusted source record list is still empty."_q,
				u"Список доверенных записей источников пока пуст."_q)
			: PluginUiText(
				u"No trusted Astrogram source records have been published yet, so this plugin cannot be verified."_q,
				u"Доверенные записи источников Astrogram ещё не опубликованы, поэтому этот плагин пока нельзя подтвердить."_q);
	}
	if (reason == u"no-valid-trusted-records"_q) {
		return (mode == PluginSourceBadgeMode::Card)
			? PluginUiText(
				u"Trusted source records exist, but they are malformed."_q,
				u"Доверенные записи источников существуют, но они повреждены."_q)
			: PluginUiText(
				u"Trusted source records were loaded, but none of them contain a valid exact SHA-256 entry."_q,
				u"Доверенные записи источников загрузились, но ни одна из них не содержит корректный exact SHA-256."_q);
	}
	if (reason == u"hash-found-in-untrusted-channel"_q) {
		auto result = (mode == PluginSourceBadgeMode::Card)
			? PluginUiText(
				u"Matching hash exists, but only outside the trusted source channel list."_q,
				u"Совпадающий хеш найден, но только вне списка доверенных каналов-источников."_q)
			: PluginUiText(
				u"A matching SHA-256 record exists, but it points to a channel that is not in the trusted source allowlist."_q,
				u"Совпадающая запись SHA-256 существует, но указывает на канал вне списка доверенных источников."_q);
		if (const auto origin = PluginSourceOriginText(state); !origin.isEmpty()) {
			result = addLine(result, origin);
		}
		return result;
	}
	if (reason == u"matching-record-missing-origin"_q) {
		return mode == PluginSourceBadgeMode::Card
			? PluginUiText(
				u"Matching hash record exists, but its source metadata is incomplete."_q,
				u"Совпадающая запись хеша есть, но у неё неполные метаданные источника."_q)
			: PluginUiText(
					u"A matching SHA-256 record exists, but it does not contain a valid trusted channel and post id."_q,
					u"Совпадающая запись SHA-256 существует, но в ней нет корректных идентификаторов доверенного канала и поста."_q);
	}
	if (mode == PluginSourceBadgeMode::Card) {
		return PluginUiText(
			u"This exact SHA-256 was not found in trusted Astrogram source records."_q,
			u"Точный SHA-256 не найден в доверенных записях источников Astrogram."_q);
	}
	auto result = PluginUiText(
		u"This exact plugin binary SHA-256 was not found in trusted Astrogram source records."_q,
		u"Точный SHA-256 этого бинарника не найден в доверенных записях источников Astrogram."_q);
	if (const auto origin = PluginSourceOriginText(state); !origin.isEmpty()) {
		result = addLine(result, origin);
	}
	return result;
}

void AddPluginSourceBadge(
		not_null<Ui::VerticalLayout*> container,
		const ::Plugins::PluginState &state,
		PluginSourceBadgeMode mode = PluginSourceBadgeMode::Card) {
	const auto badge = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(
			kPluginCardContentInsetLeft,
			(mode == PluginSourceBadgeMode::Card) ? 4 : 0,
			kPluginCardContentInsetRight,
			0),
		style::al_top);
	const auto text = PluginSourceBadgeText(state);
	const auto fill = state.sourceVerified
		? QColor(0x2e, 0xa4, 0xff, 36)
		: QColor(0xeb, 0x57, 0x57, 30);
	const auto border = state.sourceVerified
		? QColor(0x5c, 0xba, 0xff)
		: QColor(0xeb, 0x57, 0x57);
	const auto fg = state.sourceVerified
		? QColor(0x37, 0x8e, 0xff)
		: QColor(0xd8, 0x48, 0x48);
	const auto badgeHeight = st::semiboldFont->height + 12;
	container->widthValue() | rpl::on_next([=](int width) {
		badge->resize(std::max(0, width), badgeHeight);
	}, badge->lifetime());
	badge->resize(std::max(0, container->width()), badgeHeight);
	badge->paintRequest() | rpl::on_next([=] {
		auto p = Painter(badge);
		auto hq = PainterHighQualityEnabler(p);
		p.setFont(st::semiboldFont);
		const auto pillWidth = std::min(
			badge->width(),
			st::semiboldFont->width(text) + 22);
		const auto rect = QRectF(0, 0, pillWidth, badge->height() - 1)
			.adjusted(0.5, 0.5, -0.5, -0.5);
		p.setPen(QPen(border, 1.));
		p.setBrush(fill);
		p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
		p.setPen(fg);
		p.drawText(
			QRect(0, 0, pillWidth, badge->height()),
			Qt::AlignCenter,
			text);
	}, badge->lifetime());

	AddPluginDescriptionText(
		container,
		PluginSourceBadgeDetailText(state, mode));
	if (mode == PluginSourceBadgeMode::Details) {
		AddPluginMetaText(container, PluginSourceHashText(state));
	}
}

QString PluginDocsText() {
	return UseRussianPluginUi()
		? QString::fromUtf8(R"PLUGIN(Плагины Astrogram Desktop (техническая документация)

0) Кратко про архитектуру
- Плагин = нативная библиотека .tgd, загружается в процесс Astrogram Desktop.
- Любая ошибка ABI или native-crash в плагине может уронить процесс.
- Менеджер плагинов ведёт recovery-state и может включить safe mode.

1) Пути и файлы
- Папка плагинов: <working dir>/tdata/plugins
- Ручные выключения: <working dir>/tdata/plugins.json
- Флаг safe mode: <working dir>/tdata/plugins.safe-mode
- Основной лог: <working dir>/tdata/plugins.log
- Recovery-state: <working dir>/tdata/plugins.recovery.json

2) Минимальный каркас плагина
```cpp
#include "plugins/plugins_api.h"

class MyPlugin final : public Plugins::Plugin {
public:
	explicit MyPlugin(Plugins::Host *host) : _host(host) {}
	Plugins::PluginInfo info() const override {
		return {
			.id = "example.my_plugin",
			.name = "My Plugin",
			.version = "1.0.0",
			.author = "You",
			.description = "Example plugin",
		};
	}
	void onLoad() override {}
	void onUnload() override {}
private:
	Plugins::Host *_host = nullptr;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) return nullptr;
	return new MyPlugin(host);
}
```

3) Preview metadata (без запуска кода плагина)
```cpp
TGD_PLUGIN_PREVIEW(
	"example.my_plugin",
	"My Plugin",
	"1.0.0",
	"You",
	"Example plugin",
	"https://example.com",
	"GusTheDuck/4")
```

4) Регистрация slash-команды
```cpp
_commandId = _host->registerCommand(
	"example.my_plugin",
	{ .command = "/ping", .description = "Ping command" },
	[=](const Plugins::CommandContext &ctx) {
		_host->showToast("pong");
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Cancel
		};
	});
```

5) Кнопка-действие в Settings > Plugins
```cpp
_actionId = _host->registerAction(
	"example.my_plugin",
	"Open popup",
	"Opens a toast",
	[=] { _host->showToast("Action called"); });
```

6) Action с контекстом (окно/сессия)
```cpp
_actionCtxId = _host->registerActionWithContext(
	"example.my_plugin",
	"Context action",
	"Uses active window/session",
	[=](const Plugins::ActionContext &ctx) {
		if (!ctx.window) return;
		_host->showToast("Window is available");
	});
```

7) Host-rendered settings page
```cpp
Plugins::SettingDescriptor slider;
slider.id = "opacity";
slider.title = "Window opacity";
slider.type = Plugins::SettingControl::IntSlider;
slider.intValue = 85;
slider.intMinimum = 20;
slider.intMaximum = 100;
slider.intStep = 1;
slider.valueSuffix = "%";

Plugins::SettingsSectionDescriptor section;
section.id = "appearance";
section.title = "Appearance";
section.settings.push_back(slider);

_settingsPageId = _host->registerSettingsPage(
	"example.my_plugin",
	{ .id = "my_plugin", .title = "My Plugin", .sections = { section } },
	[=](const Plugins::SettingDescriptor &setting) {
		if (setting.id == "opacity") {
			_host->showToast(QString::number(setting.intValue));
		}
	});
```

Старый `registerPanel()` всё ещё доступен для legacy UI, но сырой plugin-owned dialog значительно менее надёжен, чем host-rendered controls на странице Settings > Plugins.

8) Перехват исходящего текста
```cpp
_outgoingId = _host->registerOutgoingTextInterceptor(
	"example.my_plugin",
	[=](const Plugins::OutgoingTextContext &ctx) {
		if (ctx.text.startsWith("/shout ")) {
			_host->showToast("Intercepted");
			return Plugins::CommandResult{
				.action = Plugins::CommandResult::Action::Cancel
			};
		}
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Continue
		};
	},
	/*priority=*/100);
```

9) Observer новых/изменённых/удалённых сообщений
```cpp
Plugins::MessageObserverOptions opts;
opts.newMessages = true;
opts.editedMessages = true;
opts.deletedMessages = true;
opts.incoming = true;
opts.outgoing = true;

_observerId = _host->registerMessageObserver(
	"example.my_plugin",
	opts,
	[=](const Plugins::MessageEventContext &ctx) {
		switch (ctx.event) {
		case Plugins::MessageEvent::New: _host->showToast("New"); break;
		case Plugins::MessageEvent::Edited: _host->showToast("Edited"); break;
		case Plugins::MessageEvent::Deleted: _host->showToast("Deleted"); break;
		}
	});
```

10) Window/session callbacks
```cpp
_host->onWindowCreated([=](Window::Controller *window) {
	Q_UNUSED(window);
	_host->showToast("Window created");
});

_host->onWindowWidgetCreated([=](QWidget *widget) {
	if (widget && widget->isWindow()) {
		widget->setWindowOpacity(0.85);
	}
});

	_host->onSessionActivated([=](Main::Session *session) {
		Q_UNUSED(session);
		_host->showToast("Session activated");
	});
```

11) Runtime API / HostInfo (всегда видимо в документации)
- `host->hostInfo()` всегда содержит поля `runtimeApiEnabled`, `runtimeApiPort`, `runtimeApiBaseUrl`.
- Даже если runtime API выключен, эти поля остаются частью контракта HostInfo и описаны здесь без скрытых unlock-жестов.
- Для диагностики также доступны `systemInfo()`, `workingPath` и `pluginsPath`.

12) Корректная очистка в onUnload
```cpp
void onUnload() override {
	if (_commandId) _host->unregisterCommand(_commandId);
	if (_actionId) _host->unregisterAction(_actionId);
	if (_panelId) _host->unregisterPanel(_panelId);
	if (_outgoingId) _host->unregisterOutgoingTextInterceptor(_outgoingId);
	if (_observerId) _host->unregisterMessageObserver(_observerId);
}
```

13) CMake пример для плагина
```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

14) ABI/совместимость (обязательно)
- platform, pointer size, compiler ABI, Qt major/minor, plugin API version должны совпадать.
- Простое переименование файла в `.tgd` не работает: нужен реальный compile+link.
- Несовместимость пишется в plugins.log как load-failed/abi-mismatch.

15) Safe mode и recovery
- При падении в рискованной операции (load/onload/panel/command/window/session/observer...) менеджер включает safe mode.
- Подозрительный плагин выключается автоматически.
- На следующем запуске появляется recovery-уведомление.

16) Диагностика: что смотреть сначала
1. plugins.log: `load-failed`, `abi-mismatch`, `onload failed`, `panel failed`.
2. Путь и SHA пакета в логе.
3. Совпадение compiler + Qt.
4. Не храните long-lived сырые указатели на объекты Astrogram.
5. Уберите тяжёлую синхронную работу из callback'ов UI.

17) Практика надёжности
- `info()` и конструктор плагина должны быть дешёвыми и без I/O.
- Любые сетевые/тяжёлые операции уносите в worker.
- UI код открывайте только из UI callback'ов.
- Всегда тестируйте onUnload после reload/disable.

18) Runtime API и host info
- Runtime API больше не скрывается за developer easter egg и считается частью публичной документации.
- Поля `hostInfo().runtimeApiEnabled`, `hostInfo().runtimeApiPort`, `hostInfo().runtimeApiBaseUrl` всегда видны плагину.
- Если runtime API выключен, пустые/нулевые значения — нормальное состояние.
- Проверяйте `runtimeApiEnabled`, а не наличие «секретной» кнопки или серии кликов по документации.
)PLUGIN")
		: QString::fromUtf8(R"PLUGIN(Astrogram Desktop Plugins (technical documentation)

0) Architecture
- Plugin = native .tgd shared library loaded into Astrogram Desktop process.
- ABI mismatch or native crash in plugin code can crash the process.
- Plugin manager stores recovery-state and can auto-enable safe mode.

1) Paths
- Plugin folder: <working dir>/tdata/plugins
- Manual disable list: <working dir>/tdata/plugins.json
- Safe mode flag: <working dir>/tdata/plugins.safe-mode
- Main log: <working dir>/tdata/plugins.log
- Recovery state: <working dir>/tdata/plugins.recovery.json

2) Minimal plugin skeleton
```cpp
#include "plugins/plugins_api.h"

class MyPlugin final : public Plugins::Plugin {
public:
	explicit MyPlugin(Plugins::Host *host) : _host(host) {}
	Plugins::PluginInfo info() const override {
		return {
			.id = "example.my_plugin",
			.name = "My Plugin",
			.version = "1.0.0",
			.author = "You",
			.description = "Example plugin",
		};
	}
	void onLoad() override {}
	void onUnload() override {}
private:
	Plugins::Host *_host = nullptr;
};

TGD_PLUGIN_ENTRY {
	if (apiVersion != Plugins::kApiVersion) return nullptr;
	return new MyPlugin(host);
}
```

3) Preview metadata export
```cpp
TGD_PLUGIN_PREVIEW(
	"example.my_plugin",
	"My Plugin",
	"1.0.0",
	"You",
	"Example plugin",
	"https://example.com",
	"GusTheDuck/4")
```

4) Slash command
```cpp
_commandId = _host->registerCommand(
	"example.my_plugin",
	{ .command = "/ping", .description = "Ping command" },
	[=](const Plugins::CommandContext &ctx) {
		_host->showToast("pong");
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Cancel
		};
	});
```

5) Action button
```cpp
_actionId = _host->registerAction(
	"example.my_plugin",
	"Open popup",
	"Opens a toast",
	[=] { _host->showToast("Action called"); });
```

6) Action with context
```cpp
_actionCtxId = _host->registerActionWithContext(
	"example.my_plugin",
	"Context action",
	"Uses active window/session",
	[=](const Plugins::ActionContext &ctx) {
		if (!ctx.window) return;
		_host->showToast("Window is available");
	});
```

7) Host-rendered settings page
```cpp
Plugins::SettingDescriptor slider;
slider.id = "opacity";
slider.title = "Window opacity";
slider.type = Plugins::SettingControl::IntSlider;
slider.intValue = 85;
slider.intMinimum = 20;
slider.intMaximum = 100;
slider.intStep = 1;
slider.valueSuffix = "%";

Plugins::SettingsSectionDescriptor section;
section.id = "appearance";
section.title = "Appearance";
section.settings.push_back(slider);

_settingsPageId = _host->registerSettingsPage(
	"example.my_plugin",
	{ .id = "my_plugin", .title = "My Plugin", .sections = { section } },
	[=](const Plugins::SettingDescriptor &setting) {
		if (setting.id == "opacity") {
			_host->showToast(QString::number(setting.intValue));
		}
	});
```

Legacy `registerPanel()` is still available for plugin-owned UI, but raw native dialogs are less stable than host-rendered controls in Settings > Plugins.

8) Outgoing text interceptor
```cpp
_outgoingId = _host->registerOutgoingTextInterceptor(
	"example.my_plugin",
	[=](const Plugins::OutgoingTextContext &ctx) {
		if (ctx.text.startsWith("/shout ")) {
			_host->showToast("Intercepted");
			return Plugins::CommandResult{
				.action = Plugins::CommandResult::Action::Cancel
			};
		}
		return Plugins::CommandResult{
			.action = Plugins::CommandResult::Action::Continue
		};
	},
	/*priority=*/100);
```

9) Message observer
```cpp
Plugins::MessageObserverOptions opts;
opts.newMessages = true;
opts.editedMessages = true;
opts.deletedMessages = true;
opts.incoming = true;
opts.outgoing = true;

_observerId = _host->registerMessageObserver(
	"example.my_plugin",
	opts,
	[=](const Plugins::MessageEventContext &ctx) {
		switch (ctx.event) {
		case Plugins::MessageEvent::New: _host->showToast("New"); break;
		case Plugins::MessageEvent::Edited: _host->showToast("Edited"); break;
		case Plugins::MessageEvent::Deleted: _host->showToast("Deleted"); break;
		}
	});
```

10) Window/session callbacks
```cpp
_host->onWindowCreated([=](Window::Controller *window) {
	Q_UNUSED(window);
	_host->showToast("Window created");
});

_host->onWindowWidgetCreated([=](QWidget *widget) {
	if (widget && widget->isWindow()) {
		widget->setWindowOpacity(0.85);
	}
});

_host->onSessionActivated([=](Main::Session *session) {
	Q_UNUSED(session);
	_host->showToast("Session activated");
});
```

11) Runtime API / HostInfo (always visible in docs)
- `host->hostInfo()` always exposes `runtimeApiEnabled`, `runtimeApiPort`, and `runtimeApiBaseUrl`.
- Even when runtime API is disabled, these fields stay part of the host contract and are documented here without hidden unlock gestures.
- `systemInfo()`, `workingPath`, and `pluginsPath` are part of the runtime diagnostics surface.

12) onUnload cleanup
```cpp
void onUnload() override {
	if (_commandId) _host->unregisterCommand(_commandId);
	if (_actionId) _host->unregisterAction(_actionId);
	if (_panelId) _host->unregisterPanel(_panelId);
	if (_outgoingId) _host->unregisterOutgoingTextInterceptor(_outgoingId);
	if (_observerId) _host->unregisterMessageObserver(_observerId);
}
```

13) CMake sample
```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_include_directories(my_plugin PRIVATE ${TGD_PLUGIN_API_DIR})
target_link_libraries(my_plugin PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(my_plugin PROPERTIES SUFFIX ".tgd")
```

14) ABI checklist
- Match platform, architecture, compiler ABI, Qt major/minor, plugin API version.
- Renaming files to `.tgd` is not enough; you must compile/link.
- ABI failures are logged as load-failed/abi-mismatch in plugins.log.

15) Recovery/safe mode
- If Astrogram detects crash risk in plugin operation, it enables safe mode.
- Suspected plugin is disabled automatically.
- Recovery notice appears on next start.

16) Debug order
1. Check plugins.log (`load-failed`, `abi-mismatch`, `onload failed`, `panel failed`).
2. Verify package path and SHA in log.
3. Verify compiler + Qt match.
4. Avoid long-lived raw pointers to Astrogram internals.
5. Move heavy sync work out of UI callbacks.

17) Runtime API and host info
- Runtime API is no longer hidden behind a developer easter egg and is treated as public documentation.
- `hostInfo().runtimeApiEnabled`, `hostInfo().runtimeApiPort`, `hostInfo().runtimeApiBaseUrl` are always visible to the plugin.
- Empty / zero values are normal when runtime API is disabled.
- Check `runtimeApiEnabled`, not the presence of a hidden button or repeated taps on documentation.
)PLUGIN");
}

QString PluginRuntimeText() {
	const auto host = Core::App().plugins().hostInfo();
	const auto system = Core::App().plugins().systemInfo();

	auto lines = QStringList();
	lines.push_back(PluginUiText(
		u"Plugin Runtime & Diagnostics"_q,
		u"Рантайм и диагностика плагинов"_q));
	lines.push_back(QString());
	lines.push_back(PluginUiText(
		u"Host"_q,
		u"Хост"_q));
	lines.push_back(
		PluginUiText(u"App version: "_q, u"Версия приложения: "_q)
		+ host.appVersion);
	lines.push_back(
		PluginUiText(u"UI language: "_q, u"Язык интерфейса: "_q)
		+ host.appUiLanguage);
	lines.push_back(
		PluginUiText(u"Compiler: "_q, u"Компилятор: "_q)
		+ host.compiler
		+ u" "_q
		+ QString::number(host.compilerVersion));
	lines.push_back(
		PluginUiText(u"Platform: "_q, u"Платформа: "_q)
		+ host.platform
		+ u" • Qt "_q
		+ QString::number(host.qtMajor)
		+ u"."_q
		+ QString::number(host.qtMinor));
	lines.push_back(
		PluginUiText(u"Working dir: "_q, u"Рабочая папка: "_q)
		+ QDir::toNativeSeparators(host.workingPath));
	lines.push_back(
		PluginUiText(u"Plugins dir: "_q, u"Папка плагинов: "_q)
		+ QDir::toNativeSeparators(host.pluginsPath));
	lines.push_back(
		PluginUiText(u"Safe mode: "_q, u"Безопасный режим: "_q)
		+ PluginUiText(
			host.safeModeEnabled ? u"enabled"_q : u"disabled"_q,
			host.safeModeEnabled ? u"включён"_q : u"выключен"_q));
	lines.push_back(
		PluginUiText(u"Runtime API: "_q, u"Runtime API: "_q)
		+ PluginUiText(
			host.runtimeApiEnabled ? u"enabled"_q : u"disabled"_q,
			host.runtimeApiEnabled ? u"включён"_q : u"выключен"_q));
	if (host.runtimeApiEnabled) {
		lines.push_back(
			PluginUiText(u"Runtime port: "_q, u"Порт runtime: "_q)
			+ QString::number(host.runtimeApiPort));
		lines.push_back(
			PluginUiText(u"Runtime base URL: "_q, u"Base URL runtime: "_q)
			+ host.runtimeApiBaseUrl);
	}
#if defined(_WIN32)
	lines.push_back(
		PluginUiText(
			u"Runtime CLI: astro (installed to PATH on startup)"_q,
			u"Runtime CLI: astro (добавляется в PATH при запуске)"_q));
#endif // _WIN32

	lines.push_back(QString());
	lines.push_back(PluginUiText(
		u"System"_q,
		u"Система"_q));
	lines.push_back(
		PluginUiText(u"OS: "_q, u"ОС: "_q)
		+ system.prettyProductName);
	lines.push_back(
		PluginUiText(u"Kernel: "_q, u"Ядро: "_q)
		+ system.kernelType
		+ u" "_q
		+ system.kernelVersion);
	lines.push_back(
		PluginUiText(u"Architecture: "_q, u"Архитектура: "_q)
		+ system.architecture
		+ u" • "_q
		+ system.buildAbi);
	lines.push_back(
		PluginUiText(u"CPU cores: "_q, u"Ядра CPU: "_q)
		+ QString::number(system.logicalCpuCores)
		+ u" / "_q
		+ QString::number(system.physicalCpuCores));
	lines.push_back(
		PluginUiText(u"Locale: "_q, u"Локаль: "_q)
		+ system.locale
		+ u" • "_q
		+ system.uiLanguage);
	lines.push_back(
		PluginUiText(u"Time zone: "_q, u"Часовой пояс: "_q)
		+ system.timeZone);
	lines.push_back(
		PluginUiText(u"User: "_q, u"Пользователь: "_q)
		+ system.userName
		+ u" @ "_q
		+ system.hostName);
	return lines.join(u"\n"_q);
}

void ShowPluginDocsBox(not_null<Window::SessionController*> controller) {
	const auto text = PluginDocsText();
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			PluginUiText(u"Plugin Documentation"_q, u"Документация плагинов"_q)));
		box->addLeftButton(
			rpl::single(PluginUiText(u"Copy"_q, u"Копировать"_q)),
			[=] {
				if (const auto clipboard = QGuiApplication::clipboard()) {
					clipboard->setText(text);
				}
				controller->window().showToast(PluginUiText(
					u"Documentation copied."_q,
					u"Документация скопирована."_q));
			});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				0),
			style::al_top);
	}));
}

void ShowPluginRuntimeBox(not_null<Window::SessionController*> controller) {
	const auto text = PluginRuntimeText();
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(
			PluginUiText(
				u"Plugin Runtime"_q,
				u"Рантайм плагинов"_q)));
		box->addLeftButton(
			rpl::single(PluginUiText(u"Copy"_q, u"Копировать"_q)),
			[=] {
				if (const auto clipboard = QGuiApplication::clipboard()) {
					clipboard->setText(text);
				}
				controller->window().showToast(PluginUiText(
					u"Runtime info copied."_q,
					u"Информация о рантайме скопирована."_q));
			});
		const auto enabled = Core::App().plugins().runtimeApiEnabled();
		box->addButton(
			rpl::single(enabled
				? PluginUiText(u"Disable API"_q, u"Выключить API"_q)
				: PluginUiText(u"Enable API"_q, u"Включить API"_q)),
			[=] {
				if (!Core::App().plugins().setRuntimeApiEnabled(!enabled)) {
					controller->window().showToast(PluginUiText(
						u"Could not change runtime API state."_q,
						u"Не удалось изменить состояние runtime API."_q));
					return;
				}
				controller->window().showToast(!enabled
					? PluginUiText(
						u"Runtime API enabled."_q,
						u"Runtime API включён."_q)
					: PluginUiText(
						u"Runtime API disabled."_q,
						u"Runtime API выключен."_q));
				box->closeBox();
			});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel),
			style::margins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				0),
			style::al_top);
	}));
}

void OpenPluginsFolder() {
	File::ShowInFolder(Core::App().plugins().pluginsPath());
}

[[nodiscard]] QColor PluginCardBackgroundColor(
		const ::Plugins::PluginState &state) {
	auto color = st::windowBgOver->c;
	if (state.recoverySuspected || state.disabledByRecovery) {
		color = st::attentionButtonFg->c;
		color.setAlpha(18);
	} else if (!state.error.isEmpty()) {
		color = st::windowBgActive->c;
		color.setAlpha(18);
	}
	return color;
}

[[nodiscard]] QColor PluginCardBorderColor(
		const ::Plugins::PluginState &state) {
	if (state.recoverySuspected || state.disabledByRecovery) {
		return st::attentionButtonFg->c;
	} else if (!state.error.isEmpty()) {
		return st::windowBgActive->c;
	}
	auto color = st::windowBgOver->c;
	color.setAlpha(0);
	return color;
}

[[nodiscard]] not_null<Ui::VerticalLayout*> AddPluginCardContainer(
		not_null<Ui::VerticalLayout*> container,
		::Plugins::PluginState state) {
	const auto card = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(20, 0, 20, 0),
		style::al_top);
	const auto raw = static_cast<Ui::RpWidget*>(card);
	const auto inner = Ui::CreateChild<Ui::VerticalLayout>(raw);
	const auto margins = QMargins(
		2,
		8,
		2,
		8);

	raw->widthValue() | rpl::on_next([=](int width) {
		const auto innerWidth = std::max(0, width - margins.left() - margins.right());
		inner->resizeToWidth(innerWidth);
		inner->move(margins.left(), margins.top());
	}, raw->lifetime());
	inner->heightValue() | rpl::on_next([=](int height) {
		raw->resize(
			raw->width(),
			height + margins.top() + margins.bottom());
	}, raw->lifetime());
	raw->paintRequest() | rpl::on_next([=] {
		auto p = Painter(raw);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(PluginCardBorderColor(state), state.recoverySuspected ? 1.5 : 1.0));
		p.setBrush(PluginCardBackgroundColor(state));
		const auto rect = QRectF(raw->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		p.drawRoundedRect(rect, kPluginCardRadius, kPluginCardRadius);
	}, raw->lifetime());

	return inner;
}

QString FormatPluginCardSummary(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	const auto &info = state.info;
	if (!info.description.trimmed().isEmpty()) {
		lines.push_back(info.description.trimmed());
	}
	return lines.join(u"\n"_q);
}

void SharePluginPackage(
		not_null<Window::SessionController*> controller,
		::Plugins::PluginState state);

void RequestPluginRemoval(
		not_null<Window::SessionController*> controller,
		::Plugins::PluginState state,
		Fn<void()> onRemoved);

void DestroyLayoutChildrenSynchronously(not_null<Ui::VerticalLayout*> layout) {
	const auto children = layout->findChildren<QWidget*>(
		QString(),
		Qt::FindDirectChildrenOnly);
	for (const auto child : children) {
		delete child;
	}
}

void AddPluginCardActionRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		::Plugins::PluginState state,
		Fn<void()> onChanged) {
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(kPluginCardContentInsetLeft, 10, kPluginCardContentInsetRight, 2),
		style::al_top);
	const auto raw = static_cast<Ui::RpWidget*>(row);
	const auto settings = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarSettings);
	Ui::IconButton *share = nullptr;
	const auto remove = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarDelete);

	settings->setClickedCallback([=] {
		controller->showSettings(PluginDetailsId(state.info.id));
	});
	if (!state.path.trimmed().isEmpty()) {
		share = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarForward);
		share->setClickedCallback([=] {
			SharePluginPackage(controller, state);
		});
	}
	remove->setClickedCallback([=] {
		RequestPluginRemoval(controller, state, onChanged);
	});

	const auto buttonHeight = std::max({
		settings->height(),
		share ? share->height() : 0,
		remove->height(),
	});
	raw->resize(raw->width(), buttonHeight);
	raw->setMinimumHeight(buttonHeight);
	raw->setMaximumHeight(buttonHeight);

	raw->widthValue() | rpl::on_next([=](int) {
		const auto gap = 8;
		auto buttons = std::vector<Ui::IconButton*>{ settings };
		if (share) {
			buttons.push_back(share);
		}
		buttons.push_back(remove);
		auto left = 0;
		const auto top = std::max(0, raw->height() - buttonHeight - 1);
		for (const auto current : buttons) {
			current->move(left, top);
			left += current->width() + gap;
		}
	}, raw->lifetime());
}

void RevealPluginAuxFile(
		not_null<Window::SessionController*> controller,
		const QString &path,
		const QString &errorText) {
	if (!QFileInfo(path).exists()) {
		controller->window().showToast(errorText);
		return;
	}
	File::ShowInFolder(path);
}

std::optional<::Plugins::PluginState> LookupPluginState(
		const QString &pluginId) {
	for (const auto &state : Core::App().plugins().plugins()) {
		if (state.info.id == pluginId) {
			return state;
		}
	}
	return std::nullopt;
}

void SharePluginPackage(
		not_null<Window::SessionController*> controller,
		::Plugins::PluginState state) {
	if (state.path.trimmed().isEmpty()) {
		controller->window().showToast(PluginUiText(
			u"Plugin file path is unavailable."_q,
			u"Путь к файлу плагина недоступен."_q));
		return;
	}
	if (const auto clipboard = QGuiApplication::clipboard()) {
		clipboard->setText(QDir::toNativeSeparators(state.path));
	}
	File::ShowInFolder(state.path);
	controller->window().showToast(PluginUiText(
		u"Plugin package path copied and file revealed."_q,
		u"Путь к пакету плагина скопирован, файл показан."_q));
}

void RequestPluginRemoval(
		not_null<Window::SessionController*> controller,
		::Plugins::PluginState state,
		Fn<void()> onRemoved) {
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(PluginUiText(u"Delete plugin"_q, u"Удалить плагин"_q)));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(PluginUiText(
				u"Delete plugin \"%1\"?"_q,
				u"Удалить плагин \"%1\"?"_q).arg(FormatPluginTitle(state))),
			st::boxLabel),
			style::margins(st::boxPadding.left(), 0, st::boxPadding.right(), 0),
			style::al_top);
		box->addButton(rpl::single(PluginUiText(u"Delete"_q, u"Удалить"_q)), [=] {
			QString error;
			if (!Core::App().plugins().removePlugin(state.info.id, &error)) {
				controller->window().showToast(
					error.isEmpty()
						? PluginUiText(
							u"Could not delete the plugin."_q,
							u"Не удалось удалить плагин."_q)
						: error);
				return;
			}
			box->closeBox();
			if (onRemoved) {
				QTimer::singleShot(0, controller, [=] {
					onRemoved();
				});
			}
		});
		box->addButton(rpl::single(tr::lng_cancel()), [=] {
			box->closeBox();
		});
	}));
}

void RequestSafeModeChange(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> context,
		bool enabled,
		Fn<void()> onStateChanged) {
	const auto title = enabled
		? PluginUiText(u"Enable plugin safe mode?"_q, u"Включить безопасный режим плагинов?"_q)
		: PluginUiText(u"Disable plugin safe mode?"_q, u"Выключить безопасный режим плагинов?"_q);
	const auto text = enabled
		? PluginUiText(
			u"Astrogram will unload all plugins and reopen the plugin list in metadata-only mode."_q,
			u"Astrogram выгрузит все плагины и откроет список плагинов в режиме только-метаданных."_q)
		: PluginUiText(
			u"Astrogram will try to load plugins again. Disable safe mode only if you trust the installed plugins."_q,
			u"Astrogram снова попробует загрузить плагины. Выключайте безопасный режим только если доверяете установленным плагинам."_q);
	controller->show(Ui::MakeConfirmBox({
		.text = title + u"\n\n"_q + text,
		.confirmed = crl::guard(context, [=] {
			QTimer::singleShot(0, context, [=] {
				if (!Core::App().plugins().setSafeModeEnabled(enabled)) {
					controller->window().showToast(PluginUiText(
						u"Could not change safe mode."_q,
						u"Не удалось переключить безопасный режим."_q));
				}
				if (onStateChanged) {
					onStateChanged();
				}
			});
		}),
		.confirmText = enabled
			? PluginUiText(u"Enable"_q, u"Включить"_q)
			: PluginUiText(u"Disable"_q, u"Выключить"_q),
	}));
}

void AddPluginSettingsContent(
		not_null<Ui::VerticalLayout*> container,
		const ::Plugins::SettingsPageState &page,
		Fn<void()> onStateChanged) {
	if (!page.title.trimmed().isEmpty()) {
		Ui::AddSubsectionTitle(container, rpl::single(page.title.trimmed()));
	}
	if (!page.description.trimmed().isEmpty()) {
		Ui::AddDividerText(container, rpl::single(page.description.trimmed()));
	}
	for (const auto &section : page.sections) {
		if (!section.title.trimmed().isEmpty()) {
			Ui::AddSkip(container);
			Ui::AddSubsectionTitle(container, rpl::single(section.title.trimmed()));
		}
		if (!section.description.trimmed().isEmpty()) {
			Ui::AddDividerText(container, rpl::single(section.description.trimmed()));
		}
		for (const auto &setting : section.settings) {
			switch (setting.type) {
			case ::Plugins::SettingControl::Toggle: {
				const auto button = container->add(object_ptr<Ui::SettingsButton>(
					container,
					rpl::single(setting.title),
					st::settingsButtonNoIcon
				))->toggleOn(rpl::single(setting.boolValue));
				button->toggledChanges(
				) | rpl::filter([=](bool value) {
					return value != setting.boolValue;
				}) | rpl::on_next([=](bool value) {
					auto updated = setting;
					updated.boolValue = value;
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				}, button->lifetime());
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::IntSlider: {
				if (!setting.title.trimmed().isEmpty()) {
					Ui::AddSkip(container);
					Ui::AddSubsectionTitle(
						container,
						rpl::single(setting.title.trimmed()));
				}
				const auto minValue = std::min(
					setting.intMinimum,
					setting.intMaximum);
				const auto maxValue = std::max(
					setting.intMinimum,
					setting.intMaximum);
				const auto step = std::max(1, setting.intStep);
				const auto formatValue = [=](int value) {
					return QString::number(value) + setting.valueSuffix;
				};
				auto sliderWithLabel = MakeSliderWithLabel(
					container,
					st::settingsScale,
					st::settingsScaleLabel,
					st::normalFont->spacew * 2,
					std::max(
						st::settingsScaleLabel.style.font->width(
							formatValue(minValue)),
						st::settingsScaleLabel.style.font->width(
							formatValue(maxValue))),
					true);
				const auto slider = sliderWithLabel.slider;
				const auto valueLabel = sliderWithLabel.label;
				slider->setAccessibleName(setting.title);
				const auto valueFromSlider = [=](double raw) {
					if (maxValue <= minValue) {
						return minValue;
					}
					auto candidate = minValue + int(std::lround(
						raw * (maxValue - minValue)));
					candidate = minValue + int(std::lround(
						(candidate - minValue) / double(step))) * step;
					return std::clamp(candidate, minValue, maxValue);
				};
				const auto sliderFromValue = [=](int value) {
					if (maxValue <= minValue) {
						return 0.;
					}
					return (value - minValue) / double(maxValue - minValue);
				};
				valueLabel->setText(formatValue(setting.intValue));
				slider->setAdjustCallback([=](double raw) {
					return sliderFromValue(valueFromSlider(raw));
				});
				slider->setValue(sliderFromValue(setting.intValue));
				slider->setChangeProgressCallback([=](double raw) {
					valueLabel->setText(formatValue(valueFromSlider(raw)));
				});
				slider->setChangeFinishedCallback([=](double raw) {
					auto updated = setting;
					updated.intValue = valueFromSlider(raw);
					valueLabel->setText(formatValue(updated.intValue));
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				});
				container->add(
					std::move(sliderWithLabel.widget),
					st::settingsBigScalePadding);
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::TextInput: {
				if (!setting.title.trimmed().isEmpty()) {
					Ui::AddSkip(container);
					Ui::AddSubsectionTitle(
						container,
						rpl::single(setting.title.trimmed()));
				}
				const auto placeholder = setting.placeholderText.trimmed().isEmpty()
					? setting.title.trimmed()
					: setting.placeholderText.trimmed();
				const auto lastValue = std::make_shared<QString>(setting.textValue);
				const auto handleTextChange = [=](const QString &current) {
					if (current == *lastValue) {
						return;
					}
					*lastValue = current;
					auto updated = setting;
					updated.textValue = current;
					if (!Core::App().plugins().updateSetting(page.id, updated) && onStateChanged) {
						onStateChanged();
					}
				};
				if (setting.secret) {
					const auto field = CloudPassword::AddPasswordField(
						container,
						rpl::single(placeholder),
						setting.textValue);
					QObject::connect(field, &Ui::MaskedInputField::changed, [=] {
						handleTextChange(field->getLastText().trimmed());
					});
				} else {
					const auto field = CloudPassword::AddWrappedField(
						container,
						rpl::single(placeholder),
						setting.textValue);
					field->changes() | rpl::on_next([=] {
						handleTextChange(field->getLastText().trimmed());
					}, field->lifetime());
				}
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::ActionButton: {
				const auto buttonTitle = setting.buttonText.isEmpty()
					? setting.title
					: setting.buttonText;
				const auto button = container->add(object_ptr<Ui::SettingsButton>(
					container,
					rpl::single(buttonTitle),
					st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					if (!Core::App().plugins().updateSetting(page.id, setting) && onStateChanged) {
						onStateChanged();
					}
				});
				if (!setting.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						container,
						rpl::single(setting.description.trimmed()));
				}
			} break;
			case ::Plugins::SettingControl::InfoText: {
				const auto text = setting.description.trimmed().isEmpty()
					? setting.title.trimmed()
					: setting.description.trimmed();
				if (!text.isEmpty()) {
					Ui::AddDividerText(container, rpl::single(text));
				}
			} break;
			}
		}
	}
}

class PluginDetailsSection final : public AbstractSection {
public:
	PluginDetailsSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		QString pluginId,
		Type type)
	: AbstractSection(parent)
	, _controller(controller)
	, _pluginId(std::move(pluginId))
	, _type(std::move(type))
	, _content(Ui::CreateChild<Ui::VerticalLayout>(this)) {
		rebuild();
	}

	[[nodiscard]] Type id() const override {
		return _type;
	}

	[[nodiscard]] rpl::producer<QString> title() override {
		return rpl::single(_title);
	}

private:
	void rebuild() {
		_content->clear();
		Ui::AddDivider(_content);
		Ui::AddSkip(_content);

		const auto state = LookupPluginState(_pluginId);
		if (!state) {
			_title = PluginUiText(u"Plugin"_q, u"Плагин"_q);
			Ui::AddDividerText(
				_content,
				rpl::single(PluginUiText(
					u"Plugin was not found."_q,
					u"Плагин не найден."_q)));
			Ui::ResizeFitChild(this, _content);
			return;
		}

		_title = FormatPluginTitle(*state);
		const auto stateChanged = crl::guard(this, [=] { rebuild(); });

		AddPluginSourceBadge(_content, *state, PluginSourceBadgeMode::Details);
		Ui::AddSkip(_content);

		const auto actions = Core::App().plugins().actionsFor(state->info.id);
		const auto settingsPages = Core::App().plugins().settingsPagesFor(state->info.id);

		if (!actions.empty()) {
			Ui::AddSubsectionTitle(
				_content,
				rpl::single(PluginUiText(u"Actions"_q, u"Действия"_q)));
			for (const auto &action : actions) {
				Ui::AddSkip(_content);
				const auto button = _content->add(object_ptr<Ui::SettingsButton>(
					_content,
					rpl::single(action.title.trimmed().isEmpty()
						? PluginUiText(u"Run action"_q, u"Выполнить действие"_q)
						: action.title.trimmed()),
					st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					if (!Core::App().plugins().triggerAction(action.id)) {
						_controller->window().showToast(PluginUiText(
							u"Could not run the plugin action."_q,
							u"Не удалось выполнить действие плагина."_q));
					}
				});
				if (!action.description.trimmed().isEmpty()) {
					Ui::AddDividerText(
						_content,
						rpl::single(action.description.trimmed()));
				}
			}
			if (!settingsPages.empty()) {
				Ui::AddSkip(_content);
			}
		}

		if (!settingsPages.empty()) {
			Ui::AddSubsectionTitle(
				_content,
				rpl::single(PluginUiText(u"Settings"_q, u"Настройки"_q)));
			for (const auto &page : settingsPages) {
				Ui::AddSkip(_content);
				AddPluginSettingsContent(_content, page, stateChanged);
			}
		} else {
			Ui::AddSkip(_content);
			Ui::AddDividerText(
				_content,
				rpl::single(PluginUiText(
					u"This plugin does not expose separate settings yet."_q,
					u"У этого плагина пока нет отдельных настроек."_q)));
		}

		Ui::ResizeFitChild(this, _content);
	}

	const not_null<Window::SessionController*> _controller;
	const QString _pluginId;
	const Type _type;
	not_null<Ui::VerticalLayout*> _content;
	QString _title;
};

struct PluginDetailsFactory final
	: AbstractSectionFactory
	, std::enable_shared_from_this<PluginDetailsFactory> {
	explicit PluginDetailsFactory(QString pluginId)
	: pluginId(std::move(pluginId)) {
	}

	object_ptr<AbstractSection> create(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*> scroll,
		rpl::producer<Container> containerValue
	) const final override {
		Q_UNUSED(scroll);
		Q_UNUSED(containerValue);
		const auto type = std::static_pointer_cast<AbstractSectionFactory>(
			std::const_pointer_cast<PluginDetailsFactory>(shared_from_this()));
		return object_ptr<PluginDetailsSection>(
			parent,
			controller,
			pluginId,
			type);
	}

	QString pluginId;
};

[[nodiscard]] Type MakePluginDetailsType(const QString &pluginId) {
	return std::make_shared<PluginDetailsFactory>(pluginId);
}

} // namespace

Type PluginDetailsId(const QString &pluginId) {
	return MakePluginDetailsType(pluginId);
}

Plugins::Plugins(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent)
, _controller(controller)
, _content(Ui::CreateChild<Ui::VerticalLayout>(this))
, _list(_content->add(object_ptr<Ui::VerticalLayout>(_content))) {
	setupContent();
}

rpl::producer<QString> Plugins::title() {
	return rpl::single(PluginUiText(u"Plugins"_q, u"Плагины"_q));
}

void Plugins::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Documentation"_q, u"Документация"_q),
		.handler = [=] {
			QDesktopServices::openUrl(QUrl(u"https://docs.astrogram.su"_q));
		},
		.icon = &st::menuIconFaq,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(
			u"Runtime & Diagnostics"_q,
			u"Рантайм и диагностика"_q),
		.handler = [=] { ShowPluginRuntimeBox(_controller); },
		.icon = &st::menuIconIpAddress,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Open Plugins Folder"_q, u"Открыть папку плагинов"_q),
		.handler = [=] { File::ShowInFolder(Core::App().plugins().pluginsPath()); },
		.icon = &st::menuIconShowInFolder,
	});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Open plugins.log"_q, u"Открыть plugins.log"_q),
		.handler = [=] {
			RevealPluginAuxFile(
				_controller,
				u"./tdata/plugins.log"_q,
				PluginUiText(
					u"plugins.log was not found."_q,
					u"Файл plugins.log не найден."_q));
		},
		.icon = &st::menuIconSettings,
	});
	const auto safeModeEnabled = Core::App().plugins().safeModeEnabled();
	addAction(Ui::Menu::MenuCallback::Args{
		.text = safeModeEnabled
			? PluginUiText(
				u"Disable Safe Mode"_q,
				u"Выключить безопасный режим"_q)
			: PluginUiText(
				u"Enable Safe Mode"_q,
				u"Включить безопасный режим"_q),
		.handler = [=] {
			RequestSafeModeChange(
				_controller,
				this,
				!safeModeEnabled,
				crl::guard(this, [=] {
					_listRefreshPending = true;
					scheduleRebuildList(kPluginListRefreshDelayMs);
				}));
		},
		.icon = &st::menuIconSettings,
	});
}

void Plugins::setupContent() {
	Ui::AddDivider(_content);
	Ui::AddSkip(_content);
	rebuildList();
	Ui::ResizeFitChild(this, _content);
}

void Plugins::scheduleRebuildList(int delayMs) {
	if (_rebuildScheduled) {
		return;
	}
	_rebuildScheduled = true;
	QTimer::singleShot(delayMs, this, [=] {
		_rebuildScheduled = false;
		refreshPending();
		Ui::ResizeFitChild(this, _content);
		update();
	});
}

void Plugins::setListInteractive(bool enabled) {
	_list->setEnabled(enabled);
	_list->setAttribute(Qt::WA_TransparentForMouseEvents, !enabled);
}

void Plugins::refreshPending() {
	if (!_listRefreshPending) {
		return;
	}
	_listRefreshPending = false;
	rebuildList();
}

void Plugins::rebuildList() {
	_listRefreshPending = false;
	setListInteractive(false);
	_list->setUpdatesEnabled(false);
	DestroyLayoutChildrenSynchronously(_list);
	const auto scheduleRefresh = crl::guard(this, [=] {
		Logs::writeClient(u"[plugins-ui] scheduled list refresh"_q);
		setListInteractive(false);
		_listRefreshPending = true;
		scheduleRebuildList(kPluginListRefreshDelayMs);
	});
	const auto finishRebuild = [=] {
		_list->setUpdatesEnabled(true);
		setListInteractive(true);
		Ui::ResizeFitChild(this, _content);
	};
	if (Core::App().plugins().safeModeEnabled()) {
		Ui::AddDividerText(
			_list,
			rpl::single(
				PluginUiText(
					u"Safe mode is enabled. Plugins are shown without loading. Open the top bar menu to disable it."_q,
					u"Безопасный режим включён. Плагины показаны без загрузки. Откройте меню в верхней панели, чтобы выключить его."_q)));
		Ui::AddSkip(_list);
	}

	const auto plugins = Core::App().plugins().plugins();
	Logs::writeClient(QString::fromLatin1(
		"[plugins-ui] rebuild list: safeMode=%1 pluginCount=%2")
		.arg(Core::App().plugins().safeModeEnabled() ? u"true"_q : u"false"_q)
		.arg(plugins.size()));
	if (plugins.empty()) {
		Ui::AddDividerText(
			_list,
			rpl::single(PluginUiText(
				u"No plugins found in tdata/plugins. Use the top bar menu for the plugins folder and diagnostics."_q,
				u"В tdata/plugins плагины не найдены. Для папки плагинов и диагностики используйте меню в верхней панели."_q)));
		Ui::AddSkip(_list);
		finishRebuild();
		return;
	}
	auto first = true;
	for (const auto &state : plugins) {
		if (!first) {
			Ui::AddSkip(_list, kPluginCardVerticalMargin);
		}
		first = false;

		const auto title = FormatPluginTitle(state);
		const auto meta = PluginCardMetaText(state);
		const auto summary = FormatPluginCardSummary(state);
		const auto card = AddPluginCardContainer(_list, state);
		const auto headerWidget = card->add(
			object_ptr<Button>(
				card,
				rpl::single(title),
				st::settingsPluginCardHeader),
			style::margins(
				kPluginCardContentInsetLeft,
				0,
				kPluginCardContentInsetRight,
				0),
			style::al_top);
		const auto header = static_cast<Button*>(headerWidget);
		header->toggleOn(rpl::single(state.enabled));
		if (!state.error.isEmpty() && !state.disabledByRecovery) {
			header->setToggleLocked(true);
		}
		header->toggledChanges(
		) | rpl::filter([=](bool value) {
			return (value != state.enabled);
		}) | rpl::on_next([=](bool value) {
			if (!Core::App().plugins().setEnabled(state.info.id, value)) {
				_controller->window().showToast(PluginUiText(
					u"Could not change state."_q,
					u"Не удалось изменить состояние плагина."_q));
			}
			scheduleRefresh();
		}, header->lifetime());
		AddPluginSourceBadge(card, state, PluginSourceBadgeMode::Card);
		if (!meta.text.isEmpty()) {
			AddPluginMetaText(card, meta);
		}
		if (!summary.isEmpty()) {
			AddPluginDescriptionText(card, summary);
		}
		AddPluginCardActionRow(
			card,
			_controller,
			state,
			scheduleRefresh);
	}

	finishRebuild();
}

PluginsDocumentation::PluginsDocumentation(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent)
, _controller(controller) {
	setupContent();
}

rpl::producer<QString> PluginsDocumentation::title() {
	return rpl::single(
		PluginUiText(u"Plugin Documentation"_q, u"Документация плагинов"_q));
}

void PluginsDocumentation::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	Ui::AddDividerText(content, rpl::single(PluginDocsText()));
	Ui::AddSkip(content);
	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
