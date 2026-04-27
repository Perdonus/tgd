/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/boxes/astrogram_onboarding_box.h"

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
#include "ui/controls/userpic_button.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSet>

#include <algorithm>
#include <memory>

namespace Ui {
namespace {

enum class Step {
	Welcome = 0,
	Presets = 1,
	PluginsInfo = 2,
	PluginsInstall = 3,
	MenuCustomization = 4,
	ShellMode = 5,
	ExperimentalTips = 6,
	Finish = 7,
};

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] int OnboardingSidePadding() {
	return 18;
}

[[nodiscard]] int OnboardingTextInset() {
	return 30;
}

[[nodiscard]] int AdaptiveOnboardingWidth(
		not_null<Window::SessionController*> controller) {
	const auto body = controller->widget()->bodyWidget();
	const auto available = body ? body->width() : 0;
	if (available <= 0) {
		return st::boxWideWidth;
	}
	return std::clamp(
		available - (OnboardingSidePadding() * 4),
		st::boxWideWidth,
		st::boxWideWidth + 96);
}

struct PeerCardTexts {
	QString title;
	QString status;
};

struct PluginCardTexts {
	QString title;
	QString description;
};

void AppendInlinePart(QString &base, QString part) {
	part = part.trimmed();
	if (part.isEmpty()) {
		return;
	}
	if (!base.isEmpty()) {
		base += u" \u00b7 "_q;
	}
	base += part;
}

[[nodiscard]] QString AppendLine(QString base, QString line) {
	base = base.trimmed();
	line = line.trimmed();
	if (line.isEmpty()) {
		return base;
	}
	return base.isEmpty() ? line : (base + u"\n"_q + line);
}

[[nodiscard]] QString AstrogramKnownChannelUsername(qint64 channelId) {
	switch (channelId) {
	case -1003814280064LL: return u"astroplugin"_q;
	case -1003641835839LL: return u"astrogramchannel"_q;
	}
	return QString();
}

[[nodiscard]] QString ChannelHandleText(PeerData *peer, qint64 channelId) {
	auto username = peer ? peer->username().trimmed() : QString();
	if (username.isEmpty()) {
		username = AstrogramKnownChannelUsername(channelId);
	}
	return username.isEmpty() ? QString() : (u"@"_q + username);
}

[[nodiscard]] QString ChannelMetaText(PeerData *peer, qint64 channelId) {
	auto result = QString();
	AppendInlinePart(result, ChannelHandleText(peer, channelId));
	if (channelId) {
		AppendInlinePart(
			result,
			RuEn("ID %1", "ID %1").arg(QString::number(channelId)));
	}
	return result;
}


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
	Q_UNUSED(fallbackChannelTitle);
	return {
		.title = AstrogramPluginTitle(
			IsAstrogramPluginPackage(document) ? document : nullptr,
			plugin.title),
		.description = AstrogramPluginDescription(
			item,
			plugin.description,
			plugin.postId),
	};
}

not_null<Ui::LinkButton*> AddLinkAction(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		std::function<void()> callback) {
	const auto button = container->add(
		object_ptr<Ui::LinkButton>(
			container,
			text,
			st::defaultLinkButton),
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
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
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
	button->setClickedCallback(std::move(callback));
	if (!description.isEmpty()) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(description),
				st::defaultFlatLabel),
			style::margins(
				OnboardingTextInset(),
				-2,
				OnboardingTextInset(),
				6),
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
		style::margins(
			OnboardingSidePadding(),
			0,
			OnboardingSidePadding(),
			0));
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
		const auto controller = args.controller;
		const auto updateBoxWidth = [=] {
			box->setWidth(AdaptiveOnboardingWidth(controller));
		};
		updateBoxWidth();
		box->setNoContentMargin(true);
		box->setCloseByEscape(false);
		box->setCloseByOutsideClick(false);
		if (const auto body = controller->widget()->bodyWidget()) {
			body->widthValue() | rpl::on_next([=](int) {
				updateBoxWidth();
			}, box->lifetime());
		}

		const auto container = box->verticalLayout();
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
		const auto skip = [=] {
			markCompleted();
			box->closeBox();
		};
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
			box->clearButtons();

			const auto addTitle = [&](const QString &title, const QString &subtitle) {
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(title),
						st::boxTitle),
					style::margins(
						OnboardingSidePadding(),
						14,
						OnboardingSidePadding(),
						0),
					style::al_top);
				if (subtitle.trimmed().isEmpty()) {
					return;
				}
				container->add(
					object_ptr<Ui::FlatLabel>(
						container,
						rpl::single(subtitle),
						st::boxLabel),
					style::margins(
						OnboardingSidePadding(),
						4,
						OnboardingSidePadding(),
						0),
					style::al_top);
			};
			const auto goToStep = [&](Step step) {
				state->step = step;
				(*rebuild)();
			};
			const auto setFooterButtons = [&](QString primaryText,
					Fn<void()> primaryCallback,
					QString secondaryText = QString(),
					Fn<void()> secondaryCallback = {}) {
				box->clearButtons();
				if (!secondaryText.isEmpty() && secondaryCallback) {
					const auto secondary = box->addLeftButton(
						rpl::single(secondaryText),
						std::move(secondaryCallback));
					secondary->setTextTransform(
						Ui::RoundButton::TextTransform::NoTransform);
				}
				if (!primaryText.isEmpty() && primaryCallback) {
					const auto primary = box->addButton(
						rpl::single(primaryText),
						std::move(primaryCallback));
					primary->setTextTransform(
						Ui::RoundButton::TextTransform::NoTransform);
				}
			};
			const auto pluginStatusText = [](const AstrogramOnboardingPlugin &plugin) {
				if (plugin.invalidServerData) {
					return RuEn(
						"нужно обновить",
						"needs refresh");
				} else if (plugin.pendingServerData) {
					return RuEn(
						"данные ещё загружаются",
						"data is still loading");
				}
				return RuEn(
					"рекомендация AstroPlugins",
					"AstroPlugins recommendation");
			};
			const auto applyShellPreset = [&](AstrogramOnboardingShellPreset preset) {
				if (args.applyShellPreset && !args.applyShellPreset(preset)) {
					controller->showToast(RuEn(
						"Не удалось сохранить пресет меню Astrogram.",
						"Failed to save the Astrogram menu preset."));
					return;
				}
				state->step = Step::ExperimentalTips;
				(*rebuild)();
			};

				switch (state->step) {
				case Step::Welcome: {
					addTitle(
						RuEn(
							"Добро пожаловать в Astrogram Desktop",
							"Welcome to Astrogram Desktop"),
						RuEn(
							"Быстро подготовим клиент к старту.",
							"We'll quickly prepare the client for launch."));
					Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
					setFooterButtons(
						RuEn("Продолжить", "Continue"),
						[=] { goToStep(Step::Presets); },
					RuEn("Пропустить", "Skip"),
					skip);
			} break;
				case Step::Presets: {
					addTitle(
						RuEn("Выбери стартовый пресет", "Choose a startup preset"),
						RuEn(
							"Потом всё можно поменять вручную.",
							"You can change everything later."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Рекомендованный", "Recommended"),
					RuEn(
						"Комфортные дефолты Astrogram.",
						"Comfortable Astrogram defaults."),
					&st::menuIconPremium,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Recommended);
						}
						goToStep(Step::PluginsInfo);
					});
				AddChoiceButton(
					container,
					RuEn("Приватный", "Private"),
					RuEn(
						"Упор на приватность.",
						"Privacy-focused setup."),
					&st::menuIconLock,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Private);
						}
						goToStep(Step::PluginsInfo);
					});
				AddChoiceButton(
					container,
					RuEn("Минимальный", "Minimal"),
					RuEn(
						"Ближе всего к Telegram Desktop.",
						"Closest to Telegram Desktop."),
					&st::menuIconPalette,
					[=] {
						if (args.applyPreset) {
							args.applyPreset(AstrogramOnboardingPreset::Minimal);
						}
						goToStep(Step::PluginsInfo);
					});
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Позже", "Later"),
					[=] { goToStep(Step::PluginsInfo); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::Welcome); });
			} break;
				case Step::PluginsInfo: {
					addTitle(
						RuEn("Плагины внутри Astrogram", "Plugins inside Astrogram"),
						RuEn(
							"Покажем доверенные .tgd-пакеты, которые можно поставить сразу.",
							"We'll show trusted .tgd packages you can install right away."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::PluginsInstall); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::Presets); });
			} break;
				case Step::PluginsInstall: {
				if (!state->autoReloadTriggered && args.reloadPlugins) {
					state->autoReloadTriggered = true;
					(*requestPluginsReload)();
				}
					addTitle(
						RuEn("Рекомендации AstroPlugins", "AstroPlugins recommendations"),
						RuEn(
							"Выбери, что поставить сразу. Остальное можно открыть позже в Plugins.",
							"Choose what to install right away. Everything else stays available later in Plugins."));
				const auto fallbackPluginsTitle = args.pluginsChannelTitle.isEmpty()
					? RuEn("AstroPlugins", "AstroPlugins")
					: args.pluginsChannelTitle;
				if (state->pluginsChannelPeer) {
					AddPeerChoiceButton(
						container,
						state->pluginsChannelPeer,
						fallbackPluginsTitle,
						ChannelMetaText(state->pluginsChannelPeer, args.pluginsChannelId),
						[=] {
							if (args.subscribePluginsChannel) {
								args.subscribePluginsChannel();
							}
						});
				} else if (args.subscribePluginsChannel) {
					AddChoiceButton(
						container,
						fallbackPluginsTitle,
						ChannelMetaText(nullptr, args.pluginsChannelId),
						&st::menuIconIpAddress,
						[=] {
							args.subscribePluginsChannel();
						});
				}
				if (args.reloadPlugins) {
					AddLinkAction(container, RuEn("Обновить рекомендации", "Refresh recommendations"), [=] {
						(*requestPluginsReload)();
					});
				}
				if (state->reloadingPlugins) {
					container->add(
						object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(RuEn(
								"Обновляем рекомендации и карточки…",
								"Refreshing recommendations and cards…")),
							st::boxLabel),
						style::margins(
							OnboardingSidePadding(),
							8,
							OnboardingSidePadding(),
							0),
						style::al_top);
				}
					if (plugins->empty()) {
						container->add(
							object_ptr<Ui::FlatLabel>(
							container,
							rpl::single(state->reloadingPlugins
								? RuEn(
									"Подтягиваем карточки…",
									"Loading package cards…")
								: RuEn(
									"Карточки появятся здесь автоматически, как только будут готовы.",
									"The cards will appear here automatically once they are ready.")),
							st::boxLabel),
						style::margins(
							OnboardingSidePadding(),
							8,
							OnboardingSidePadding(),
							0),
							style::al_top);
					} else {
						const auto visiblePlugins = std::min<int>(plugins->size(), 3);
						const auto channel = state->pluginsChannelPeer
							? state->pluginsChannelPeer->asChannel()
							: nullptr;
					for (auto i = 0; i != visiblePlugins; ++i) {
						const auto &plugin = (*plugins)[i];
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
							const auto action = plugin.invalidServerData
								? std::function<void()>([requestPluginsReload] {
									(*requestPluginsReload)();
								})
								: plugin.install;
							auto description = texts.description.trimmed();
							if (plugin.invalidServerData || plugin.pendingServerData) {
								description = AppendLine(description, pluginStatusText(plugin));
							}
							AddChoiceButton(
								container,
								texts.title,
							description,
							&st::menuIconDownload,
							[action] {
								if (action) {
									action();
								}
							});
						Ui::AddSkip(container, st::settingsCheckboxesSkip / 3);
					}
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::MenuCustomization); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::PluginsInfo); });
			} break;
				case Step::MenuCustomization: {
					addTitle(
						RuEn("Меню Astrogram", "Astrogram menus"),
						RuEn(
							"Боковую панель и контекстное меню можно докрутить позже вручную.",
							"You can fine-tune the side panel and context menu later."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::ShellMode); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::PluginsInstall); });
			} break;
				case Step::ShellMode: {
					addTitle(
						RuEn("Shell-режим", "Shell mode"),
						RuEn(
							"Выбери стартовый вид окна.",
							"Choose the starting shell layout."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				AddChoiceButton(
					container,
					RuEn("Сбалансированный", "Balanced"),
					RuEn(
						"Спокойный базовый вариант.",
						"A calm default setup."),
					&st::menuIconPalette,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Balanced); });
				AddChoiceButton(
					container,
					RuEn("Фокус на меню", "Focused"),
					RuEn(
						"Шире и выразительнее, без левого края.",
						"Wider and bolder, without the left edge."),
					&st::menuIconCustomize,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Focused); });
				AddChoiceButton(
					container,
					RuEn("Широкий интерфейс", "Wide shell"),
					RuEn(
						"Самый широкий и заметный вариант.",
						"The widest and most expressive option."),
					&st::menuIconExperimental,
					[=] { applyShellPreset(AstrogramOnboardingShellPreset::Wide); });
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::ExperimentalTips); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::MenuCustomization); });
			} break;
				case Step::ExperimentalTips: {
					addTitle(
						RuEn("Дальше всё в настройках", "Next everything stays in settings"),
						RuEn(
							"После гайда останутся только обычные настройки клиента.",
							"After onboarding only the regular client settings remain."));
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Продолжить", "Continue"),
					[=] { goToStep(Step::Finish); },
					RuEn("Назад", "Back"),
					[=] { goToStep(Step::ShellMode); });
			} break;
				case Step::Finish: {
					addTitle(
						RuEn("Готово", "Ready"),
						RuEn(
							"Стартовая настройка завершена.",
							"The initial setup is complete."));
				if (state->officialChannelPeer) {
					AddPeerChoiceButton(
						container,
						state->officialChannelPeer,
						args.officialChannelTitle.trimmed().isEmpty()
							? RuEn("Astrogram", "Astrogram")
							: args.officialChannelTitle.trimmed(),
						ChannelMetaText(state->officialChannelPeer, args.officialChannelId),
						[=] {
							if (args.openOfficialChannel) {
								args.openOfficialChannel();
							}
						});
				} else if (args.openOfficialChannel) {
					AddChoiceButton(
						container,
						args.officialChannelTitle.trimmed().isEmpty()
							? RuEn("Astrogram", "Astrogram")
							: args.officialChannelTitle.trimmed(),
						ChannelMetaText(nullptr, args.officialChannelId),
						&st::menuIconInfo,
						[=] {
							args.openOfficialChannel();
						});
				}
					if (args.openDonate) {
						AddChoiceButton(
							container,
							RuEn(
								"Поддержать Astrogram",
								"Support Astrogram"),
							QString(),
							&st::menuIconGiftPremium,
							[=] {
								args.openDonate();
						});
				}
				Ui::AddSkip(container, st::settingsCheckboxesSkip / 2);
				setFooterButtons(
					RuEn("Завершить", "Finish"),
					finish,
					args.openOfficialChannel
						? RuEn("Канал", "Channel")
						: RuEn("Назад", "Back"),
					args.openOfficialChannel
						? Fn<void()>([=] { args.openOfficialChannel(); })
						: Fn<void()>([=] { goToStep(Step::ExperimentalTips); }));
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
