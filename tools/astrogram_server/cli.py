from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from .bot import run_bot_forever
from .http_api import serve_http, start_http_in_thread
from .storage import AstrogramServerStorage


DEFAULT_STATE_DIR = Path(
    os.environ.get(
        "ASTROGRAM_SERVER_STATE_DIR",
        Path(__file__).resolve().parent / "state",
    )
)
BADGE_KIND_CHOICES = ("server", "verified", "staff", "partner", "custom")
PEER_TYPE_CHOICES = ("user", "channel", "unknown")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="astrogram_server",
        description="Standalone Astrogram server tooling for badges and trusted sources.",
    )
    parser.add_argument(
        "--state-dir",
        default=str(DEFAULT_STATE_DIR),
        help="Directory for astrogram_server.sqlite3 and bot_state.json",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    serve = subparsers.add_parser("serve", help="Run only the HTTP service")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8099)

    bot = subparsers.add_parser("bot", help="Run only the Telegram admin bot")
    bot.add_argument("--poll-timeout", type=int, default=25)

    run = subparsers.add_parser("run", help="Run HTTP service and bot together")
    run.add_argument("--host", default="127.0.0.1")
    run.add_argument("--port", type=int, default=8099)
    run.add_argument("--poll-timeout", type=int, default=25)

    badge_set = subparsers.add_parser("badge-set", help="Set or update a peer badge")
    badge_set.add_argument("peer_id")
    badge_set.add_argument("--kind", choices=BADGE_KIND_CHOICES, default="server")
    badge_set.add_argument("--peer-type", choices=PEER_TYPE_CHOICES)
    badge_set.add_argument("--label", default="")
    badge_set.add_argument("--title")
    badge_set.add_argument("--username")
    badge_set.add_argument("--actor", default="cli")

    badge_clear = subparsers.add_parser("badge-clear", help="Remove a peer badge")
    badge_clear.add_argument("peer_id")
    badge_clear.add_argument("--actor", default="cli")

    badge_show = subparsers.add_parser("badge-show", help="Show a peer badge")
    badge_show.add_argument("peer_id")

    user_badge_set = subparsers.add_parser(
        "user-badge-set",
        help="Set or update a user badge",
    )
    user_badge_set.add_argument("user_id")
    user_badge_set.add_argument("--kind", choices=BADGE_KIND_CHOICES, default="server")
    user_badge_set.add_argument("--label", default="")
    user_badge_set.add_argument("--title")
    user_badge_set.add_argument("--username")
    user_badge_set.add_argument("--actor", default="cli")

    user_badge_clear = subparsers.add_parser(
        "user-badge-clear",
        help="Remove a user badge",
    )
    user_badge_clear.add_argument("user_id")
    user_badge_clear.add_argument("--actor", default="cli")

    user_badge_show = subparsers.add_parser("user-badge-show", help="Show a user badge")
    user_badge_show.add_argument("user_id")

    channel_badge_set = subparsers.add_parser(
        "channel-badge-set",
        help="Set or update a channel badge",
    )
    channel_badge_set.add_argument("channel_id")
    channel_badge_set.add_argument(
        "--kind",
        choices=BADGE_KIND_CHOICES,
        default="server",
    )
    channel_badge_set.add_argument("--label", default="")
    channel_badge_set.add_argument("--title")
    channel_badge_set.add_argument("--username")
    channel_badge_set.add_argument("--actor", default="cli")

    channel_badge_clear = subparsers.add_parser(
        "channel-badge-clear",
        help="Remove a channel badge",
    )
    channel_badge_clear.add_argument("channel_id")
    channel_badge_clear.add_argument("--actor", default="cli")

    channel_badge_show = subparsers.add_parser(
        "channel-badge-show",
        help="Show a channel badge",
    )
    channel_badge_show.add_argument("channel_id")

    source_set = subparsers.add_parser(
        "trusted-source-set",
        help="Set or update a trusted source channel",
    )
    source_set.add_argument("channel_id")
    source_set.add_argument("--label", default="")
    source_set.add_argument("--title")
    source_set.add_argument("--username")
    source_set.add_argument("--actor", default="cli")

    source_clear = subparsers.add_parser(
        "trusted-source-clear",
        help="Remove a trusted source channel",
    )
    source_clear.add_argument("channel_id")
    source_clear.add_argument("--actor", default="cli")

    source_show = subparsers.add_parser(
        "trusted-source-show",
        help="Show a trusted source channel",
    )
    source_show.add_argument("channel_id")

    plugin_set = subparsers.add_parser(
        "plugin-record-set",
        help="Set or update a plugin record by sha256",
    )
    plugin_set.add_argument("sha256")
    plugin_set.add_argument("channel_id")
    plugin_set.add_argument("message_id", type=int)
    plugin_set.add_argument("--label", default="")
    plugin_set.add_argument("--channel-title")
    plugin_set.add_argument("--channel-username")
    plugin_set.add_argument("--actor", default="cli")

    plugin_clear = subparsers.add_parser(
        "plugin-record-clear",
        help="Remove a plugin record by sha256",
    )
    plugin_clear.add_argument("sha256")
    plugin_clear.add_argument("--actor", default="cli")

    plugin_show = subparsers.add_parser(
        "plugin-record-show",
        help="Show a plugin record by sha256",
    )
    plugin_show.add_argument("sha256")

    snapshot = subparsers.add_parser("snapshot", help="Dump the full JSON snapshot")
    snapshot.add_argument("--pretty", action="store_true")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    storage = AstrogramServerStorage(args.state_dir)
    try:
        if args.command == "serve":
            serve_http(storage, host=args.host, port=args.port)
            return 0
        if args.command == "bot":
            run_bot_forever(storage, poll_timeout=args.poll_timeout)
            return 0
        if args.command == "run":
            server, _thread = start_http_in_thread(storage, host=args.host, port=args.port)
            try:
                run_bot_forever(storage, poll_timeout=args.poll_timeout)
            finally:
                server.shutdown()
                server.server_close()
            return 0
        if args.command == "badge-set":
            value, revision = storage.set_peer_badge(
                peer_id=args.peer_id,
                badge_kind=args.kind,
                label=args.label,
                actor=args.actor,
                peer_type=args.peer_type,
                title=args.title,
                username=args.username,
            )
            _print_json({"ok": True, "revision": revision, "badge": value}, pretty=True)
            return 0
        if args.command == "badge-clear":
            removed, revision = storage.clear_peer_badge(args.peer_id, actor=args.actor)
            _print_json({"ok": True, "revision": revision, "removed": removed}, pretty=True)
            return 0
        if args.command == "badge-show":
            value = storage.get_peer_badge(args.peer_id)
            _print_json({"ok": True, "found": value is not None, "badge": value}, pretty=True)
            return 0
        if args.command == "user-badge-set":
            value, revision = storage.set_user_badge(
                user_id=args.user_id,
                badge_kind=args.kind,
                label=args.label,
                actor=args.actor,
                title=args.title,
                username=args.username,
            )
            _print_json({"ok": True, "revision": revision, "badge": value}, pretty=True)
            return 0
        if args.command == "user-badge-clear":
            removed, revision = storage.clear_user_badge(args.user_id, actor=args.actor)
            _print_json({"ok": True, "revision": revision, "removed": removed}, pretty=True)
            return 0
        if args.command == "user-badge-show":
            value = storage.get_user_badge(args.user_id)
            _print_json({"ok": True, "found": value is not None, "badge": value}, pretty=True)
            return 0
        if args.command == "channel-badge-set":
            value, revision = storage.set_channel_badge(
                channel_id=args.channel_id,
                badge_kind=args.kind,
                label=args.label,
                actor=args.actor,
                title=args.title,
                username=args.username,
            )
            _print_json({"ok": True, "revision": revision, "badge": value}, pretty=True)
            return 0
        if args.command == "channel-badge-clear":
            removed, revision = storage.clear_channel_badge(
                args.channel_id,
                actor=args.actor,
            )
            _print_json({"ok": True, "revision": revision, "removed": removed}, pretty=True)
            return 0
        if args.command == "channel-badge-show":
            value = storage.get_channel_badge(args.channel_id)
            _print_json({"ok": True, "found": value is not None, "badge": value}, pretty=True)
            return 0
        if args.command == "trusted-source-set":
            value, revision = storage.set_trusted_source(
                channel_id=args.channel_id,
                label=args.label,
                title=args.title,
                username=args.username,
                actor=args.actor,
            )
            _print_json(
                {"ok": True, "revision": revision, "trusted_source": value},
                pretty=True,
            )
            return 0
        if args.command == "trusted-source-clear":
            removed, revision = storage.clear_trusted_source(
                args.channel_id,
                actor=args.actor,
            )
            _print_json({"ok": True, "revision": revision, "removed": removed}, pretty=True)
            return 0
        if args.command == "trusted-source-show":
            value = storage.get_trusted_source(args.channel_id)
            _print_json(
                {"ok": True, "found": value is not None, "trusted_source": value},
                pretty=True,
            )
            return 0
        if args.command == "plugin-record-set":
            value, revision = storage.set_plugin_record(
                sha256=args.sha256,
                channel_id=args.channel_id,
                message_id=args.message_id,
                label=args.label,
                actor=args.actor,
                channel_title=args.channel_title,
                channel_username=args.channel_username,
            )
            _print_json(
                {"ok": True, "revision": revision, "plugin_record": value},
                pretty=True,
            )
            return 0
        if args.command == "plugin-record-clear":
            removed, revision = storage.clear_plugin_record(args.sha256, actor=args.actor)
            _print_json({"ok": True, "revision": revision, "removed": removed}, pretty=True)
            return 0
        if args.command == "plugin-record-show":
            value = storage.get_plugin_record(args.sha256)
            _print_json(
                {"ok": True, "found": value is not None, "plugin_record": value},
                pretty=True,
            )
            return 0
        if args.command == "snapshot":
            _print_json(storage.export_snapshot(), pretty=args.pretty)
            return 0
    finally:
        storage.close()
    return 1


def _print_json(payload: dict, pretty: bool = False) -> None:
    if pretty:
        print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    raise SystemExit(main())
