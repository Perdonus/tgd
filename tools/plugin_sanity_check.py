#!/usr/bin/env python3
"""
Fast non-build sanity checks for plugin runtime invariants.

These checks are intentionally simple and text-based, so they can run in
seconds in CI without compiling Telegram Desktop.
They are used by the "Quick Plugin Checks" GitHub Actions workflow.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CPP = ROOT / "Telegram/SourceFiles/plugins/plugins_manager.cpp"
HDR = ROOT / "Telegram/SourceFiles/plugins/plugins_manager.h"
API = ROOT / "Telegram/SourceFiles/plugins/plugins_api.h"
EXAMPLES_DIR = ROOT / "Telegram/Plugins/Examples"
PLUGIN_CATALOG_DIR = ROOT / "PluginCatalog"
SETTINGS_UI = ROOT / "Telegram/SourceFiles/settings/settings_plugins.cpp"

EXPECTED_EXAMPLES = {
    "ai_chat.cpp",
    "font_tuner.cpp",
    "show_logs.cpp",
    "transparent_telegram.cpp",
}


def require(pattern: str, text: str, name: str, errors: list[str]) -> None:
    if re.search(pattern, text, flags=re.MULTILINE | re.DOTALL) is None:
        errors.append(name)


def load_plugin_source(path: pathlib.Path, errors: list[str], prefix: str) -> str:
    text = path.read_text(encoding="utf-8")
    include_only = re.fullmatch(r'\s*#include\s+"([^"]+)"\s*', text)
    if include_only is None:
        return text

    target = (path.parent / include_only.group(1)).resolve()
    try:
        target.relative_to(ROOT)
    except ValueError:
        errors.append(f"{prefix}: include-wrapper points outside the repository")
        return text

    if not target.exists():
        errors.append(f"{prefix}: include-wrapper target is missing")
        return text

    return target.read_text(encoding="utf-8")


def extract_host_method_names(api_text: str) -> list[str]:
    host_block = re.search(
        r"class Host\s*\{(?P<body>[\s\S]*?)\n\};",
        api_text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if host_block is None:
        return []
    body = host_block.group("body")
    result: list[str] = []
    for signature in re.findall(
        r"virtual\s+[^;]*=\s*0\s*;",
        body,
        flags=re.MULTILINE | re.DOTALL,
    ):
        match = re.search(
            r"virtual\s+.*?\b([A-Za-z_]\w*)\s*\(",
            signature,
            flags=re.MULTILINE | re.DOTALL,
        )
        if match is not None:
            result.append(match.group(1))
    return sorted(set(result))


def check_plugin_source(
    path: pathlib.Path,
    host_methods: set[str],
    errors: list[str],
    prefix: str,
) -> None:
    text = load_plugin_source(path, errors, prefix)
    require(
        r'#include\s+"plugins/plugins_api.h"',
        text,
        f"{prefix}: includes plugins_api.h",
        errors,
    )
    require(
        r"TGD_PLUGIN_ENTRY\s*\{",
        text,
        f"{prefix}: defines TGD_PLUGIN_ENTRY",
        errors,
    )
    require(
        r"TGD_PLUGIN_PREVIEW\s*\(",
        text,
        f"{prefix}: defines TGD_PLUGIN_PREVIEW",
        errors,
    )
    require(
        r"apiVersion\s*!=\s*Plugins::kApiVersion",
        text,
        f"{prefix}: has exact apiVersion compatibility check",
        errors,
    )
    if re.search(r"QStringLiteral\s*\(\s*k[A-Za-z_]\w*\s*\)", text):
        errors.append(
            f"{prefix}: uses QStringLiteral() with a k* constant instead of a string literal"
        )
    if re.search(r"\.pop_front\s*\(", text):
        errors.append(
            f"{prefix}: uses pop_front(), which is not portable for the Qt containers we build against"
        )

    used_methods = set(re.findall(r"_host->([A-Za-z_]\w*)\s*\(", text))
    for method in sorted(used_methods):
        if method not in host_methods:
            errors.append(f"{prefix}: uses unknown Host method _host->{method}()")


def check_examples(host_methods: set[str], errors: list[str]) -> None:
    if not EXAMPLES_DIR.exists():
        errors.append("Examples directory exists")
        return

    sources = sorted(EXAMPLES_DIR.glob("*.cpp"))
    source_names = {path.name for path in sources}
    missing = sorted(EXPECTED_EXAMPLES - source_names)
    for filename in missing:
        errors.append(f"missing example source: {filename}")

    for path in sources:
        check_plugin_source(path, host_methods, errors, path.name)


def check_catalog(host_methods: set[str], errors: list[str]) -> None:
    if not PLUGIN_CATALOG_DIR.exists():
        errors.append("PluginCatalog directory exists")
        return

    sources = sorted(PLUGIN_CATALOG_DIR.glob("*/*/*.cpp"))
    if not sources:
        errors.append("PluginCatalog contains versioned plugin sources")
        return

    for path in sources:
        rel = path.relative_to(PLUGIN_CATALOG_DIR)
        if len(rel.parts) != 3:
            errors.append(f"{rel}: must be PluginCatalog/<plugin>/<version>/<source>.cpp")
            continue

        plugin_name, version_name, filename = rel.parts
        if path.stem != plugin_name:
            errors.append(f"{rel}: source filename must match plugin folder name")
        if re.fullmatch(r"\d+\.\d+(?:\.\d+)?", version_name) is None:
            errors.append(f"{rel}: version folder must look like 1.0 or 1.0.0")

        binary = path.with_suffix(".tgd")
        if binary.exists() and binary.stem != plugin_name:
            errors.append(f"{binary.relative_to(PLUGIN_CATALOG_DIR)}: binary filename must match plugin folder name")

        check_plugin_source(path, host_methods, errors, str(rel))


def main() -> int:
    errors: list[str] = []

    if not CPP.exists() or not HDR.exists() or not API.exists():
        print("Required plugin manager files are missing.")
        return 2

    cpp = CPP.read_text(encoding="utf-8")
    hdr = HDR.read_text(encoding="utf-8")
    api = API.read_text(encoding="utf-8")

    require(
        r"constexpr int kApiVersion = [1-9]\d*;",
        api,
        "positive kApiVersion constant",
        errors,
    )
    require(
        r"constexpr int kBinaryInfoVersion = [1-9]\d*;",
        api,
        "positive kBinaryInfoVersion constant",
        errors,
    )
    require(
        r"TgdPluginBinaryInfo",
        api,
        "plugins_api.h exports TgdPluginBinaryInfo",
        errors,
    )
    require(
        r"TgdPluginPreviewInfo",
        api,
        "plugins_api.h exports TgdPluginPreviewInfo",
        errors,
    )

    host_methods = extract_host_method_names(api)
    if not host_methods:
        errors.append("extract Host methods from plugins_api.h")
    host_method_set = set(host_methods)

    # Core runtime guards.
    require(r"bool Manager::hasPlugin\(", cpp, "hasPlugin() implementation", errors)
    require(
        r"void Manager::disablePlugin\([^)]*\)\s*\{[\s\S]*saveConfig\(\);",
        cpp,
        "disablePlugin() persists config",
        errors,
    )
    require(
        r"library->resolve\(kBinaryInfoName\)",
        cpp,
        "loader resolves TgdPluginBinaryInfo",
        errors,
    )
    require(
        r"DescribeBinaryInfoMismatch",
        cpp,
        "loader validates plugin binary metadata",
        errors,
    )

    # Ensure disable path unregisters every registry.
    for call in (
        "unregisterPluginCommands",
        "unregisterPluginActions",
        "unregisterPluginPanels",
        "unregisterPluginSettingsPages",
        "unregisterPluginOutgoingInterceptors",
        "unregisterPluginMessageObservers",
        "unregisterPluginWindowHandlers",
        "unregisterPluginWindowWidgetHandlers",
        "unregisterPluginSessionHandlers",
    ):
        require(
            rf"disablePlugin\([^)]*\)\s*\{{[\s\S]*{call}\(",
            cpp,
            f"disablePlugin() calls {call}()",
            errors,
        )

    # Registration guards by plugin ownership.
    for guard in (
        r"CommandId Manager::registerCommand\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler\)",
        r"ActionId Manager::registerAction\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler",
        r"ActionId Manager::registerActionWithContext\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler",
        r"OutgoingInterceptorId Manager::registerOutgoingTextInterceptor\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler\)",
        r"MessageObserverId Manager::registerMessageObserver\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler\)",
        r"PanelId Manager::registerPanel\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler",
        r"SettingsPageId Manager::registerSettingsPage\([^)]*\)\s*\{[\s\S]*if \(!hasPlugin\(pluginId\) \|\| !handler",
    ):
        require(guard, cpp, f"ownership/handler guard: {guard}", errors)

    # Host API contract -> Manager declarations and definitions.
    for method in host_methods:
        require(
            rf"\b{re.escape(method)}\s*\([^;]*\)\s*(?:const\s*)?override\s*;",
            hdr,
            f"Manager overrides Host::{method}",
            errors,
        )
        require(
            rf"\bManager::{re.escape(method)}\s*\(",
            cpp,
            f"Manager::{method} implementation",
            errors,
        )

    # Snapshot iteration to avoid mutation invalidation during callbacks.
    require(
        r"const auto observerSnapshot = \[=\]",
        cpp,
        "observer snapshot lambda",
        errors,
    )
    require(
        r"const auto snapshot = observerSnapshot\(\);",
        cpp,
        "observer snapshot usage",
        errors,
    )

    # Header tracks plugin IDs for window/session handlers.
    require(
        r"struct WindowHandlerEntry\s*\{[\s\S]*QString pluginId;",
        hdr,
        "WindowHandlerEntry stores pluginId",
        errors,
    )
    require(
        r"struct SessionHandlerEntry\s*\{[\s\S]*QString pluginId;",
        hdr,
        "SessionHandlerEntry stores pluginId",
        errors,
    )

    # Examples remain compatible with Host methods.
    check_examples(host_method_set, errors)
    check_catalog(host_method_set, errors)

    if SETTINGS_UI.exists():
        settings_ui = SETTINGS_UI.read_text(encoding="utf-8")
        require(
            r"settingsPagesFor\s*\(",
            settings_ui,
            "settings_plugins.cpp renders plugin settings pages",
            errors,
        )
        require(
            r"updateSetting\s*\(",
            settings_ui,
            "settings_plugins.cpp pushes setting updates back to manager",
            errors,
        )

    if errors:
        print("plugin_sanity_check: FAILED")
        for item in errors:
            print(f"- {item}")
        return 1

    print("plugin_sanity_check: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
