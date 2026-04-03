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

## Recommended patterns

- Keep settings pages small and task-focused.
- Prefer `ActionButton` for “Open site”, “Export”, or “Reset”.
- Use `InfoText` for status or developer notes.
- Store runtime-critical values through the host instead of ad hoc local files.
