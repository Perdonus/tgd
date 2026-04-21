/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

#include <QtCore/QStringList>

namespace Settings {

struct ShellModePreferences;

enum class AstrogramShellPreset {
	Balanced,
	Focused,
	Wide,
};

[[nodiscard]] ShellModePreferences ShellModePreferencesFor(
	AstrogramShellPreset preset);
[[nodiscard]] bool ApplyAstrogramShellPreset(
	AstrogramShellPreset preset);
[[nodiscard]] QStringList LoadAstrogramShellLayoutOrder();

class Experimental : public Section<Experimental> {
public:
	Experimental(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent(not_null<Window::SessionController*> controller);

};

} // namespace Settings
