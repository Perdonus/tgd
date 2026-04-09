/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include <functional>
#include <vector>

namespace Window {
class SessionController;
} // namespace Window

class PeerData;

namespace Ui {

enum class AstrogramOnboardingPreset {
	Recommended,
	Private,
	Minimal,
};

struct AstrogramOnboardingPlugin {
	QString title;
	QString description;
	QString sourceLabel;
	qint64 postId = 0;
	std::function<void()> install;
};

struct AstrogramOnboardingArgs {
	Window::SessionController *controller = nullptr;
	PeerData *pluginsChannelPeer = nullptr;
	QString pluginsChannelTitle;
	QString pluginsChannelSubtitle;
	qint64 pluginsChannelId = 0;
	PeerData *officialChannelPeer = nullptr;
	QString officialChannelTitle;
	QString officialChannelSubtitle;
	qint64 officialChannelId = 0;
	std::function<void(std::function<void(PeerData*)>)> resolvePluginsChannel;
	std::function<void(std::function<void(PeerData*)>)> resolveOfficialChannel;
	std::vector<AstrogramOnboardingPlugin> plugins;
	std::function<void(AstrogramOnboardingPreset)> applyPreset;
	std::function<void()> subscribePluginsChannel;
	std::function<void()> openAllPlugins;
	std::function<void()> openOfficialChannel;
	std::function<void()> openDonate;
	std::function<void()> finished;
};

void ShowAstrogramOnboardingBox(AstrogramOnboardingArgs args);

} // namespace Ui
