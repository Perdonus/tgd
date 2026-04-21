/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_plugins.h"
#include "settings/settings_common.h"
#include "settings/cloud_password/settings_cloud_password_common.h"

#include "api/api_common.h"
#include "boxes/abstract_box.h"
#include "boxes/share_box.h"
#include "core/application.h"
#include "core/astrogram_channel_registry.h"
#include "core/file_utilities.h"
#include "data/data_chat_participant_status.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history_item_helpers.h"
#include "logs.h"
#include "lang/lang_keys.h"
#include "lang/lang_text_entity.h"
#include "apiwrap.h"
#include "main/main_session.h"
#include "plugins/plugins_manager.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/scroll_area.h"
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

constexpr auto kPluginUiRebuildDebounceMs = 120;

[[nodiscard]] bool UseRussianPluginUi() {
	return Lang::LanguageIdOrDefault(Lang::Id()).startsWith(u"ru"_q);
}

[[nodiscard]] QString PluginUiText(QString en, QString ru) {
	return UseRussianPluginUi() ? std::move(ru) : std::move(en);
}

[[nodiscard]] bool ShouldHidePluginFromPrimaryUi(
		const ::Plugins::PluginState &state) {
	return state.info.id.trimmed() == u"astro.show_logs"_q;
}

[[nodiscard]] bool IsStablePluginCardState(
		const ::Plugins::PluginState &state) {
	return !state.info.id.trimmed().isEmpty()
		&& (!state.info.name.trimmed().isEmpty()
			|| !state.path.trimmed().isEmpty());
}

constexpr auto kPluginCardRadius = 20.;
constexpr auto kPluginCardVerticalMargin = 14;
constexpr auto kPluginCardOuterMargin = 16;
constexpr auto kPluginCardContentInsetLeft = 18;
constexpr auto kPluginCardContentInsetRight = 12;
constexpr auto kPluginCardDescriptionInsetLeft = 18;
constexpr auto kPluginCardMetaBottomMargin = 8;
constexpr auto kPluginCardDescriptionBottomMargin = 2;
constexpr auto kPluginCardActionRowTopMargin = 14;
constexpr auto kPluginCardActionRowBottomPadding = 10;
constexpr auto kPluginCardActionGap = 8;

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
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(
			kPluginCardContentInsetLeft,
			0,
			kPluginCardContentInsetRight,
			kPluginCardMetaBottomMargin),
		style::al_top);
	const auto raw = static_cast<Ui::RpWidget*>(row);
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		raw,
		rpl::single(text),
		st::defaultFlatLabel);
	label->setTryMakeSimilarLines(true);
	raw->widthValue() | rpl::on_next([=](int width) {
		label->resizeToWidth(std::max(width, 0));
		label->moveToLeft(0, 0, width);
	}, raw->lifetime());
	label->sizeValue() | rpl::on_next([=](const QSize &size) {
		raw->resize(raw->width(), size.height());
	}, raw->lifetime());
	label->resizeToWidth(std::max(raw->width(), 0));
	label->moveToLeft(0, 0, raw->width());
	raw->resize(raw->width(), label->height());
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
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(
			kPluginCardDescriptionInsetLeft,
			0,
			kPluginCardContentInsetRight,
			kPluginCardDescriptionBottomMargin),
		style::al_top);
	const auto raw = static_cast<Ui::RpWidget*>(row);
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		raw,
		rpl::single(TextWithEntities{ text.trimmed() }),
		st::defaultFlatLabel);
	label->setBreakEverywhere(true);
	label->setTryMakeSimilarLines(true);
	raw->widthValue() | rpl::on_next([=](int width) {
		label->resizeToWidth(std::max(width, 0));
		label->moveToLeft(0, 0, width);
	}, raw->lifetime());
	label->sizeValue() | rpl::on_next([=](const QSize &size) {
		raw->resize(raw->width(), size.height());
	}, raw->lifetime());
	label->resizeToWidth(std::max(raw->width(), 0));
	label->moveToLeft(0, 0, raw->width());
	raw->resize(raw->width(), label->height());
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
	if (state.sourceTrustLoading) {
		return PluginUiText(
			u"Updating source data..."_q,
			u"Обновление данных..."_q);
	}
	return state.sourceVerified
		? PluginUiText(
			u"Verified source"_q,
			u"Подтверждённый источник"_q)
		: PluginUiText(
			u"Unverified source"_q,
			u"Неподтверждённый источник"_q);
}

QString PluginSourceBadgeDetailText(const ::Plugins::PluginState &state) {
	const auto channelLabel = !state.sourceChannelTitle.trimmed().isEmpty()
		? state.sourceChannelTitle.trimmed()
		: !state.sourceChannelUsername.trimmed().isEmpty()
		? u'@' + state.sourceChannelUsername.trimmed()
		: QString::number(
			Core::AstrogramChannelRegistry::details::kRegistryChannelFullId);
	if (state.sourceTrustLoading
		|| state.sourceTrustReason == u"channel-feed-refreshing"_q) {
		return PluginUiText(
			u"Checking records in %1."_q,
			u"Проверяем записи в %1."_q).arg(channelLabel);
	}
	if (state.sourceTrustReason == u"channel-feed-refresh-failed"_q) {
		return PluginUiText(
			u"Could not refresh %1 yet. The client will retry automatically."_q,
			u"Пока не удалось обновить %1. Клиент попробует снова автоматически."_q
		).arg(channelLabel);
	}
	if (state.sourceVerified) {
		return state.sourceMessageId
			? PluginUiText(
				u"Matched in %1, post #%2."_q,
				u"Совпадение найдено в %1, пост #%2."_q)
					.arg(channelLabel)
					.arg(state.sourceMessageId)
			: PluginUiText(
				u"Matched in %1."_q,
				u"Совпадение найдено в %1."_q).arg(channelLabel);
	}
	if (state.sourceTrustReason == u"channel-feed-no-match"_q) {
		return PluginUiText(
			u"No matching record was found in %1."_q,
			u"В %1 не найдено подтверждающей записи."_q).arg(channelLabel);
	}
	if (state.sourceTrustReason == u"sha256-unavailable"_q) {
		return PluginUiText(
			u"Could not calculate the package hash yet."_q,
			u"Пока не удалось вычислить хэш пакета."_q);
	}
	return QString();
}

void DrawPluginSourceBadgeGlyph(
		Painter &p,
		QRectF rect,
		const QColor &fg,
		bool loading,
		bool verified) {
	p.setPen(QPen(fg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	if (loading) {
		const auto cy = rect.center().y();
		const auto startX = rect.left() + 2.;
		for (auto i = 0; i != 3; ++i) {
			auto color = fg;
			if (i == 2) {
				color.setAlpha(110);
			}
			p.setPen(Qt::NoPen);
			p.setBrush(color);
			p.drawEllipse(
				QRectF(
					startX + (i * 4.5),
					cy - 1.5,
					3.,
					3.));
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(fg, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		return;
	}
	if (verified) {
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(rect.adjusted(0.8, 0.8, -0.8, -0.8));
		p.drawLine(
			QPointF(rect.left() + 3.5, rect.center().y() + 0.6),
			QPointF(rect.left() + 6.1, rect.bottom() - 3.6));
		p.drawLine(
			QPointF(rect.left() + 6.1, rect.bottom() - 3.6),
			QPointF(rect.right() - 3.0, rect.top() + 3.2));
		return;
	}
	p.setBrush(Qt::NoBrush);
	const auto top = QPointF(rect.center().x(), rect.top() + 1.5);
	const auto left = QPointF(rect.left() + 2.4, rect.bottom() - 2.1);
	const auto right = QPointF(rect.right() - 2.4, rect.bottom() - 2.1);
	p.drawLine(top, left);
	p.drawLine(left, right);
	p.drawLine(right, top);
	p.drawLine(
		QPointF(rect.center().x(), rect.top() + 4.0),
		QPointF(rect.center().x(), rect.center().y() + 1.5));
	p.drawPoint(QPointF(rect.center().x(), rect.bottom() - 4.3));
}

void AddPluginSourceBadge(
		not_null<Ui::VerticalLayout*> container,
		const ::Plugins::PluginState &state,
		PluginSourceBadgeMode mode = PluginSourceBadgeMode::Card) {
	Q_UNUSED(mode);
	const auto badge = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(
			kPluginCardContentInsetLeft,
			(mode == PluginSourceBadgeMode::Card) ? 4 : 0,
			kPluginCardContentInsetRight,
			0),
		style::al_top);
	const auto text = PluginSourceBadgeText(state);
	const auto fill = state.sourceTrustLoading
		? QColor(0xf2, 0xc9, 0x4c, 44)
		: state.sourceVerified
		? QColor(0x27, 0xae, 0x60, 38)
		: QColor(0xeb, 0x57, 0x57, 34);
	const auto border = state.sourceTrustLoading
		? QColor(0xd8, 0xa1, 0x00)
		: state.sourceVerified
		? QColor(0x21, 0x96, 0x53)
		: QColor(0xeb, 0x57, 0x57);
	const auto fg = state.sourceTrustLoading
		? QColor(0x8a, 0x67, 0x00)
		: state.sourceVerified
		? QColor(0x1b, 0x8f, 0x50)
		: QColor(0xcf, 0x45, 0x45);
	const auto badgeHeight = st::semiboldFont->height + 12;
	const auto horizontalPadding = 12;
	const auto iconWidth = 18;
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
			st::semiboldFont->width(text)
				+ (horizontalPadding * 2)
				+ iconWidth);
		const auto pillLeft = std::max((badge->width() - pillWidth) / 2, 0);
		const auto rect = QRectF(pillLeft, 0, pillWidth, badge->height() - 1)
			.adjusted(0.5, 0.5, -0.5, -0.5);
		p.setPen(QPen(border, 1.));
		p.setBrush(fill);
		p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
		p.setPen(fg);
		DrawPluginSourceBadgeGlyph(
			p,
			QRectF(
				pillLeft + horizontalPadding,
				(badge->height() - 14.) / 2.,
				14.,
				14.),
			fg,
			state.sourceTrustLoading,
			state.sourceVerified);
		p.drawText(
			QRect(
				pillLeft + horizontalPadding + iconWidth,
				0,
				std::max(1, pillWidth - (horizontalPadding * 2) - iconWidth),
				badge->height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			text);
	}, badge->lifetime());
	if (mode == PluginSourceBadgeMode::Details) {
		if (const auto detail = PluginSourceBadgeDetailText(state);
			!detail.isEmpty()) {
			container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					rpl::single(TextWithEntities{ detail }),
					st::defaultFlatLabel),
				style::margins(
					kPluginCardContentInsetLeft,
					6,
					kPluginCardContentInsetRight,
					0),
				style::al_top);
		}
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
	const auto logsRoot = QDir(host.workingPath);

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

	lines.push_back(QString());
	lines.push_back(PluginUiText(
		u"Diagnostics files"_q,
		u"Файлы диагностики"_q));
	lines.push_back(PluginUiText(u"client.log: "_q, u"client.log: "_q)
		+ QDir::toNativeSeparators(logsRoot.filePath(u"client.log"_q)));
	lines.push_back(PluginUiText(u"plugins.log: "_q, u"plugins.log: "_q)
		+ QDir::toNativeSeparators(logsRoot.filePath(u"tdata/plugins.log"_q)));
	lines.push_back(PluginUiText(u"plugins.trace.jsonl: "_q, u"plugins.trace.jsonl: "_q)
		+ QDir::toNativeSeparators(logsRoot.filePath(u"tdata/plugins.trace.jsonl"_q)));
	lines.push_back(PluginUiText(u"plugins.recovery.json: "_q, u"plugins.recovery.json: "_q)
		+ QDir::toNativeSeparators(logsRoot.filePath(u"tdata/plugins.recovery.json"_q)));

	lines.push_back(QString());
	lines.push_back(PluginUiText(
		u"What to check first"_q,
		u"Что смотреть в первую очередь"_q));
	lines.push_back(PluginUiText(
		u"1. client.log for [plugins] and [plugins-ui] lines."_q,
		u"1. client.log на строки [plugins] и [plugins-ui]."_q));
	lines.push_back(PluginUiText(
		u"2. plugins.log for load-failed / abi-mismatch / action failed / panel failed."_q,
		u"2. plugins.log на load-failed / abi-mismatch / action failed / panel failed."_q));
	lines.push_back(PluginUiText(
		u"3. plugins.trace.jsonl for exact event order and runtime API requests."_q,
		u"3. plugins.trace.jsonl на точный порядок событий и runtime API запросы."_q));
	lines.push_back(PluginUiText(
		u"4. plugins.recovery.json if safe mode or auto-disable was triggered."_q,
		u"4. plugins.recovery.json, если сработал safe mode или авто-выключение."_q));
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
		const ::Plugins::PluginState &state) {
	const auto card = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(kPluginCardOuterMargin, 0, kPluginCardOuterMargin, 0),
		style::al_top);
	const auto raw = static_cast<Ui::RpWidget*>(card);
	const auto inner = Ui::CreateChild<Ui::VerticalLayout>(raw);
	const auto margins = QMargins(
		2,
		10,
		2,
		10);

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
	const auto initialInnerWidth = std::max(
		0,
		raw->width() - margins.left() - margins.right());
	inner->resizeToWidth(initialInnerWidth);
	inner->move(margins.left(), margins.top());
	raw->resize(
		raw->width(),
		inner->height() + margins.top() + margins.bottom());

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
		const ::Plugins::PluginState &state);

void RequestPluginRemoval(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> context,
		const ::Plugins::PluginState &state,
		Fn<void()> onRemoved);

void AddPluginCardActionRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		const ::Plugins::PluginState &state,
		Fn<void()> onChanged) {
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		style::margins(kPluginCardContentInsetLeft, kPluginCardActionRowTopMargin, kPluginCardContentInsetRight, kPluginCardActionRowBottomPadding),
		style::al_top);
	const auto raw = static_cast<Ui::RpWidget*>(row);
	const auto settings = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarSettings);
	Ui::IconButton *share = nullptr;
	const auto remove = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarDelete);

	settings->setClickedCallback([=] {
		controller->showSettings(PluginDetailsId(state.info.id));
	});
	if (const auto path = state.path.trimmed();
		!path.isEmpty() && QFileInfo(path).isFile()) {
		share = Ui::CreateChild<Ui::IconButton>(raw, st::infoTopBarForward);
		share->setClickedCallback([=] {
			SharePluginPackage(controller, state);
		});
	}
	remove->setClickedCallback([=] {
		RequestPluginRemoval(controller, raw, state, onChanged);
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
		const auto gap = kPluginCardActionGap;
		auto buttons = std::vector<Ui::IconButton*>{ settings };
		if (share) {
			buttons.push_back(share);
		}
		buttons.push_back(remove);
		auto left = 0;
		const auto top = std::max(0, raw->height() - buttonHeight - kPluginCardActionRowBottomPadding);
		for (const auto current : buttons) {
			current->move(left, top);
			left += current->width() + gap;
		}
	}, raw->lifetime());
	{
		const auto gap = kPluginCardActionGap;
		auto buttons = std::vector<Ui::IconButton*>{ settings };
		if (share) {
			buttons.push_back(share);
		}
		buttons.push_back(remove);
		auto left = 0;
		const auto top = std::max(
			0,
			raw->height() - buttonHeight - kPluginCardActionRowBottomPadding);
		for (const auto current : buttons) {
			current->move(left, top);
			left += current->width() + gap;
		}
	}
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

QString PluginsDiagnosticsSummaryText() {
	const auto host = Core::App().plugins().hostInfo();
	const auto states = Core::App().plugins().plugins();
	auto enabledCount = 0;
	auto loadedCount = 0;
	auto errorCount = 0;
	auto recoveryCount = 0;
	for (const auto &state : states) {
		enabledCount += state.enabled ? 1 : 0;
		loadedCount += state.loaded ? 1 : 0;
		errorCount += !state.error.trimmed().isEmpty() ? 1 : 0;
		recoveryCount += (state.recoverySuspected || state.disabledByRecovery) ? 1 : 0;
	}
	auto lines = QStringList();
	lines.push_back(PluginUiText(
		u"Always-visible plugin diagnostics. Runtime docs, safe mode and logs stay here even when plugins are broken."_q,
		u"Постоянно видимая диагностика плагинов. Рантайм, safe mode и логи остаются здесь даже если плагины сломаны."_q));
	lines.push_back(PluginUiText(
		u"Plugins: %1 total · %2 enabled · %3 loaded · %4 with errors · %5 in recovery"_q,
		u"Плагинов: %1 всего · %2 включено · %3 загружено · %4 с ошибками · %5 в recovery"_q)
		.arg(states.size())
		.arg(enabledCount)
		.arg(loadedCount)
		.arg(errorCount)
		.arg(recoveryCount));
	lines.push_back(PluginUiText(
		u"Safe mode: %1 · Runtime API: %2"_q,
		u"Безопасный режим: %1 · Runtime API: %2"_q)
		.arg(PluginUiText(host.safeModeEnabled ? u"enabled"_q : u"disabled"_q, host.safeModeEnabled ? u"включён"_q : u"выключен"_q))
		.arg(host.runtimeApiEnabled
			? PluginUiText(u"enabled on port %1"_q, u"включён на порту %1"_q).arg(host.runtimeApiPort)
			: PluginUiText(u"disabled"_q, u"выключен"_q)));
	return lines.join(u"\n"_q);
}

QString PluginDiagnosticsText(const ::Plugins::PluginState &state) {
	auto lines = QStringList();
	lines.push_back(PluginUiText(u"Plugin ID: "_q, u"ID плагина: "_q) + state.info.id);
	if (!state.path.trimmed().isEmpty()) {
		lines.push_back(PluginUiText(u"Package: "_q, u"Пакет: "_q) + QDir::toNativeSeparators(state.path));
	}
	lines.push_back(PluginUiText(
		u"State: %1 / %2"_q,
		u"Состояние: %1 / %2"_q)
		.arg(state.enabled
			? PluginUiText(u"enabled"_q, u"включён"_q)
			: PluginUiText(u"disabled"_q, u"выключен"_q))
		.arg(state.loaded
			? PluginUiText(u"loaded"_q, u"загружен"_q)
			: PluginUiText(u"not loaded"_q, u"не загружен"_q)));
	if (!state.error.trimmed().isEmpty()) {
		lines.push_back(PluginUiText(u"Last error: "_q, u"Последняя ошибка: "_q) + state.error.trimmed());
	}
	if (state.recoverySuspected || state.disabledByRecovery) {
		lines.push_back(PluginUiText(
			u"Recovery: this plugin was auto-disabled after a risky callback."_q,
			u"Recovery: этот плагин был автоматически выключен после рискованного callback."_q));
		if (!state.recoveryReason.trimmed().isEmpty()) {
			lines.push_back(PluginUiText(u"Recovery reason: "_q, u"Причина recovery: "_q) + state.recoveryReason.trimmed());
		}
	}
	return lines.join(u"\n"_q);
}

void AddSettingsActionButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Fn<void()> callback) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(text),
		st::settingsButtonNoIcon));
	button->setClickedCallback([callback = std::move(callback)] {
		if (callback) {
			callback();
		}
			});
		}

int PluginPackageCount(const QString &pluginId) {
	const auto normalized = pluginId.trimmed();
	if (normalized.isEmpty()) {
		return 0;
	}
	auto count = 0;
	for (const auto &state : Core::App().plugins().plugins()) {
		if (state.info.id.trimmed() == normalized
			&& !state.path.trimmed().isEmpty()) {
			++count;
		}
	}
	return count;
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
		const ::Plugins::PluginState &state) {
	const auto path = state.path.trimmed();
	if (path.isEmpty()) {
		controller->window().showToast(PluginUiText(
			u"Plugin file path is unavailable."_q,
			u"Путь к файлу плагина недоступен."_q));
		return;
	}
	if (!QFileInfo(path).isFile()) {
		controller->window().showToast(PluginUiText(
			u"The plugin package file could not be found."_q,
			u"Файл пакета плагина не найден."_q));
		return;
	}
	const auto box = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
	const auto sending = std::make_shared<bool>(false);
	auto countMessagesCallback = [=](const TextWithTags &) {
		return 1;
	};
	auto submitCallback = [=](
			std::vector<not_null<Data::Thread*>> &&result,
			Fn<bool()> checkPaid,
			TextWithTags &&comment,
			Api::SendOptions options,
			Data::ForwardOptions) {
		if (*sending || result.empty()) {
			return;
		}
		const auto error = GetErrorForSending(
			result,
			{
				.text = comment.text.isEmpty() ? nullptr : &comment,
				.messagesCount = 1,
			});
		if (error.error) {
			if (*box) {
				(*box)->uiShow()->showBox(MakeSendErrorBox(
					error,
					result.size() > 1));
			}
			return;
		} else if (!checkPaid()) {
			return;
		}

		*sending = true;
		const auto premium = controller->session().user()->isPremium();
		for (const auto thread : result) {
			auto list = Storage::PrepareMediaList(
				QStringList{ path },
				st::sendMediaPreviewSize,
				premium);
			if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
				*sending = false;
				controller->window().showToast(PluginUiText(
					u"Could not prepare the plugin package for sending."_q,
					u"Не удалось подготовить пакет плагина к отправке."_q));
				return;
			}
			auto action = Api::SendAction(thread, options);
			action.clearDraft = false;
			auto caption = comment;
			thread->session().api().sendFiles(
				std::move(list),
				SendMediaType::File,
				std::move(caption),
				nullptr,
				action);
		}
		if (*box) {
			(*box)->closeBox();
		}
		controller->window().showToast(PluginUiText(
			u"Plugin package sent."_q,
			u"Пакет плагина отправлен."_q));
	};
	auto filterCallback = [](not_null<Data::Thread*> thread) {
		if (const auto user = thread->peer()->asUser()) {
			if (user->canSendIgnoreMoneyRestrictions()) {
				return true;
			}
		}
		return Data::CanSend(thread, ChatRestriction::SendFiles);
	};
	auto object = Box<ShareBox>(ShareBox::Descriptor{
		.session = &controller->session(),
		.countMessagesCallback = std::move(countMessagesCallback),
		.submitCallback = std::move(submitCallback),
		.filterCallback = std::move(filterCallback),
		.titleOverride = rpl::single(PluginUiText(
			u"Share plugin package"_q,
			u"Поделиться плагином"_q)),
		.moneyRestrictionError = ShareMessageMoneyRestrictionError(),
	});
	*box = base::make_weak(object.data());
	controller->show(std::move(object));
}

void RequestPluginRemoval(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> context,
		const ::Plugins::PluginState &state,
		Fn<void()> onRemoved) {
	controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		const auto packageCount = std::max(1, PluginPackageCount(state.info.id));
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(PluginUiText(u"Delete plugin"_q, u"Удалить плагин"_q)));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single((packageCount > 1)
				? PluginUiText(
					u"Delete plugin \"%1\" and all %2 package files with the same plugin ID?"_q,
					u"Удалить плагин \"%1\" и все %2 файла пакетов с тем же ID?"_q)
						.arg(FormatPluginTitle(state))
						.arg(packageCount)
				: PluginUiText(
					u"Delete plugin \"%1\"?"_q,
					u"Удалить плагин \"%1\"?"_q).arg(FormatPluginTitle(state))),
			st::boxLabel),
			style::margins(st::boxPadding.left(), 0, st::boxPadding.right(), 0),
			style::al_top);
		if (packageCount > 1) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(PluginUiText(
					u"Astrogram will unload the plugin first and then remove every local package file for this plugin ID."_q,
					u"Astrogram сначала выгрузит плагин, а затем удалит все локальные файлы пакетов для этого ID."_q)),
				st::boxLabel),
				style::margins(
					st::boxPadding.left(),
					st::defaultVerticalListSkip / 2,
					st::boxPadding.right(),
					0),
				style::al_top);
		}
		box->addButton(rpl::single(PluginUiText(u"Delete"_q, u"Удалить"_q)), [=] {
			box->closeBox();
			QTimer::singleShot(0, context.get(), [=] {
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
				controller->window().showToast((packageCount > 1)
					? PluginUiText(
						u"Plugin removed together with %1 package files."_q,
						u"Плагин удалён вместе с %1 файлами пакетов."_q).arg(packageCount)
					: PluginUiText(
						u"Plugin removed."_q,
						u"Плагин удалён."_q));
				if (onRemoved) {
					onRemoved();
				}
			});
		});
		box->addButton(tr::lng_cancel(), [=] {
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
			QTimer::singleShot(0, context.get(), [=] {
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

void AddPluginsDiagnosticsSection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> context,
		Fn<void()> onStateChanged) {
	Ui::AddSubsectionTitle(
		container,
		rpl::single(PluginUiText(
			u"Runtime & Diagnostics"_q,
			u"Рантайм и диагностика"_q)));
	Ui::AddDividerText(container, rpl::single(PluginsDiagnosticsSummaryText()));
	AddSettingsActionButton(container, PluginUiText(
		u"Open runtime overview"_q,
		u"Открыть обзор рантайма"_q), [=] {
		ShowPluginRuntimeBox(controller);
	});
	AddSettingsActionButton(container, PluginUiText(
		u"Open local plugin docs"_q,
		u"Открыть локальную документацию плагинов"_q), [=] {
		ShowPluginDocsBox(controller);
	});
	AddSettingsActionButton(container, PluginUiText(
		u"Open plugins folder"_q,
		u"Открыть папку плагинов"_q), [=] {
		File::ShowInFolder(Core::App().plugins().pluginsPath());
	});
	AddSettingsActionButton(container, PluginUiText(
		u"Reload plugins now"_q,
		u"Перезагрузить плагины сейчас"_q), [=] {
		Core::App().plugins().reload();
	});
	AddSettingsActionButton(container, PluginUiText(
			u"Open client.log"_q,
			u"Открыть client.log"_q), [=] {
		RevealPluginAuxFile(
			controller,
			u"./client.log"_q,
			PluginUiText(u"client.log was not found."_q, u"Файл client.log не найден."_q));
	});
	AddSettingsActionButton(container, PluginUiText(
		u"Open plugins.log"_q,
		u"Открыть plugins.log"_q), [=] {
			RevealPluginAuxFile(
			controller,
			u"./tdata/plugins.log"_q,
			PluginUiText(u"plugins.log was not found."_q, u"Файл plugins.log не найден."_q));
	});
	AddSettingsActionButton(container, PluginUiText(
		u"Open plugins.trace.jsonl"_q,
		u"Открыть plugins.trace.jsonl"_q), [=] {
			RevealPluginAuxFile(
			controller,
			u"./tdata/plugins.trace.jsonl"_q,
			PluginUiText(u"plugins.trace.jsonl was not found."_q, u"Файл plugins.trace.jsonl не найден."_q));
	});
	AddSettingsActionButton(container, PluginUiText(
		u"Open recovery state"_q,
		u"Открыть recovery-state"_q), [=] {
			RevealPluginAuxFile(
			controller,
			u"./tdata/plugins.recovery.json"_q,
			PluginUiText(u"plugins.recovery.json was not found."_q, u"Файл plugins.recovery.json не найден."_q));
	});
	const auto safeModeEnabled = Core::App().plugins().safeModeEnabled();
	AddSettingsActionButton(container, safeModeEnabled
		? PluginUiText(u"Disable Safe Mode"_q, u"Выключить безопасный режим"_q)
		: PluginUiText(u"Enable Safe Mode"_q, u"Включить безопасный режим"_q), [=] {
		RequestSafeModeChange(controller, context, !safeModeEnabled, onStateChanged);
	});
	Ui::AddSkip(container);
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
				const auto currentValue = std::make_shared<bool>(setting.boolValue);
				button->toggledChanges() | rpl::on_next([=](bool value) {
					if (value == *currentValue) {
						return;
					}
					auto updated = setting;
					updated.boolValue = value;
					if (Core::App().plugins().updateSetting(page.id, updated)) {
						*currentValue = value;
					} else if (onStateChanged) {
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
				const auto currentValue = std::make_shared<int>(setting.intValue);
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
					if (updated.intValue == *currentValue) {
						valueLabel->setText(formatValue(updated.intValue));
						return;
					}
					valueLabel->setText(formatValue(updated.intValue));
					if (Core::App().plugins().updateSetting(page.id, updated)) {
						*currentValue = updated.intValue;
					} else if (onStateChanged) {
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

[[nodiscard]] QString PluginSettingsPageUiKey(
		const ::Plugins::SettingsPageState &page) {
	auto key = QString();
	key.reserve(256);
	key += page.title.trimmed();
	key += u'\n';
	key += page.description.trimmed();
	for (const auto &section : page.sections) {
		key += u"\n#"_q + section.id.trimmed();
		key += u'\n';
		key += section.title.trimmed();
		key += u'\n';
		key += section.description.trimmed();
		for (const auto &setting : section.settings) {
			key += u"\n-"_q + setting.id.trimmed();
			key += u'|';
			key += setting.title.trimmed();
			key += u'|';
			key += QString::number(int(setting.type));
		}
	}
	return key;
}

[[nodiscard]] std::vector<::Plugins::SettingsPageState> DeduplicatePluginSettingsPages(
		const std::vector<::Plugins::SettingsPageState> &pages) {
	auto result = std::vector<::Plugins::SettingsPageState>();
	auto seenKeys = QStringList();
	result.reserve(pages.size());
	seenKeys.reserve(int(pages.size()));
	for (auto i = pages.crbegin(); i != pages.crend(); ++i) {
		const auto key = PluginSettingsPageUiKey(*i);
		if (seenKeys.contains(key)) {
			continue;
		}
		seenKeys.push_back(key);
		result.push_back(*i);
	}
	std::reverse(result.begin(), result.end());
	return result;
}

class PluginDetailsSection final : public AbstractSection {
public:
	PluginDetailsSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::ScrollArea*> scroll,
		QString pluginId,
		Type type)
	: AbstractSection(parent)
	, _controller(controller)
	, _scroll(scroll)
	, _pluginId(std::move(pluginId))
	, _type(std::move(type))
	, _content(Ui::CreateChild<Ui::VerticalLayout>(this)) {
			Core::App().plugins().stateChanges() | rpl::on_next([=](
					const ::Plugins::ManagerStateChange &change) {
				const auto normalizedChangePluginId = change.pluginId.trimmed();
				if (!change.structural && normalizedChangePluginId != _pluginId) {
					return;
				} else if (!change.structural
					&& (normalizedChangePluginId == _pluginId)
					&& (change.reason == u"settings"_q)) {
					return;
				}
			Logs::writeClient(QString::fromLatin1(
				"[plugins-ui] details refresh requested: plugin=%1 seq=%2 reason=%3 sourcePlugin=%4 structural=%5 failed=%6")
				.arg(_pluginId)
				.arg(change.sequence)
				.arg(change.reason)
				.arg(normalizedChangePluginId.isEmpty() ? u"-"_q : normalizedChangePluginId)
				.arg(change.structural ? u"true"_q : u"false"_q)
				.arg(change.failed ? u"true"_q : u"false"_q));
			if (_rebuildScheduled) {
				return;
			}
			_rebuildScheduled = true;
			QTimer::singleShot(kPluginUiRebuildDebounceMs, this, [=] {
				_rebuildScheduled = false;
				rebuild();
			});
		}, _stateChangesLifetime);
		rebuild();
	}

	[[nodiscard]] Type id() const override {
		return _type;
	}

	[[nodiscard]] rpl::producer<QString> title() override {
		return rpl::single(_title);
	}

private:
	void restoreScrollTop(int top) {
		QTimer::singleShot(0, this, [=] {
			const auto target = std::clamp(top, 0, _scroll->scrollTopMax());
			if (_scroll->scrollTop() != target) {
				_scroll->scrollToY(target);
			}
		});
	}

	void rebuild() {
		const auto preservedScrollTop = _scroll->scrollTop();
		const auto state = LookupPluginState(_pluginId);
		if (!state) {
			if (_lastKnownState
				&& Core::App().plugins().uiTransientPluginsActive()) {
				if (_missingStateRetryScheduled) {
					return;
				}
				_missingStateRetryScheduled = true;
				QTimer::singleShot(150, this, [=] {
					_missingStateRetryScheduled = false;
					rebuild();
				});
				return;
			}
			_lastKnownState.reset();
			_content->clear();
			_title = PluginUiText(u"Plugin"_q, u"Плагин"_q);
			Ui::AddDividerText(
				_content,
				rpl::single(PluginUiText(
					u"Plugin was not found."_q,
					u"Плагин не найден."_q)));
			Ui::ResizeFitChild(this, _content);
			restoreScrollTop(preservedScrollTop);
			return;
		}

		_lastKnownState = *state;
		_content->clear();
		_title = FormatPluginTitle(*state);
		AddPluginSourceBadge(_content, *state, PluginSourceBadgeMode::Details);
		const auto stateChanged = crl::guard(this, [=] { rebuild(); });

		const auto actions = Core::App().plugins().actionsFor(state->info.id);
		const auto panels = Core::App().plugins().panelsFor(state->info.id);
		const auto settingsPages = DeduplicatePluginSettingsPages(
			Core::App().plugins().settingsPagesFor(state->info.id));

		if (!actions.empty()) {
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
			}
		}

		if (!panels.empty()) {
			for (const auto &panel : panels) {
				Ui::AddSkip(_content);
				const auto button = _content->add(object_ptr<Ui::SettingsButton>(
					_content,
					rpl::single(panel.title.trimmed().isEmpty()
						? PluginUiText(u"Open panel"_q, u"Открыть панель"_q)
						: panel.title.trimmed()),
					st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					if (!Core::App().plugins().openPanel(panel.id)) {
						_controller->window().showToast(PluginUiText(
							u"Could not open the plugin panel."_q,
							u"Не удалось открыть панель плагина."_q));
					}
				});
			}
		}

		if (!settingsPages.empty()) {
			for (const auto &page : settingsPages) {
				Ui::AddSkip(_content);
				AddPluginSettingsContent(_content, page, stateChanged);
			}
		}

		Ui::ResizeFitChild(this, _content);
		restoreScrollTop(preservedScrollTop);
	}

	const not_null<Window::SessionController*> _controller;
	const not_null<Ui::ScrollArea*> _scroll;
	const QString _pluginId;
	const Type _type;
	not_null<Ui::VerticalLayout*> _content;
	QString _title;
	rpl::lifetime _stateChangesLifetime;
	bool _rebuildScheduled = false;
	bool _missingStateRetryScheduled = false;
	std::optional<::Plugins::PluginState> _lastKnownState;
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
		Q_UNUSED(containerValue);
		const auto type = std::static_pointer_cast<AbstractSectionFactory>(
			std::const_pointer_cast<PluginDetailsFactory>(shared_from_this()));
		return object_ptr<PluginDetailsSection>(
			parent,
			controller,
			scroll,
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
	Core::App().plugins().stateChanges() | rpl::on_next([=](
			const ::Plugins::ManagerStateChange &change) {
		if (!change.structural && (change.reason == u"settings"_q)) {
			return;
		}
		Logs::writeClient(QString::fromLatin1(
			"[plugins-ui] manager change observed: seq=%1 reason=%2 plugin=%3 structural=%4 failed=%5")
			.arg(change.sequence)
			.arg(change.reason)
			.arg(change.pluginId.trimmed().isEmpty() ? u"-"_q : change.pluginId.trimmed())
			.arg(change.structural ? u"true"_q : u"false"_q)
			.arg(change.failed ? u"true"_q : u"false"_q));
		_listRefreshPending = true;
		scheduleRebuildList(kPluginUiRebuildDebounceMs);
	}, _stateChangesLifetime);
	setupContent();
}

rpl::producer<QString> Plugins::title() {
	return rpl::single(PluginUiText(u"Plugins"_q, u"Плагины"_q));
}

void Plugins::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Documentation"_q, u"Документация"_q),
		.handler = [=] { ShowPluginDocsBox(_controller); },
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
			.text = PluginUiText(u"Reload Plugins"_q, u"Перезагрузить плагины"_q),
			.handler = [=] {
				Core::App().plugins().reload();
			},
			.icon = &st::menuIconSettings,
		});
	addAction(Ui::Menu::MenuCallback::Args{
		.text = PluginUiText(u"Open Plugins Folder"_q, u"Открыть папку плагинов"_q),
		.handler = [=] { File::ShowInFolder(Core::App().plugins().pluginsPath()); },
		.icon = &st::menuIconShowInFolder,
	});
}

void Plugins::setupContent() {
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

void Plugins::refreshPending() {
	if (!_listRefreshPending) {
		return;
	}
	_listRefreshPending = false;
	rebuildList();
}

void Plugins::rebuildList() {
	_listRefreshPending = false;
	const auto allPlugins = Core::App().plugins().plugins();
	auto plugins = std::vector<::Plugins::PluginState>();
	plugins.reserve(allPlugins.size());
	for (const auto &state : allPlugins) {
		if (!ShouldHidePluginFromPrimaryUi(state)) {
			plugins.push_back(state);
		}
	}
	const auto transientSnapshotActive = Core::App().plugins().uiTransientPluginsActive();
	const auto stableNow = !plugins.empty()
		&& std::all_of(
			plugins.begin(),
			plugins.end(),
			&IsStablePluginCardState);
	const auto samePluginList = [&] {
		if (plugins.size() != _lastStablePlugins.size()) {
			return false;
		}
		for (auto i = 0, count = int(plugins.size()); i != count; ++i) {
			if (plugins[i].info.id.trimmed() != _lastStablePlugins[i].info.id.trimmed()) {
				return false;
			}
		}
		return true;
	};
	const auto transientStructureChanged = transientSnapshotActive
		&& !_lastStablePlugins.empty()
		&& !samePluginList();
	if (stableNow && !transientStructureChanged) {
		_lastStablePlugins = plugins;
	}
	const auto useStableSnapshot = !_lastStablePlugins.empty()
		&& (transientStructureChanged
			|| (!plugins.empty() && !stableNow)
			|| (transientSnapshotActive
				&& plugins.empty()
				&& (_lastRenderedPluginCount > 0)));
	if (useStableSnapshot && (_lastRenderedPluginCount > 0)) {
		Logs::writeClient(
			u"[plugins-ui] keep previous rendered cards during transient snapshot"_q);
		_listRefreshPending = true;
		scheduleRebuildList(150);
		return;
	}
	const auto &renderPlugins = useStableSnapshot
		? _lastStablePlugins
		: plugins;
	Logs::writeClient(QString::fromLatin1(
		"[plugins-ui] rebuild list: safeMode=%1 pluginCount=%2 visibleCount=%3 stable=%4 snapshot=%5 transient=%6")
		.arg(Core::App().plugins().safeModeEnabled() ? u"true"_q : u"false"_q)
		.arg(allPlugins.size())
		.arg(plugins.size())
		.arg(stableNow ? u"true"_q : u"false"_q)
		.arg(_lastStablePlugins.empty() ? u"false"_q : u"true"_q)
		.arg(transientSnapshotActive ? u"true"_q : u"false"_q));
	if (renderPlugins.empty()
		&& transientSnapshotActive
		&& (_lastRenderedPluginCount > 0)) {
		Logs::writeClient(
			u"[plugins-ui] transient empty plugin list observed, keeping previous cards"_q);
		_listRefreshPending = true;
		scheduleRebuildList(150);
		return;
	}
	_list->clear();
	if (Core::App().plugins().safeModeEnabled()) {
		Ui::AddDividerText(
			_list,
			rpl::single(
				PluginUiText(
					u"Safe mode is enabled. Plugins are shown without loading. Open the top bar menu to disable it."_q,
					u"Безопасный режим включён. Плагины показаны без загрузки. Откройте меню в верхней панели, чтобы выключить его."_q)));
		Ui::AddSkip(_list);
	}
	if (renderPlugins.empty()) {
		_lastRenderedPluginCount = 0;
		Ui::AddDividerText(
			_list,
			rpl::single(PluginUiText(
				u"No plugins found in tdata/plugins."_q,
				u"В tdata/plugins плагины не найдены."_q)));
		Ui::AddSkip(_list);
		Ui::ResizeFitChild(this, _content);
		return;
	}
	auto first = true;
	for (const auto &state : renderPlugins) {
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
			}, header->lifetime());
		if (!meta.text.isEmpty()) {
			AddPluginMetaText(card, meta);
		}
		if (!summary.isEmpty()) {
			AddPluginDescriptionText(card, summary);
		}
		AddPluginSourceBadge(card, state);
			AddPluginCardActionRow(
				card,
				_controller,
				state,
				nullptr);
	}
	_lastRenderedPluginCount = int(renderPlugins.size());

	Ui::ResizeFitChild(this, _content);
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
