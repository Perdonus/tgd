/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_common_session.h"

#include "settings/cloud_password/settings_cloud_password_email_confirm.h"
#include "settings/settings_astrogram.h"
#include "settings/settings_experimental.h"
#include "settings/settings_information.h"
#include "settings/settings_plugins.h"
#include "settings/settings_privacy_security.h"
#include "settings/settings_chat.h"
#include "settings/settings_main.h"

namespace Settings {

bool HasMenu(Type type) {
	return (type == ::Settings::CloudPasswordEmailConfirmId())
		|| (type == Main::Id())
		|| (type == Chat::Id())
		|| (type == Plugins::Id());
}

bool UsesExperimentalShellGeometry(Type type) {
	return (type == Main::Id())
		|| (type == Information::Id())
		|| (type == PrivacySecurity::Id())
		|| (type == Chat::Id())
		|| (type == Plugins::Id())
		|| (type == Astrogram::Id())
		|| (type == AstrogramCore::Id())
		|| (type == AstrogramPrivacy::Id())
		|| (type == AstrogramInterface::Id())
		|| (type == AstrogramAntiRecall::Id())
		|| (type == AstrogramLinks::Id())
		|| (type == Experimental::Id());
}

} // namespace Settings
