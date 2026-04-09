/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

#include <QtCore/QString>

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

struct ShellModePreferences {
	bool immersiveAnimation = true;
	bool expandedSidePanel = false;
	bool leftEdgeSettings = false;
	bool wideSettingsPane = false;
};

[[nodiscard]] QString ShellModePreferencesPath();
[[nodiscard]] ShellModePreferences LoadShellModePreferences();
[[nodiscard]] bool SaveShellModePreferences(
	const ShellModePreferences &prefs);

void AddMenuCustomizationEditor(
	not_null<Window::SessionController*> controller,
	not_null<Ui::VerticalLayout*> container);

} // namespace Settings
