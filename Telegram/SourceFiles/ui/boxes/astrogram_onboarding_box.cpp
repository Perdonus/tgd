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
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/ministar_particles.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/top_background_gradient.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/wrap/slide_wrap.h"
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

#include <memory>
#include <optional>

namespace Ui {
namespace {

constexpr auto kStepCount = 5;

enum class Step {
	Welcome = 0,
	Presets = 1,
	PluginsInfo = 2,
	PluginsInstall = 3,
	Finish = 4,
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
	auto button = object_ptr<Ui::LinkButton>(
		container,
		rpl::single(text),
		st::defaultLinkButton);
	const auto raw = container->add(std::move(button),
		style::margins(st::boxRowPadding.left(), 0, st::boxRowPadding.right(), 0));
	raw->setClickedCallback(std::move(callback));
	return raw;
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
		auto label = object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(description),
			st::defaultFlatLabel);
		container->add(std::move(label),
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

		struct State {
			Step step = Step::Welcome;
			PeerData *pluginsChannelPeer = nullptr;
			PeerData *officialChannelPeer = nullptr;
		};
		const auto state = box->lifetime().make_state<State>(State{
			.pluginsChannelPeer = args.pluginsChannelPeer,
			.officialChannelPeer = args.officialChannelPeer,
		});
		const auto weak = base::make_weak(box);

		const auto finish = [=] {
			if (args.finished) {
				args.finished();
			}
			box->closeBox();
		};

		// Build step containers wrapped in SlideWrap for smooth transitions.
		auto stepWraps = std::vector<
			not_null<Ui::SlideWrap<Ui::VerticalLayout>*>>();
		stepWraps.reserve(kStepCount);

		for (auto i = 0; i < kStepCount; ++i) {
			const auto wrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					container,
					object_ptr<Ui::VerticalLayout>(container)));
			wrap->toggle(false, anim::type::instant);
			stepWraps.push_back(wrap);
		}

		const auto showStep = [=](Step step) {
			state->step = step;
			for (auto i = 0; i < kStepCount; ++i) {
				const auto isCurrent = (Step(i) == step);
				stepWraps[i]->toggle(isCurrent, anim::type::normal);
			}
		};

		// Welcome step
		{
			const auto inner = stepWraps[0]->entity();
			AddHeroCover(
				inner,
				RuEn("Добро пожаловать в", "Welcome to"),
				RuEn(
					"Astrogram Desktop\nКлиент уже готов к работе, дальше поможем быстро настроить самое важное.",
					"Astrogram Desktop\nThe client is ready to use, next we will quickly tune the important parts."),
				QColor(0x08, 0x15, 0x11),
				QColor(0x10, 0x2b, 0x20),
				QColor(0x2d, 0xd1, 0x85),
				QColor(0xa1, 0xff, 0xcc));
			Ui::AddSkip(inner, st::settingsCheckboxesSkip);
			AddPrimaryButton(inner, RuEn("Продолжить", "Continue"), [=] {
				showStep(Step::Presets);
			});
		}

		// Presets step
		{
			const auto inner = stepWraps[1]->entity();
			AddHeroCover(
				inner,
				RuEn("Выбери пресет", "Choose a preset"),
				RuEn(
					"Astrogram Desktop\nЭто можно поменять позже в настройках Astrogram.",
					"Astrogram Desktop\nYou can change this later in Astrogram settings."),
				QColor(0x08, 0x15, 0x11),
				QColor(0x10, 0x2b, 0x20),
				QColor(0x2d, 0xd1, 0x85),
				QColor(0xa1, 0xff, 0xcc));
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddChoiceButton(
				inner,
				RuEn("Рекомендованный", "Recommended"),
				RuEn(
					"Включает полезные Astrogram-функции без агрессивных изменений интерфейса.",
					"Enables useful Astrogram features without aggressive interface changes."),
				&st::menuIconPremium,
				[=] {
					if (args.applyPreset) {
						args.applyPreset(AstrogramOnboardingPreset::Recommended);
					}
					showStep(Step::PluginsInfo);
				});
			AddChoiceButton(
				inner,
				RuEn("Приватный", "Private"),
				RuEn(
					"Делает упор на ghost mode, защиту от удаления и более осторожное поведение клиента.",
					"Focuses on ghost mode, anti-recall and a more private client setup."),
				&st::menuIconLock,
				[=] {
					if (args.applyPreset) {
						args.applyPreset(AstrogramOnboardingPreset::Private);
					}
					showStep(Step::PluginsInfo);
				});
			AddChoiceButton(
				inner,
				RuEn("Минимальный", "Minimal"),
				RuEn(
					"Оставляет интерфейс ближе к Telegram Desktop и включает только базовые улучшения Astrogram.",
					"Keeps the interface closer to Telegram Desktop and enables only core Astrogram improvements."),
				&st::menuIconPalette,
				[=] {
					if (args.applyPreset) {
						args.applyPreset(AstrogramOnboardingPreset::Minimal);
					}
					showStep(Step::PluginsInfo);
				});
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddLinkAction(inner, RuEn("Выбрать позже", "Choose later"), [=] {
				showStep(Step::PluginsInfo);
			});
			Ui::AddSkip(inner, st::settingsCheckboxesSkip);
		}

		// Plugins Info step
		{
			const auto inner = stepWraps[2]->entity();
			AddHeroCover(
				inner,
				RuEn("Плагины", "Plugins"),
				RuEn(
					"Astrogram Desktop\nПлагины расширяют Astrogram: добавляют новые функции, настройки и полезные сценарии прямо внутри клиента.",
					"Astrogram Desktop\nPlugins extend Astrogram with extra features, settings and useful workflows directly inside the client."),
				QColor(0x08, 0x13, 0x1f),
				QColor(0x0f, 0x2b, 0x3f),
				QColor(0x44, 0xc0, 0xff),
				QColor(0x9c, 0xff, 0xd3));
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddChoiceButton(
				inner,
				RuEn("Быстрая установка рекомендуемых плагинов", "Install recommended plugins"),
				RuEn(
					"Мы покажем несколько безопасных пакетов и дадим установить их в пару кликов.",
					"We'll show a few safe packages and let you install them in a couple of clicks."),
				&st::menuIconCustomize,
				[=] {
					showStep(Step::PluginsInstall);
				});
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddPrimaryButton(inner, RuEn("Продолжить", "Continue"), [=] {
				showStep(Step::PluginsInstall);
			});
			AddLinkAction(inner, RuEn("Позже", "Later"), [=] {
				showStep(Step::Finish);
			});
		}

		// Plugins Install step
		{
			const auto inner = stepWraps[3]->entity();
			AddHeroCover(
				inner,
				RuEn("Рекомендуемые плагины", "Recommended plugins"),
				RuEn(
					"Astrogram Desktop\nВыбирай, что поставить сразу. Позже можно вернуться к этому списку из раздела плагинов.",
					"Astrogram Desktop\nChoose what to install right now. You can come back to this list later from the plugins section."),
				QColor(0x08, 0x13, 0x1f),
				QColor(0x0f, 0x2b, 0x3f),
				QColor(0x44, 0xc0, 0xff),
				QColor(0x9c, 0xff, 0xd3));
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 3);

			AddPeerChoiceButton(
				inner,
				state->pluginsChannelPeer,
				args.pluginsChannelTitle.isEmpty()
					? RuEn("AstroPlugins", "AstroPlugins")
					: args.pluginsChannelTitle,
				args.pluginsChannelSubtitle,
				[=] {
					if (args.subscribePluginsChannel) {
						args.subscribePluginsChannel();
					}
				});
			AddPrimaryButton(inner, RuEn("Подписаться", "Follow"), [=] {
				if (args.subscribePluginsChannel) {
					args.subscribePluginsChannel();
				}
			});
			if (args.plugins.empty()) {
				auto label = object_ptr<Ui::FlatLabel>(
					inner,
					rpl::single(RuEn(
						"Сервер ещё не передал рекомендованные пакеты. Они появятся здесь автоматически.",
						"The server has not provided recommended packages yet. They will appear here automatically.")),
					st::boxLabel);
				inner->add(std::move(label),
					style::margins(st::boxRowPadding.left(), 8, st::boxRowPadding.right(), 0),
					style::al_top);
			} else {
				for (const auto &plugin : args.plugins) {
					const auto install = plugin.install;
					AddChoiceButton(
						inner,
						plugin.title,
						plugin.description,
						&st::menuIconDownload,
						[install] {
							if (install) {
								install();
							}
						});
					if (!plugin.sourceLabel.trimmed().isEmpty()) {
						auto srcLabel = object_ptr<Ui::FlatLabel>(
							inner,
							rpl::single(plugin.sourceLabel.trimmed()),
							st::defaultFlatLabel);
						inner->add(std::move(srcLabel),
							style::margins(26, -2, 26, 4),
							style::al_top);
					}
					AddPrimaryButton(
						inner,
						RuEn("Установить", "Install"),
						[install] {
							if (install) {
								install();
							}
						});
					Ui::AddSkip(inner, st::settingsCheckboxesSkip / 3);
				}
			}
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddPrimaryButton(inner, RuEn("Продолжить", "Continue"), [=] {
				showStep(Step::Finish);
			});
			AddLinkAction(inner, RuEn("Посмотреть все", "View all"), [=] {
				if (args.openAllPlugins) {
					args.openAllPlugins();
				}
			});
		}

		// Finish step
		{
			const auto inner = stepWraps[4]->entity();
			AddHeroCover(
				inner,
				RuEn("Всё готово", "You're all set"),
				RuEn(
					"Astrogram Desktop\nНастройка завершена. Подпишись на официальный канал Astrogram и при желании поддержи разработку.",
					"Astrogram Desktop\nSetup is complete. Follow the official Astrogram channel and support development if you want."),
				QColor(0x08, 0x13, 0x1f),
				QColor(0x0f, 0x2b, 0x3f),
				QColor(0x44, 0xc0, 0xff),
				QColor(0x9c, 0xff, 0xd3));
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddPeerChoiceButton(
				inner,
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
				inner,
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
			Ui::AddSkip(inner, st::settingsCheckboxesSkip / 2);
			AddPrimaryButton(inner, RuEn("Завершить", "Finish"), finish);
		}

		// Show the current step and resolve peer data.
		showStep(Step::Welcome);

		if (!state->pluginsChannelPeer && args.resolvePluginsChannel) {
			args.resolvePluginsChannel([=](PeerData *peer) {
				if (!peer) {
					return;
				}
				state->pluginsChannelPeer = peer;
				if (weak.get() && (state->step == Step::PluginsInstall)) {
					showStep(Step::PluginsInstall);
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
					showStep(Step::Finish);
				}
			});
		}
	}));
}

} // namespace Ui