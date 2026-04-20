from __future__ import annotations

import json
import logging
import os
import shlex
import time
from dataclasses import dataclass
from typing import Any
from urllib import error, parse, request

from .storage import (
    AstrogramServerStorage,
    infer_peer_type,
    normalize_text,
    normalize_username,
)


LOG = logging.getLogger("astrogram_server.bot")
DEFAULT_ADMIN_USER_ID = 6603471853
KNOWN_BADGE_KINDS = {"server", "verified", "staff", "partner", "custom"}
MAX_TELEGRAM_MESSAGE_LENGTH = 4000


def _looks_like_sha256(value: str) -> bool:
    return len(value) == 64 and all(ch in "0123456789abcdefABCDEF" for ch in value)


@dataclass
class BotCommandResult:
    ok: bool
    text: str


@dataclass(frozen=True)
class TelegramCommandSpec:
    command: str
    description: str


class AstrogramBadgeBot:
    def __init__(
        self,
        storage: AstrogramServerStorage,
        token: str,
        admin_user_id: int = DEFAULT_ADMIN_USER_ID,
        poll_timeout: int = 25,
        actor_name: str = "badge-bot",
    ) -> None:
        self._storage = storage
        self._token = token
        self._admin_user_id = int(admin_user_id)
        self._poll_timeout = max(1, int(poll_timeout))
        self._actor_name = actor_name
        self._base_url = f"https://api.telegram.org/bot{self._token}"
        self._commands_synced = False

    @classmethod
    def from_env(
        cls,
        storage: AstrogramServerStorage,
        poll_timeout: int = 25,
    ) -> "AstrogramBadgeBot":
        token = os.environ.get("ASTROGRAM_BADGE_BOT_TOKEN", "").strip()
        if not token:
            raise RuntimeError("ASTROGRAM_BADGE_BOT_TOKEN is not set")
        return cls(
            storage,
            token=token,
            admin_user_id=DEFAULT_ADMIN_USER_ID,
            poll_timeout=poll_timeout,
        )

    def run_forever(self) -> None:
        backoff = 2.0
        while True:
            try:
                self._ensure_bot_setup()
                self._poll_once()
                backoff = 2.0
            except KeyboardInterrupt:
                raise
            except Exception as exc:  # noqa: BLE001
                LOG.exception("bot polling failed: %s", exc)
                self._commands_synced = False
                time.sleep(backoff)
                backoff = min(backoff * 2.0, 30.0)

    def _ensure_bot_setup(self) -> None:
        if self._commands_synced:
            return
        self._sync_telegram_commands()
        self._commands_synced = True

    def _sync_telegram_commands(self) -> None:
        commands = [
            {
                "command": item.command,
                "description": item.description,
            }
            for item in self._telegram_command_specs()
        ]
        self._api(
            "setMyCommands",
            {
                "commands": json.dumps(commands, ensure_ascii=False),
            },
        )

    def _poll_once(self) -> None:
        state = self._storage.load_bot_state()
        offset = int(state.get("offset", 0))
        payload = {
            "timeout": self._poll_timeout,
            "offset": offset,
            "allowed_updates": json.dumps(["message"]),
        }
        response = self._api("getUpdates", payload)
        for update in response.get("result", []):
            update_id = int(update["update_id"])
            self._handle_update(update)
            offset = max(offset, update_id + 1)
            self._storage.save_bot_state({"offset": offset})

    def _handle_update(self, update: dict[str, Any]) -> None:
        message = update.get("message") or {}
        from_user = message.get("from") or {}
        user_id = int(from_user.get("id", 0))
        if user_id != self._admin_user_id:
            return
        text = (message.get("text") or "").strip()
        if not text:
            return
        result = self._dispatch_command(text)
        chat_id = message.get("chat", {}).get("id")
        if chat_id is not None:
            self._send_message(int(chat_id), result.text)

    def _dispatch_command(self, text: str) -> BotCommandResult:
        try:
            parts = shlex.split(text)
        except ValueError as exc:
            return BotCommandResult(False, f"parse error: {exc}")
        if not parts:
            return BotCommandResult(False, "empty command")
        command = parts[0].split("@", 1)[0].lower()
        args = parts[1:]
        try:
            if command in {"/help", "/start"}:
                return BotCommandResult(True, self._help_text())
            if command == "/ping":
                return BotCommandResult(True, "pong")
            if command == "/set_badge":
                return self._cmd_set_badge(args)
            if command == "/set_user_badge":
                return self._cmd_set_badge(args, peer_type="user")
            if command == "/set_channel_badge":
                return self._cmd_set_badge(args, peer_type="channel")
            if command == "/clear_badge":
                return self._cmd_clear_badge(args)
            if command == "/clear_user_badge":
                return self._cmd_clear_badge(args, peer_type="user")
            if command == "/clear_channel_badge":
                return self._cmd_clear_badge(args, peer_type="channel")
            if command == "/show_badge":
                return self._cmd_show_badge(args)
            if command == "/show_user_badge":
                return self._cmd_show_badge(args, peer_type="user")
            if command == "/show_channel_badge":
                return self._cmd_show_badge(args, peer_type="channel")
            if command == "/set_trusted_source":
                return self._cmd_set_trusted_source(args)
            if command == "/clear_trusted_source":
                return self._cmd_clear_trusted_source(args)
            if command == "/show_trusted_source":
                return self._cmd_show_trusted_source(args)
            if command == "/set_plugin_record":
                return self._cmd_set_plugin_record(args)
            if command == "/clear_plugin_record":
                return self._cmd_clear_plugin_record(args)
            if command == "/show_plugin_record":
                return self._cmd_show_plugin_record(args)
            if command == "/snapshot":
                snapshot = self._storage.export_snapshot()
                return BotCommandResult(
                    True,
                    json.dumps(snapshot, ensure_ascii=False, indent=2, sort_keys=True),
                )
        except Exception as exc:  # noqa: BLE001
            return BotCommandResult(False, f"error: {exc}")
        return BotCommandResult(False, f"unknown command: {command}")

    def _cmd_set_badge(
        self,
        args: list[str],
        peer_type: str | None = None,
    ) -> BotCommandResult:
        if not args:
            return BotCommandResult(False, f"usage: {self._badge_usage('set', peer_type)}")
        peer_id = args[0]
        badge_kind = "server"
        label_start = 1
        if len(args) >= 2 and args[1].lower() in KNOWN_BADGE_KINDS:
            badge_kind = args[1].lower()
            label_start = 2
        label = " ".join(args[label_start:]).strip()
        effective_peer_type = peer_type or infer_peer_type(peer_id)
        title: str | None = None
        username: str | None = None
        note: str | None = None
        if effective_peer_type == "channel":
            metadata, note = self._get_channel_metadata(peer_id)
            title = metadata["title"] or None
            username = metadata["username"] or None
            badge, revision = self._storage.set_channel_badge(
                channel_id=peer_id,
                badge_kind=badge_kind,
                label=label,
                actor=self._actor_name,
                title=title,
                username=username,
            )
        elif effective_peer_type == "user":
            badge, revision = self._storage.set_user_badge(
                user_id=peer_id,
                badge_kind=badge_kind,
                label=label,
                actor=self._actor_name,
            )
        else:
            badge, revision = self._storage.set_peer_badge(
                peer_id=peer_id,
                badge_kind=badge_kind,
                label=label,
                actor=self._actor_name,
                peer_type=effective_peer_type,
            )
        text = (
            f"badge set revision={revision}\n"
            f"{json.dumps(badge, ensure_ascii=False, indent=2)}"
        )
        if note:
            text += f"\nwarning: {note}"
        return BotCommandResult(
            True,
            text,
        )

    def _cmd_clear_badge(
        self,
        args: list[str],
        peer_type: str | None = None,
    ) -> BotCommandResult:
        if len(args) != 1:
            return BotCommandResult(False, f"usage: {self._badge_usage('clear', peer_type)}")
        peer_id = args[0]
        if peer_type == "user":
            removed, revision = self._storage.clear_user_badge(
                peer_id,
                actor=self._actor_name,
            )
        elif peer_type == "channel":
            removed, revision = self._storage.clear_channel_badge(
                peer_id,
                actor=self._actor_name,
            )
        else:
            removed, revision = self._storage.clear_peer_badge(
                peer_id,
                actor=self._actor_name,
            )
        return BotCommandResult(
            True,
            f"badge cleared={str(removed).lower()} revision={revision}",
        )

    def _cmd_show_badge(
        self,
        args: list[str],
        peer_type: str | None = None,
    ) -> BotCommandResult:
        if len(args) != 1:
            return BotCommandResult(False, f"usage: {self._badge_usage('show', peer_type)}")
        peer_id = args[0]
        if peer_type == "user":
            badge = self._storage.get_user_badge(peer_id)
        elif peer_type == "channel":
            badge = self._storage.get_channel_badge(peer_id)
        else:
            badge = self._storage.get_peer_badge(peer_id)
        return BotCommandResult(
            True,
            json.dumps(
                {
                    "peer_id": peer_id,
                    "found": badge is not None,
                    "badge": badge,
                },
                ensure_ascii=False,
                indent=2,
            ),
        )

    def _cmd_set_trusted_source(self, args: list[str]) -> BotCommandResult:
        if not args:
            return BotCommandResult(
                False,
                "usage: /set_trusted_source <channel_id> [label]",
            )
        channel_id = args[0]
        label = " ".join(args[1:]).strip()
        metadata, note = self._get_channel_metadata(channel_id)
        trusted, revision = self._storage.set_trusted_source(
            channel_id=channel_id,
            label=label,
            actor=self._actor_name,
            title=metadata["title"] or None,
            username=metadata["username"] or None,
        )
        text = (
            f"trusted source set revision={revision}\n"
            f"{json.dumps(trusted, ensure_ascii=False, indent=2)}"
        )
        if note:
            text += f"\nwarning: {note}"
        return BotCommandResult(
            True,
            text,
        )

    def _cmd_clear_trusted_source(self, args: list[str]) -> BotCommandResult:
        if len(args) != 1:
            return BotCommandResult(False, "usage: /clear_trusted_source <channel_id>")
        removed, revision = self._storage.clear_trusted_source(
            args[0],
            actor=self._actor_name,
        )
        return BotCommandResult(
            True,
            f"trusted source cleared={str(removed).lower()} revision={revision}",
        )

    def _cmd_show_trusted_source(self, args: list[str]) -> BotCommandResult:
        if len(args) != 1:
            return BotCommandResult(False, "usage: /show_trusted_source <channel_id>")
        trusted = self._storage.get_trusted_source(args[0])
        return BotCommandResult(
            True,
            json.dumps(
                {
                    "channel_id": args[0],
                    "found": trusted is not None,
                    "trusted_source": trusted,
                },
                ensure_ascii=False,
                indent=2,
            ),
        )

    def _cmd_set_plugin_record(self, args: list[str]) -> BotCommandResult:
        if len(args) < 3:
            return BotCommandResult(
                False,
                "usage: /set_plugin_record <sha256> <channel_id> <message_id> [label]",
            )
        sha256 = args[0].lower()
        if not _looks_like_sha256(sha256):
            return BotCommandResult(False, "sha256 must be a 64-char hex string")
        channel_id = args[1]
        try:
            message_id = int(args[2])
        except ValueError:
            return BotCommandResult(False, "message_id must be an integer")
        label = " ".join(args[3:]).strip()
        metadata, note = self._get_channel_metadata(channel_id)
        record, revision = self._storage.set_plugin_record(
            sha256=sha256,
            channel_id=channel_id,
            message_id=message_id,
            label=label,
            actor=self._actor_name,
            channel_title=metadata["title"] or None,
            channel_username=metadata["username"] or None,
        )
        text = (
            f"plugin record set revision={revision}\n"
            f"{json.dumps(record, ensure_ascii=False, indent=2)}"
        )
        if note:
            text += f"\nwarning: {note}"
        return BotCommandResult(
            True,
            text,
        )

    def _cmd_clear_plugin_record(self, args: list[str]) -> BotCommandResult:
        if len(args) != 1:
            return BotCommandResult(False, "usage: /clear_plugin_record <sha256>")
        sha256 = args[0].lower()
        if not _looks_like_sha256(sha256):
            return BotCommandResult(False, "sha256 must be a 64-char hex string")
        removed, revision = self._storage.clear_plugin_record(
            sha256,
            actor=self._actor_name,
        )
        return BotCommandResult(
            True,
            f"plugin record cleared={str(removed).lower()} revision={revision}",
        )

    def _cmd_show_plugin_record(self, args: list[str]) -> BotCommandResult:
        if len(args) != 1:
            return BotCommandResult(False, "usage: /show_plugin_record <sha256>")
        sha256 = args[0].lower()
        if not _looks_like_sha256(sha256):
            return BotCommandResult(False, "sha256 must be a 64-char hex string")
        record = self._storage.get_plugin_record(sha256)
        return BotCommandResult(
            True,
            json.dumps(
                {
                    "sha256": sha256,
                    "found": record is not None,
                    "plugin_record": record,
                },
                ensure_ascii=False,
                indent=2,
            ),
        )

    def _help_text(self) -> str:
        return "\n".join(
            [
                "Astrogram badge admin bot",
                "/ping",
                "/set_badge <peer_id> [kind] [label]",
                "/set_user_badge <user_id> [kind] [label]",
                "/set_channel_badge <channel_id> [kind] [label]",
                "/clear_badge <peer_id>",
                "/clear_user_badge <user_id>",
                "/clear_channel_badge <channel_id>",
                "/show_badge <peer_id>",
                "/show_user_badge <user_id>",
                "/show_channel_badge <channel_id>",
                "/set_trusted_source <channel_id> [label]",
                "/clear_trusted_source <channel_id>",
                "/show_trusted_source <channel_id>",
                "/set_plugin_record <sha256> <channel_id> <message_id> [label]",
                "/clear_plugin_record <sha256>",
                "/show_plugin_record <sha256>",
                "/snapshot",
            ]
        )

    def _telegram_command_specs(self) -> list[TelegramCommandSpec]:
        return [
            TelegramCommandSpec("ping", "Проверка работы"),
            TelegramCommandSpec("set_badge", "Выдать бейдж peer"),
            TelegramCommandSpec("set_user_badge", "Выдать бейдж пользователю"),
            TelegramCommandSpec("set_channel_badge", "Выдать бейдж каналу"),
            TelegramCommandSpec("clear_badge", "Снять бейдж peer"),
            TelegramCommandSpec("clear_user_badge", "Снять бейдж пользователя"),
            TelegramCommandSpec("clear_channel_badge", "Снять бейдж канала"),
            TelegramCommandSpec("show_badge", "Показать peer бейдж"),
            TelegramCommandSpec("show_user_badge", "Показать user бейдж"),
            TelegramCommandSpec("show_channel_badge", "Показать channel бейдж"),
            TelegramCommandSpec("set_trusted_source", "Выдать trusted source"),
            TelegramCommandSpec("clear_trusted_source", "Снять trusted source"),
            TelegramCommandSpec("show_trusted_source", "Показать trusted source"),
            TelegramCommandSpec("set_plugin_record", "Записать plugin record"),
            TelegramCommandSpec("clear_plugin_record", "Удалить plugin record"),
            TelegramCommandSpec("show_plugin_record", "Показать plugin record"),
            TelegramCommandSpec("snapshot", "Показать весь snapshot"),
        ]

    def _send_message(self, chat_id: int, text: str) -> None:
        for chunk in self._split_message(text):
            payload = {"chat_id": chat_id, "text": chunk}
            self._api("sendMessage", payload)

    def _split_message(self, text: str) -> list[str]:
        value = normalize_text(text)
        if not value:
            return []
        chunks: list[str] = []
        remaining = value
        while len(remaining) > MAX_TELEGRAM_MESSAGE_LENGTH:
            split_at = remaining.rfind("\n", 0, MAX_TELEGRAM_MESSAGE_LENGTH)
            if split_at <= 0:
                split_at = MAX_TELEGRAM_MESSAGE_LENGTH
            chunks.append(remaining[:split_at])
            remaining = remaining[split_at:].lstrip("\n")
        if remaining:
            chunks.append(remaining)
        return chunks

    def _badge_usage(self, action: str, peer_type: str | None) -> str:
        if action == "set":
            if peer_type == "user":
                return "/set_user_badge <user_id> [kind] [label]"
            if peer_type == "channel":
                return "/set_channel_badge <channel_id> [kind] [label]"
            return "/set_badge <peer_id> [kind] [label]"
        if action == "clear":
            if peer_type == "user":
                return "/clear_user_badge <user_id>"
            if peer_type == "channel":
                return "/clear_channel_badge <channel_id>"
            return "/clear_badge <peer_id>"
        if peer_type == "user":
            return "/show_user_badge <user_id>"
        if peer_type == "channel":
            return "/show_channel_badge <channel_id>"
        return "/show_badge <peer_id>"

    def _get_channel_metadata(
        self,
        channel_id: str,
    ) -> tuple[dict[str, str], str | None]:
        try:
            response = self._api("getChat", {"chat_id": channel_id})
        except Exception as exc:  # noqa: BLE001
            return {"title": "", "username": ""}, str(exc)
        chat = response.get("result") or {}
        return (
            {
                "title": normalize_text(chat.get("title")),
                "username": normalize_username(chat.get("username")),
            },
            None,
        )

    def _api(self, method: str, payload: dict[str, Any]) -> dict[str, Any]:
        encoded = parse.urlencode(payload).encode("utf-8")
        req = request.Request(
            f"{self._base_url}/{method}",
            data=encoded,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self._poll_timeout + 10) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except error.HTTPError as exc:  # noqa: PERF203
            body = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"telegram api http error {exc.code}: {body}") from exc
        if not data.get("ok"):
            raise RuntimeError(f"telegram api error: {data}")
        return data


def run_bot_forever(
    storage: AstrogramServerStorage,
    poll_timeout: int = 25,
) -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    bot = AstrogramBadgeBot.from_env(storage, poll_timeout=poll_timeout)
    bot.run_forever()
