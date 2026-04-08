/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class VerticalLayout;
} // namespace Ui

class QTimer;

namespace Settings {

class Plugins : public Section<Plugins> {
public:
	Plugins(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;
	void fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) override;

private:
	void setupContent();
	void rebuildList();
	void refreshPending();
	void scheduleRebuildList(int delayMs = 0);
	void setListInteractive(bool enabled);

	const not_null<Window::SessionController*> _controller;
	not_null<Ui::VerticalLayout*> _content;
	not_null<Ui::VerticalLayout*> _list;
	bool _rebuildScheduled = false;
	bool _listRefreshPending = true;
};

class PluginsDocumentation : public Section<PluginsDocumentation> {
public:
	PluginsDocumentation(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

	const not_null<Window::SessionController*> _controller;
};

[[nodiscard]] Type PluginDetailsId(const QString &pluginId);

} // namespace Settings
