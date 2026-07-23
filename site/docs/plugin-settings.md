# Plugin Settings

Astrogram supports host-rendered settings pages. This is the preferred UI path for stable plugins.

## Why host-rendered settings

Legacy `registerPanel()` still exists, but raw plugin-owned dialogs are less reliable than host-rendered controls inside `Settings > Plugins`.

Use settings pages for:

- toggles
- sliders
- text inputs
- action buttons
- read-only info blocks

## Supported controls

```cpp
enum class SettingControl {
	Toggle,
	IntSlider,
	TextInput,
	ActionButton,
	InfoText,
};
```

## Minimal slider example

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
	{
		.id = "my_plugin",
		.title = "My Plugin",
		.sections = { section },
	},
	[=](const Plugins::SettingDescriptor &setting) {
		if (setting.id == "opacity") {
			_host->showToast(QString::number(setting.intValue));
		}
	});
```

## Full settings page example

```cpp
void MyPlugin::registerSettings() {
	Plugins::SettingDescriptor enabled;
	enabled.id = "enabled";
	enabled.title = "Enable plugin";
	enabled.description = "Turns the runtime behavior on or off.";
	enabled.type = Plugins::SettingControl::Toggle;
	enabled.boolValue = _host->settingBoolValue(id(), "enabled", true);

	Plugins::SettingDescriptor apiKey;
	apiKey.id = "api_key";
	apiKey.title = "API key";
	apiKey.description = "Used for remote requests.";
	apiKey.type = Plugins::SettingControl::TextInput;
	apiKey.textValue = _host->settingStringValue(id(), "api_key", QString());
	apiKey.placeholderText = "Paste your key";
	apiKey.secret = true;

	Plugins::SettingDescriptor openSite;
	openSite.id = "open_site";
	openSite.title = "Open API site";
	openSite.description = "Open the provider website in your browser.";
	openSite.type = Plugins::SettingControl::ActionButton;

	Plugins::SettingDescriptor status;
	status.id = "status";
	status.title = "Status";
	status.description = _host->settingStringValue(id(), "api_key", QString()).isEmpty()
		? "API key is missing."
		: "Plugin is configured.";
	status.type = Plugins::SettingControl::InfoText;

	Plugins::SettingsSectionDescriptor general;
	general.id = "general";
	general.title = "General";
	general.settings = { enabled, apiKey, status, openSite };

	_settingsPageId = _host->registerSettingsPage(
		id(),
		{
			.id = "my_plugin_settings",
			.title = "My Plugin",
			.description = "Host-rendered settings page.",
			.sections = { general },
		},
		[=](const Plugins::SettingDescriptor &setting) {
			if (setting.id == "open_site") {
				_host->openUrl("https://example.org");
			}
		});
}
```

## Reading persisted values

The host stores setting values for you.

```cpp
const auto opacity = _host->settingIntValue(
	"example.my_plugin",
	"opacity",
	85);
```

Also available:

- `storedSettingValue(...)`
- `settingBoolValue(...)`
- `settingIntValue(...)`
- `settingStringValue(...)`

## Text inputs

Use `TextInput` for API keys, URLs, names, or other user text:

```cpp
Plugins::SettingDescriptor apiKey;
apiKey.id = "api_key";
apiKey.title = "API key";
apiKey.description = "Token used for remote requests.";
apiKey.type = Plugins::SettingControl::TextInput;
apiKey.textValue = "";
apiKey.placeholderText = "Paste your key";
apiKey.secret = true;
```

## Reacting to setting changes

The settings callback receives the updated descriptor, so you can immediately apply the new value:

```cpp
_settingsPageId = _host->registerSettingsPage(
	id(),
	page,
	[=](const Plugins::SettingDescriptor &setting) {
		if (setting.id == "enabled") {
			_enabled = setting.boolValue;
			return;
		}
		if (setting.id == "opacity") {
			_opacity = setting.intValue;
			applyToAllWindows();
			return;
		}
		if (setting.id == "api_key") {
			_apiKey = setting.textValue;
		}
	});
```

## Recommended patterns

- Keep settings pages small and task-focused.
- Prefer `ActionButton` for “Open site”, “Export”, or “Reset”.
- Use `InfoText` for status or developer notes.
- Store runtime-critical values through the host instead of ad hoc local files.
- Use stable setting IDs so user data survives plugin updates.
- Treat the settings page as a view layer; actual runtime state should live in your plugin object.
