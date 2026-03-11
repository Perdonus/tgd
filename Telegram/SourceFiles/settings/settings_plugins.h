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

private:
	void setupContent();
	void rebuildDeveloperSection();
	void rebuildList();
	void handleDocumentationTap();
	void toggleDeveloperMode();

	const not_null<Window::SessionController*> _controller;
	not_null<Ui::VerticalLayout*> _content;
	not_null<Ui::VerticalLayout*> _list;
	Ui::VerticalLayout *_developer = nullptr;
	QTimer *_developerTapTimer = nullptr;
	bool _developerMode = false;
	int _documentationTapCount = 0;
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

} // namespace Settings
