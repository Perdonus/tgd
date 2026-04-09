/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/boxes/astrogram_onboarding_box.h"

#include "ui/boxes/about_cocoon_box.h"

#include "apiwrap.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_experimental.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/ministar_particles.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/top_background_gradient.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <memory>
#include <optional>

namespace Ui {
namespace {

enum class Step {
	Welcome = 0,
	Presets = 1,
	PluginsInfo = 2,
	PluginsInstall = 3,
	ShellMode = 4,
	Finish = 5,
};

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QImage AstrogramLogo() {
	auto result = QImage(u":/gui/art/astrogram/settings_avatar.png"_q);
	if (result.isNull()) {
		result = QImage(u":/gui/art/logo_256_no_margin.png"_q);
	}
	return result;
}

struct PeerCardTexts {
	QString title;
	QString status;
};

struct PluginCardTexts {
	QString title;
	QString description;
	QString sourceLabel;
};

[[nodiscard]] bool IsAstrogramPluginPackage(DocumentData *document) {
	if (!document) {
		return false;
	}
	const auto filename = document->filename().trimmed();
	return !filename.isEmpty()
		&& filename.endsWith(u".tgd"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString AstrogramPluginTitle(
		DocumentData *document,
		QString fallbackTitle) {
	fallbackTitle = fallbackTitle.trimmed();
	if (!fallbackTitle.isEmpty()) {
		return fallbackTitle;
	}
	auto filename = document ? document->filename().trimmed() : QString();
	if (filename.isEmpty()) {
		return RuEn("Плагин Astrogram", "Astrogram plugin");
	}
	if (filename.endsWith(u".tgd"_q, Qt::CaseInsensitive)) {
		filename.chop(4);
	}
	filename = QFileInfo(filename).completeBaseName().trimmed();
	return filename.isEmpty()
		? RuEn("Плагин Astrogram", "Astrogram plugin")
		: filename;
}

[[nodiscard]] QString AstrogramPluginDescription(
		HistoryItem *item,
		QString fallbackDescription,
		qint64 postId) {
	fallbackDescription = fallbackDescription.trimmed();
	if (!fallbackDescription.isEmpty()) {
		return fallbackDescription;
	}
	if (item) {
		const auto message = item->originalText().text.simplified();
		if (!message.isEmpty()) {
			return message;
		}
		const auto notification = item->notificationText().text.simplified();
		if (!notification.isEmpty()) {
			return notification;
		}
	}
	return RuEn(
		"Пакет плагина из поста #%1",
		"Plugin package from post #%1").arg(postId);
}

[[nodiscard]] QString AstrogramPluginSourceLabel(
		QString channelTitle,
		qint64 postId) {
	channelTitle = channelTitle.trimmed();
	if (channelTitle.isEmpty()) {
		channelTitle = RuEn(
			"Astrogram Plugins",
			"Astrogram Plugins");
	}
	return RuEn(
		"Источник: %1 · пост #%2",
		"Source: %1 · post #%2").arg(channelTitle).arg(postId);
}

[[nodiscard]] QString PeerAudienceText(not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	if (!channel || !channel->membersCountKnown()) {
		return QString();
	}
	const auto count = channel->membersCount();
	if (count <= 0) {
		return QString();
	}
	return (!channel->isMegagroup()
		? tr::lng_chat_status_subscribers
		: tr::lng_chat_status_members)(
			tr::now,
			lt_count,
			count);
}

[[nodiscard]] PeerCardTexts ComputePeerCardTexts(
		PeerData *peer,
		QString fallbackTitle,
		QString fallbackStatus) {
	auto title = fallbackTitle.trimmed();
	auto status = fallbackStatus.trimmed();
	if (peer) {
		const auto peerName = peer->name().trimmed();
		if (!peerName.isEmpty()) {
			title = peerName;
		}
		const auto audience = PeerAudienceText(peer);
		if (!audience.isEmpty()) {
			status = status.isEmpty()
				? audience
				: (audience + u" · "_q + status);
		}
	}
	return {
		.title = title,
		.status = status,
	};
}

[[nodiscard]] PluginCardTexts ResolvePluginCardTexts(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		const AstrogramOnboardingPlugin &plugin,
		QString fallbackChannelTitle) {
	const auto channel = peer ? peer->asChannel() : nullptr;
	const auto item = (channel && (plugin.postId > 0))
		? controller->session().data().message(channel, MsgId(plugin.postId))
		: nullptr;
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	const auto effectiveChannelTitle = ComputePeerCardTexts(
		peer,
		std::move(fallbackChannelTitle),
		QString()).title;
	return {
		.title = AstrogramPluginTitle(
			IsAstrogramPluginPackage(document) ? document : nullptr,
			plugin.title),
		.description = AstrogramPluginDescription(
			item,
			plugin.description,
			plugin.postId),
		.sourceLabel = (plugin.postId > 0)
			? AstrogramPluginSourceLabel(
				effectiveChannelTitle,
				plugin.postId)
			: plugin.sourceLabel.trimmed(),
	};
}

void AddHeroCover(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &subtitle,
		QColor gradientEdge,
		QColor gradientCenter,
		QColor gradientLeft,
		QColor gradientRight) {
	const auto cover = container->add(object_ptr<Ui::RpWidget>(container));

	const auto logoTop = st::cocoonLogoTop;
	const auto logoSize = st::cocoonLogoSize;
	const auto titleTop = logoTop + logoSize + st::cocoonTitleTop;
	const auto subtitleTop = titleTop
		+ st::cocoonTitleFont->height
		+ st::cocoonSubtitleTop;

	struct State {
		QImage gradient;
		QImage logo;
		Ui::Animations::Basic animation;
		std::optional<Ui::StarParticles> particles;
		style::owned_color subtitleFg = style::owned_color{ QColor(0xe5, 0xf1, 0xee) };
		style::owned_color subtitleBoldFg = style::owned_color{ QColor(0xff, 0xff, 0xff) };
		style::FlatLabel subtitleSt = st::cocoonSubtitle;
	};
	const auto state = cover->lifetime().make_state<State>();
	state->subtitleSt.textFg = state->subtitleFg.color();
	state->subtitleSt.palette.linkFg = state->subtitleBoldFg.color();
	state->animation.init([=] {
		cover->update();
		if (anim::Disabled()) {
			state->animation.stop();
		}
	});

	const auto ratio = style::DevicePixelRatio();
	state->logo = AstrogramLogo().scaled(
		QSize(logoSize, logoSize) * ratio,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	state->logo.setDevicePixelRatio(ratio);

	state->particles.emplace(
		Ui::StarParticles::Type::RadialInside,
		54,
		st::cocoonLogoSize / 12);
	state->particles->setSpeed(0.05);
	state->particles->setColors({
		gradientLeft,
		anim::color(gradientLeft, gradientRight, 0.45),
		anim::color(gradientLeft, gradientRight, 0.75),
		gradientRight,
	});

	const auto subtitleLabel = Ui::CreateChild<Ui::FlatLabel>(
		cover,
		tr::rich(subtitle),
		state->subtitleSt);
	subtitleLabel->setTryMakeSimilarLines(true);

	cover->widthValue() | rpl::on_next([=](int width) {
		const auto available = width
			- st::boxRowPadding.left()
			- st::boxRowPadding.right();
		subtitleLabel->resizeToWidth(available);
		subtitleLabel->moveToLeft(st::boxRowPadding.left(), subtitleTop);
		cover->resize(
			width,
			subtitleLabel->y() + subtitleLabel->height() + st::cocoonSubtitleBottom);
	}, cover->lifetime());

	cover->paintRequest() | rpl::on_next([=] {
		auto p = Painter(cover);
		const auto width = cover->width();
		const auto ratio = style::DevicePixelRatio();
		if (state->gradient.size() != cover->size() * ratio) {
			state->gradient = Ui::CreateTopBgGradient(
				cover->size(),
				gradientCenter,
				gradientEdge);

			auto font = st::cocoonTitleFont->f;
			font.setWeight(QFont::Bold);
			const auto metrics = QFontMetrics(font);
			const auto textw = metrics.horizontalAdvance(title);
			const auto left = (width - textw) / 2;

			auto q = QPainter(&state->gradient);
			auto hq = PainterHighQualityEnabler(q);

			auto gradient = QLinearGradient(left, 0, left + textw, 0);
			gradient.setStops({
				{ 0., gradientLeft },
				{ 1., gradientRight },
			});
			q.setPen(QPen(QBrush(gradient), 1.));
			q.setFont(font);
			q.setBrush(Qt::NoBrush);
			q.drawText(left, titleTop + metrics.ascent(), title);
		}
		p.drawImage(0, 0, state->gradient);

		const auto logoRect = QRect(
			(width - logoSize) / 2,
			logoTop,
			logoSize,
			logoSize);
		const auto paddingAdd = int(base::SafeRound(logoTop * 1.2));
		const auto particlesRect = logoRect.marginsAdded(
			{ paddingAdd, paddingAdd, paddingAdd, paddingAdd });
		state->particles->paint(p, particlesRect, crl::now(), false);
		if (!anim::Disabled() && !state->animation.animating()) {
			state->animation.start();
		}
		p.drawImage(logoRect, state->logo);
	}, cover->lifetime());
}

not_null<Ui::RoundButton*> AddPrimaryButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		std::function<void()> callback) {
	auto button = object_ptr<Ui::RoundButton>(
		container,
		rpl::single(text),
		st::defaultActiveButton);
	button->setTextTransform(Ui::RoundButton::TextTransform::NoTransform);
	const auto raw = container->add(
		std::move(button),
		style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
	raw->resizeToWidth(container->width()
		- st::boxRowPadding.left()
		- st::boxRowPadding.right());
	raw->setClickedCallback(std::move(callback));
	return raw;
}

not_null<Ui::LinkButton*> AddLinkAction(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		std::function<void()> callback) {
	const auto button = container->add(
		object_ptr<Ui::LinkButton>(
			container,
			rpl::single(text),
			st::defaultLinkButton),
		style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
	button->setClickedCallback(std::move(callback));
	return button;
}

not_null<Ui::SettingsButton*> AddChoiceButton(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		const QString &description,
		const style::icon *icon,
		std::function<void()> callback) {
	auto descriptor = Settings::IconDescriptor();
	descriptor.icon = icon;
	const auto button = container->add(
		Settings::CreateButtonWithIcon(
			container,
			rpl::single(title),
			st::settingsButtonLight,
			std::move(descriptor)),
		style::margins(12, 0, 12, 0));
	button->setClickedCallback(std::move(callback));
	if (!description.isEmpty()) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(description),
				st::defaultFlatLabel),
			style::margins(26, -2, 26, 6),
			style::al_top);
	}
	return button;
}

not_null<Ui::AbstractButton*> AddPeerChoiceButton(
		not_null<Ui::VerticalLayout*> container,
		PeerData *peer,
		const QString &fallbackTitle,
		const QString &fallbackDescription,
		std::function<void()> callback) {
	const auto row = container->add(
		object_ptr<Ui::AbstractButton>::fromRaw(
			Ui::CreateSimpleSettingsButton(
				container,
				st::defaultRippleAnimation,
				st::defaultSettingsButton.textBgOver)),
		style::margins(12, 0, 12, 0));
	row->resize(row->width(), st::defaultPeerListItem.height);
	row->setClickedCallback(std::move(callback));

	const auto userpicSt = row->lifetime().make_state<style::UserpicButton>(
		st::defaultUserpicButton);
	userpicSt->photoSize = st::defaultPeerListItem.photoSize;
	userpicSt->size = QSize(userpicSt->photoSize, userpicSt->photoSize);

	struct State {
		std::shared_ptr<Ui::Text::String> title;
		std::shared_ptr<Ui::Text::String> status;
	};
	const auto state = row->lifetime().make_state<State>();
	state->title = std::make_shared<Ui::Text::String>();
	state->status = std::make_shared<Ui::Text::String>();
	const auto refresh = [=] {
		const auto texts = ComputePeerCardTexts(
			peer,
			fallbackTitle,
			fallbackDescription);
		state->title->setText(
			st::defaultPeerListItem.nameStyle,
			texts.title);
		state->status->setText(
			st::defaultTextStyle,
			texts.status);
		row->update();
	};

	if (peer) {
		using Button = Ui::UserpicButton;
		const auto userpic = Ui::CreateChild<Button>(row, peer, *userpicSt);
		userpic->move(st::defaultPeerListItem.photoPosition);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		peer->loadUserpic();
		peer->session().changes().peerUpdates(
			peer,
			Data::PeerUpdate::Flag::Name
				| Data::PeerUpdate::Flag::Username
				| Data::PeerUpdate::Flag::Photo
				| Data::PeerUpdate::Flag::Members
				| Data::PeerUpdate::Flag::FullInfo
		) | rpl::on_next([=](const Data::PeerUpdate &) {
			refresh();
		}, row->lifetime());
		if (const auto channel = peer->asChannel();
			channel && !channel->membersCountKnown()) {
			peer->session().api().requestFullPeer(channel);
		}
	}
	refresh();

	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		const auto &st = st::defaultPeerListItem;
		const auto availableWidth = row->width()
			- st::boxRowPadding.right()
			- st.namePosition.x();
		auto context = Ui::Text::PaintContext{
			.position = st.namePosition,
			.outerWidth = availableWidth,
			.availableWidth = availableWidth,
			.elisionLines = 1,
		};
		p.setPen(st.nameFg);
		state->title->draw(p, context);
		p.setPen(st.statusFg);
		context.position = st.statusPosition;
		state->status->draw(p, context);
	}, row->lifetime());

	return row;
}

void ClearLayout(not_null<Ui::VerticalLayout*> container) {
	while (container->count()) {
		delete container->widgetAt(0);
	}
}

} // namespace

void ShowAstrogramOnboardingBox(AstrogramOnboardingArgs args) {
	Expects(args.controller != nullptr);

	args.controller->show(Box([args = std::move(args)](
			not_null<Ui::GenericBox*> box) mutable {
		box->setWidth(st::boxWideWidth * 5 / 4);
		box->setStyle(st::stakeBox);
		box->setNoContentMargin(true);

		const auto container = box->verticalLayout();
		AddUniqueCloseButton(box);
		const auto plugins = box->lifetime().make_state<
				std::vector<AstrogramOnboardingPlugin>>(std::move(args.plugins));

		struct State {
			Step step = Step::Welcome;
			PeerData *pluginsChannelPeer = nullptr;
			PeerData *officialChannelPeer = nullptr;
			QSet<qint64> requestedPluginPosts;
			bool reloadingPlugins = false;
			bool autoReloadTriggered = false;
			bool completionStored = false;
		};
		const auto state = box->lifetime().make_state<State>(State{
			.pluginsChannelPeer = args.pluginsChannelPeer,
			.officialChannelPeer = args.officialChannelPeer,
		});
		const auto weak = base::make_weak(box);
		const auto controller = args.controller;

		const auto markCompleted = [=] {
			if (state->completionStored) {
				return;
			}
			state->completionStored = true;
			if (args.finished) {
				args.finished();
			}
		};
		const auto finish = [=] {
			markCompleted();
			box->closeBox();
		};
		const auto openExperimental = [=] {
			markCompleted();
			box->closeBox();
			const auto weakController = base::make_weak(controller);
			QTimer::singleShot(0, [=] {
				if (const auto controller = weakController.get()) {
					controller->showSettings(Settings::Experimental::Id());
				}
			});
		};
		box->boxClosing(
		) | rpl::on_next([=] {
			if ((state->step == Step::Finish) && !state->completionStored) {
				markCompleted();
			}
		}, box->lifetime());

		const auto rebuild = std::make_shared<Fn<void()>>();
		const auto requestPluginsReload = std::make_shared<Fn<void()>>();
		*requestPluginsReload = [=] {
			if (state->reloadingPlugins || !args.reloadPlugins) {
				return;
			}
			state->reloadingPlugins = true;
			args.reloadPlugins([weak, rebuild, state, plugins](
					std::vector<AstrogramOnboardingPlugin> refreshed) {
				if (!weak.get()) {
					return;
				}
				*plugins = std::move(refreshed);
				state->reloadingPlugins = false;
				state->requestedPluginPosts.clear();
				(*rebuild)();
			});
		};

		*rebuild = [=]() mutable {
			ClearLayout(container);

			const auto addTitle = [&](const QString &title, const QString &subtitle) {
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(title),
						st::sessionBigName),
					style::margins(st::boxRowPadding.left(), 12, st::boxRowPadding.right(), 0),
					style::al_top);
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(subtitle),
						st::boxLabel),
					style::margins(st::boxRowPadding.left(), 6, st::boxRowPadding.right(), 0),
					style::al_top);
			};
			const auto pluginStatusText = [](const AstrogramOnboardingPlugin &plugin) {
				if (plugin.invalidServerData) {
					return RuEn(
						"серверная карточка требует обновления",
						"server card needs refresh");
				} else if (plugin.pendingServerData) {
					return RuEn(
						"карточка ещё загружается с сервера",
						"card is still loading from the server");
				}
				return RuEn(
					"рекомендация сервера",
					"server recommendation");
			};
			const auto applyShellPreset = [&](AstrogramOnboardingShellPreset preset) {
				if (args.applyShellPreset && !args.applyShellPreset(preset)) {
					controller->showToast(RuEn(
						"Не удалось сохранить пресет меню Astrogram.",
						"Failed to save the Astrogram menu preset."));
					return;
				}
				state->step = Step::Finish;
				(*rebuild)();
			};

			switch (state->step) {
			case Step::Welcome: {
				AddHeroCover(
					container,
					RuEn("Добро пожаловать в", "Welcome to"),
					RuEn(
						"Astrogram Desktop\nКлиент уже готов к работе, дальше поможем быстро настроить самое важное.",
						"Astrogram Desktop\nThe client is ready to use, next we will quickly tune the important parts."),
					QColor(0x08, 0x15, 0x11),
					QColor(0x10, 0x2b, 0x20),
					QColor(0x2d, 0xd1, 0x85),
					QColor(0xa1, 0xff, 0xcc));
				Ui::AddSkip(container, st::settingsCheckboxesSkip);
				AddPrimaryButton(container, RuEn("Продолжить", "Continue"), [=] {
					state->step = Step::Presets;
					(*rebuild)();
				});
			} break;
			case Step::Presets: {
				addTitle(
					RuEn("Выбери стартовый пресет", "Choose a startup preset"),
					RuEn(
						"Это можно поменять позже в настройках Astrogram. Если не хочешь выбирать сейчас, просто пропусти шаг.",
						"You can change this later in Astrogram settings. If you don't want to decide now, just skip this step."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Рекомендованный", "Recommended"),
					RuEn(
						"Включает полезные Astrogram-функции без агрессивных изменений интерфейса.",
						"Enables useful Astrogram features without aggressive interface changes."),
					&st::menuIconPremium,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Recommended);
						}
						state->step = Step::PluginsInfo;
						(*rebuild)();
					});
				AddChoiceButton(
					container,
					RuEn("Приватный", "Private"),
					RuEn(
						"Делает упор на ghost mode, защиту от удаления и более осторожное поведение клиента.",
						"Focuses on ghost mode, anti-recall and a more private client setup."),
					&st::menuIconLock,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Private);
						}
						state->step = Step::PluginsInfo;
						(*rebuild)();
					});
				AddChoiceButton(
					container,
					RuEn("Минимальный", "Minimal"),
					RuEn(
						"Оставляет интерфейс ближе к Telegram Desktop и включает только базовые улучшения Astrogram.",
						"Keeps the interface closer to Telegram Desktop and enables only core Astrogram improvements."),
					&st::menuIconPalette,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Minimal);
						}
						state->step = Step::PluginsInfo;
						(*rebuild)();
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddLinkAction(container, RuEn("Выбрать позже", "Choose later"), [=] {
					state->step = Step::PluginsInfo;
					(*rebuild)();
				});
				Ui::AddSkip(container, st::settingsCheckboxesSkip);
			} break;
			case Step::PluginsInfo: {
				addTitle(
					RuEn("Что такое плагины?", "What are plugins?"),
					RuEn(
						"Плагины расширяют Astrogram: добавляют новые функции, настройки и полезные сценарии прямо внутри клиента.",
						"Plugins extend Astrogram with extra features, settings and useful workflows directly inside the client."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Быстрая установка рекомендуемых плагинов", "Install recommended plugins"),
					RuEn(
						"Мы покажем несколько безопасных пакетов и дадим установить их в пару кликов.",
						"We'll show a few safe packages and let you install them in a couple of clicks."),
					&st::menuIconCustomize,
					[=] {
						state->step = Step::PluginsInstall;
						(*rebuild)();
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Продолжить", "Continue"), [=] {
					state->step = Step::PluginsInstall;
					(*rebuild)();
				});
				AddLinkAction(container, RuEn("Позже", "Later"), [=] {
					state->step = Step::ShellMode;
					(*rebuild)();
				});
			} break;
			case Step::PluginsInstall: {
				if (!state->autoReloadTriggered && args.reloadPlugins) {
					state->autoReloadTriggered = true;
					(*requestPluginsReload)();
				}
				addTitle(
					RuEn("Рекомендуемые плагины", "Recommended plugins"),
					RuEn(
						"Список ниже тянется из серверного app config. Можно обновить его прямо сейчас без перезапуска клиента.",
						"The list below is pulled from server app config. You can refresh it right now without restarting the client."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
				const auto fallbackPluginsTitle = args.pluginsChannelTitle.isEmpty()
					? RuEn("AstroPlugins", "AstroPlugins")
					: args.pluginsChannelTitle;
				AddPeerChoiceButton(
					container,
					state->pluginsChannelPeer,
					fallbackPluginsTitle,
					args.pluginsChannelSubtitle,
					[=] {
						if (args.subscribePluginsChannel) {
							args.subscribePluginsChannel();
						}
					});
				AddPrimaryButton(container, RuEn("Подписаться", "Follow"), [=] {
					if (args.subscribePluginsChannel) {
						args.subscribePluginsChannel();
					}
				});
				if (args.reloadPlugins) {
					AddLinkAction(container, RuEn("Обновить рекомендации с сервера", "Refresh recommendations from server"), [=] {
						(*requestPluginsReload)();
					});
				}
				if (state->reloadingPlugins) {
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(RuEn(
								"Проверяем свежие рекомендации и подтягиваем карточки постов…",
								"Checking fresh recommendations and fetching post cards…")),
							st::boxLabel),
						style::margins(st::boxRowPadding.left(), 8, st::boxRowPadding.right(), 0),
						style::al_top);
				}
				if (plugins->empty()) {
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(state->reloadingPlugins
								? RuEn(
									"Ждём ответ сервера с рекомендованными пакетами…",
									"Waiting for the server to provide recommended packages…")
								: RuEn(
									"Сервер ещё не передал рекомендованные пакеты. Когда появятся `astrogram_onboarding_plugin_post_ids`, они подтянутся сюда автоматически.",
									"The server has not provided recommended packages yet. Once `astrogram_onboarding_plugin_post_ids` appears, they will show up here automatically.")),
							st::boxLabel),
						style::margins(st::boxRowPadding.left(), 8, st::boxRowPadding.right(), 0),
						style::al_top);
				} else {
					const auto channel = state->pluginsChannelPeer
						? state->pluginsChannelPeer->asChannel()
						: nullptr;
					for (const auto &plugin : *plugins) {
						if (channel
							&& (plugin.postId > 0)
							&& !state->requestedPluginPosts.contains(plugin.postId)
							&& !controller->session().data().message(
								channel,
								MsgId(plugin.postId))) {
							state->requestedPluginPosts.insert(plugin.postId);
							controller->session().api().requestMessageData(
								channel,
								MsgId(plugin.postId),
								[weak, rebuild] {
									if (weak.get()) {
										(*rebuild)();
									}
								});
						}
						const auto texts = ResolvePluginCardTexts(
							controller,
							state->pluginsChannelPeer,
							plugin,
							fallbackPluginsTitle);
						const auto sourceText = [&] {
							const auto status = pluginStatusText(plugin);
							return texts.sourceLabel.isEmpty()
								? status
								: (texts.sourceLabel + u" · "_q + status);
						}();
						const auto action = plugin.invalidServerData
							? std::function<void()>([requestPluginsReload] {
								(*requestPluginsReload)();
							})
							: plugin.install;
						const auto actionText = plugin.invalidServerData
							? RuEn("Обновить список", "Refresh list")
							: RuEn("Установить", "Install");
						AddChoiceButton(
							container,
							texts.title,
							texts.description,
							&st::menuIconDownload,
							[action] {
								if (action) {
									action();
								}
							});
						if (!sourceText.isEmpty()) {
							container->add(
								object_ptr<Ui::FlatLabel>(
									container,
									rpl::single(sourceText),
									st::defaultFlatLabel),
								style::margins(26, -2, 26, 4),
								style::al_top);
						}
						AddPrimaryButton(
							container,
							actionText,
							[action] {
								if (action) {
									action();
								}
							});
						Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
					}
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Продолжить", "Continue"), [=] {
					state->step = Step::ShellMode;
					(*rebuild)();
				});
				AddLinkAction(container, RuEn("Посмотреть все", "View all"), [=] {
					if (args.openAllPlugins) {
						args.openAllPlugins();
					}
				});
			} break;
			case Step::ShellMode: {
				addTitle(
					RuEn("Меню и оболочка Astrogram", "Astrogram menu and shell"),
					RuEn(
						"Выбери стартовую геометрию бокового меню и настроек прямо сейчас. Полный editor останется в Experimental.",
						"Choose the starting side-menu and settings geometry right now. The full editor remains in Experimental."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Сбалансированный", "Balanced"),
					RuEn(
						"Иммерсивная анимация остаётся включённой, а интерфейс сохраняет компактную ширину.",
						"Keeps immersive animation on while leaving the interface compact."),
					&st::menuIconPalette,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Balanced); });
				AddChoiceButton(
					container,
					RuEn("Фокус на меню", "Focused"),
					RuEn(
						"Расширяет боковую панель и делает меню выразительнее без левоторцевых настроек.",
						"Expands the side panel and makes the shell stronger without left-edge settings."),
					&st::menuIconCustomize,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Focused); });
				AddChoiceButton(
					container,
					RuEn("Широкий интерфейс", "Wide shell"),
					RuEn(
						"Включает широкие настройки, левый край для settings и максимально выразительную оболочку.",
						"Enables wide settings, left-edge settings placement and the most expressive shell."),
					&st::menuIconExperimental,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Wide); });
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Открыть полный editor", "Open full editor"), openExperimental);
				AddLinkAction(container, RuEn("Выбрать позже", "Choose later"), [=] {
					state->step = Step::Finish;
					(*rebuild)();
				});
			} break;
			case Step::Finish: {
				AddHeroCover(
					container,
					RuEn("Всё готово", "You're all set"),
					RuEn(
						"Настройка завершена. Подпишись на официальный канал Astrogram и при желании поддержи разработку.",
						"Setup is complete. Follow the official Astrogram channel and support development if you want."),
					QColor(0x08, 0x13, 0x1f),
					QColor(0x0f, 0x2b, 0x3f),
					QColor(0x44, 0xc0, 0xff),
					QColor(0x9c, 0xff, 0xd3));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPeerChoiceButton(
					container,
					state->officialChannelPeer,
					args.officialChannelTitle.trimmed().isEmpty()
						? RuEn("Astrogram", "Astrogram")
						: args.officialChannelTitle.trimmed(),
					args.officialChannelSubtitle.trimmed().isEmpty()
						? RuEn(
							"Получай новости о сборках, клиентах и новых функциях Astrogram.",
							"Get updates about builds, client changes and new Astrogram features.")
						: args.officialChannelSubtitle,
					[=] {
						if (args.openOfficialChannel) {
							args.openOfficialChannel();
						}
					});
				AddChoiceButton(
					container,
					RuEn(
						"Открыть полный editor меню",
						"Open the full menu editor"),
					RuEn(
						"Experimental откроет editor бокового меню, иммерсивную анимацию, расширенную панель и широкие настройки.",
						"Experimental opens the side-menu editor, immersive animation, expanded panel and wide settings."),
					&st::menuIconExperimental,
					openExperimental);
				AddChoiceButton(
					container,
					RuEn(
						"Поддержать разработку Astrogram",
						"Support Astrogram development"),
					RuEn(
						"Открывает донат-окно Astrogram с серверным значком подписчика.",
						"Opens the Astrogram support box with the server-side subscriber badge."),
					&st::menuIconGiftPremium,
					[=] {
						if (args.openDonate) {
							args.openDonate();
						}
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddPrimaryButton(container, RuEn("Завершить", "Finish"), finish);
			} break;
			}
		};

		if (!state->pluginsChannelPeer && args.resolvePluginsChannel) {
			args.resolvePluginsChannel([=](PeerData *peer) {
				if (!peer) {
					return;
				}
				state->pluginsChannelPeer = peer;
				if (weak.get() && (state->step == Step::PluginsInstall)) {
					(*rebuild)();
				}
			});
		}
		if (!state->officialChannelPeer && args.resolveOfficialChannel) {
			args.resolveOfficialChannel([=](PeerData *peer) {
				if (!peer) {
					return;
				}
				state->officialChannelPeer = peer;
				if (weak.get() && (state->step == Step::Finish)) {
					(*rebuild)();
				}
			});
		}
		(*rebuild)();
	}));
}

} // namespace Ui
