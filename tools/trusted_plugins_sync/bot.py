#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import tempfile
import time
from pathlib import Path

from telegram import Update
from telegram.error import TelegramError
from telegram.ext import Application, CommandHandler, ContextTypes, MessageHandler, filters


DEFAULT_TOKEN_FILE = Path.home() / ".config" / "astrogram" / "trusted_plugins_bot_token"
DEFAULT_STATE_DIR = Path.home() / ".local" / "state" / "astrogram"
DEFAULT_DB_PATH = DEFAULT_STATE_DIR / "trusted_plugins.db"
DEFAULT_EXPORT_PATH = DEFAULT_STATE_DIR / "trusted_plugins.export.json"


def env_first(*names: str) -> str:
    for name in names:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return ""


def load_secret(env_names: tuple[str, ...], default_file: Path) -> str:
    value = env_first(*env_names)
    if value:
        return value
    file_value = env_first(*(f"{name}_FILE" for name in env_names))
    if file_value:
        path = Path(file_value).expanduser()
        if path.is_file():
            return path.read_text(encoding="utf-8").strip()
    if default_file.is_file():
        return default_file.read_text(encoding="utf-8").strip()
    return ""


def parse_channel_ids(raw: str) -> set[int]:
    result: set[int] = set()
    for part in raw.replace(";", ",").split(","):
        part = part.strip()
        if not part:
            continue
        result.add(int(part))
    return result


def parse_label_map(raw: str) -> dict[int, str]:
    result: dict[int, str] = {}
    for chunk in raw.replace(";", ",").split(","):
        chunk = chunk.strip()
        if not chunk or "=" not in chunk:
            continue
        left, right = chunk.split("=", 1)
        left = left.strip()
        right = right.strip()
        if not left or not right:
            continue
        result[int(left)] = right
    return result


def atomic_write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w",
        encoding="utf-8",
        dir=str(path.parent),
        delete=False,
        suffix=".tmp",
    ) as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
        tmp_path = Path(handle.name)
    os.replace(tmp_path, path)


class TrustedPluginsStore:
    def __init__(self, db_path: Path) -> None:
        self._db_path = db_path
        self._db_path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(self._db_path)
        self._conn.row_factory = sqlite3.Row
        self._ensure_schema()

    def close(self) -> None:
        self._conn.close()

    def _ensure_schema(self) -> None:
        self._conn.execute(
            """
            CREATE TABLE IF NOT EXISTS trusted_plugins (
                channel_id INTEGER NOT NULL,
                message_id INTEGER NOT NULL,
                chat_title TEXT NOT NULL DEFAULT '',
                chat_username TEXT NOT NULL DEFAULT '',
                label TEXT NOT NULL DEFAULT '',
                file_id TEXT NOT NULL DEFAULT '',
                file_unique_id TEXT NOT NULL DEFAULT '',
                filename TEXT NOT NULL DEFAULT '',
                sha256 TEXT NOT NULL,
                first_seen_at INTEGER NOT NULL,
                last_seen_at INTEGER NOT NULL,
                PRIMARY KEY (channel_id, message_id)
            )
            """
        )
        self._conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_trusted_plugins_sha256
            ON trusted_plugins (sha256)
            """
        )
        self._conn.commit()

    def upsert(
        self,
        *,
        channel_id: int,
        message_id: int,
        chat_title: str,
        chat_username: str,
        label: str,
        file_id: str,
        file_unique_id: str,
        filename: str,
        sha256: str,
    ) -> None:
        now = int(time.time())
        self._conn.execute(
            """
            INSERT INTO trusted_plugins (
                channel_id,
                message_id,
                chat_title,
                chat_username,
                label,
                file_id,
                file_unique_id,
                filename,
                sha256,
                first_seen_at,
                last_seen_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(channel_id, message_id) DO UPDATE SET
                chat_title = excluded.chat_title,
                chat_username = excluded.chat_username,
                label = excluded.label,
                file_id = excluded.file_id,
                file_unique_id = excluded.file_unique_id,
                filename = excluded.filename,
                sha256 = excluded.sha256,
                last_seen_at = excluded.last_seen_at
            """,
            (
                channel_id,
                message_id,
                chat_title,
                chat_username,
                label,
                file_id,
                file_unique_id,
                filename,
                sha256,
                now,
                now,
            ),
        )
        self._conn.commit()

    def rows(self) -> list[sqlite3.Row]:
        return list(
            self._conn.execute(
                """
                SELECT
                    channel_id,
                    message_id,
                    chat_title,
                    chat_username,
                    label,
                    filename,
                    sha256,
                    first_seen_at,
                    last_seen_at
                FROM trusted_plugins
                ORDER BY channel_id, message_id
                """
            )
        )

    def unique_hashes_count(self) -> int:
        row = self._conn.execute(
            "SELECT COUNT(DISTINCT sha256) AS c FROM trusted_plugins"
        ).fetchone()
        return int(row["c"]) if row else 0


class TrustedPluginsBot:
    def __init__(
        self,
        *,
        store: TrustedPluginsStore,
        export_path: Path,
        allowed_channel_ids: set[int],
        label_map: dict[int, str],
        admin_ids: set[int],
    ) -> None:
        self._store = store
        self._export_path = export_path
        self._allowed_channel_ids = allowed_channel_ids
        self._label_map = label_map
        self._admin_ids = admin_ids

    def export_payload(self) -> dict:
        rows = self._store.rows()
        known_channel_ids = {
            int(row["channel_id"])
            for row in rows
        } | self._allowed_channel_ids | set(self._label_map.keys())
        records = []
        for row in rows:
            label = (
                str(row["label"]).strip()
                or str(row["chat_title"]).strip()
                or self._label_map.get(int(row["channel_id"]), "").strip()
                or "Trusted source"
            )
            records.append(
                f"{row['sha256']}|{int(row['channel_id'])}|{int(row['message_id'])}|{label}"
            )
        return {
            "astrogram_trusted_plugin_channel_ids": sorted(known_channel_ids),
            "astrogram_trusted_plugin_records": records,
        }

    def write_export(self) -> None:
        atomic_write_json(self._export_path, self.export_payload())

    def _channel_allowed(self, channel_id: int) -> bool:
        return not self._allowed_channel_ids or (channel_id in self._allowed_channel_ids)

    def _channel_label(self, channel_id: int, chat_title: str) -> str:
        explicit = self._label_map.get(channel_id, "").strip()
        if explicit:
            return explicit
        if chat_title.strip():
            return chat_title.strip()
        return "Trusted source"

    async def process_channel_post(
        self,
        update: Update,
        context: ContextTypes.DEFAULT_TYPE,
    ) -> None:
        message = update.effective_message
        if not message or not message.chat:
            return
        if message.chat.type != "channel":
            return
        document = message.document
        if not document:
            return
        filename = (document.file_name or "").strip()
        if not filename.lower().endswith(".tgd"):
            return
        channel_id = int(message.chat.id)
        if not self._channel_allowed(channel_id):
            return

        file = await context.bot.get_file(document.file_id)
        data = bytes(await file.download_as_bytearray())
        sha256 = hashlib.sha256(data).hexdigest()
        chat_title = (message.chat.title or "").strip()
        chat_username = (message.chat.username or "").strip()
        self._store.upsert(
            channel_id=channel_id,
            message_id=int(message.message_id),
            chat_title=chat_title,
            chat_username=chat_username,
            label=self._channel_label(channel_id, chat_title),
            file_id=document.file_id,
            file_unique_id=document.file_unique_id,
            filename=filename,
            sha256=sha256,
        )
        self.write_export()

    def _is_admin(self, update: Update) -> bool:
        user = update.effective_user
        return bool(user and self._admin_ids and user.id in self._admin_ids)

    async def export_command(
        self,
        update: Update,
        context: ContextTypes.DEFAULT_TYPE,
    ) -> None:
        if not self._is_admin(update):
            return
        self.write_export()
        payload = self.export_payload()
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        await update.effective_message.reply_text(text[:4000])

    async def stats_command(
        self,
        update: Update,
        context: ContextTypes.DEFAULT_TYPE,
    ) -> None:
        if not self._is_admin(update):
            return
        rows = self._store.rows()
        text = (
            f"Tracked posts: {len(rows)}\n"
            f"Unique hashes: {self._store.unique_hashes_count()}\n"
            f"Export: {self._export_path}"
        )
        await update.effective_message.reply_text(text)


def build_application(bot: TrustedPluginsBot, token: str) -> Application:
    app = Application.builder().token(token).build()
    app.add_handler(CommandHandler("export", bot.export_command))
    app.add_handler(CommandHandler("stats", bot.stats_command))
    app.add_handler(MessageHandler(filters.ALL, bot.process_channel_post))
    return app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Listen to a trusted Telegram checker channel, hash every .tgd file, "
            "and export astrogram_trusted_plugin_records JSON."
        )
    )
    parser.add_argument(
        "--export-only",
        action="store_true",
        help="Rewrite the export JSON from the existing SQLite database and exit.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    token = load_secret(
        ("ASTROGRAM_TRUST_BOT_TOKEN", "ASTRO_TRUST_BOT_TOKEN"),
        DEFAULT_TOKEN_FILE,
    )
    db_path = Path(
        env_first("ASTROGRAM_TRUST_DB", "ASTRO_TRUST_DB") or str(DEFAULT_DB_PATH)
    ).expanduser()
    export_path = Path(
        env_first("ASTROGRAM_TRUST_OUTPUT", "ASTRO_TRUST_OUTPUT")
        or str(DEFAULT_EXPORT_PATH)
    ).expanduser()
    allowed_channel_ids = parse_channel_ids(
        env_first("ASTROGRAM_TRUST_CHANNEL_IDS", "ASTRO_TRUST_CHANNEL_IDS")
    )
    label_map = parse_label_map(
        env_first("ASTROGRAM_TRUST_CHANNEL_LABELS", "ASTRO_TRUST_CHANNEL_LABELS")
    )
    admin_ids = parse_channel_ids(
        env_first("ASTROGRAM_TRUST_ADMIN_IDS", "ASTRO_TRUST_ADMIN_IDS")
    )

    store = TrustedPluginsStore(db_path)
    try:
        bot = TrustedPluginsBot(
            store=store,
            export_path=export_path,
            allowed_channel_ids=allowed_channel_ids,
            label_map=label_map,
            admin_ids=admin_ids,
        )
        bot.write_export()
        if args.export_only:
            return
        if not token:
            raise RuntimeError(
                "Set ASTROGRAM_TRUST_BOT_TOKEN / ASTRO_TRUST_BOT_TOKEN or place the token in "
                f"{DEFAULT_TOKEN_FILE}"
            )
        app = build_application(bot, token)
        app.run_polling(allowed_updates=Update.ALL_TYPES)
    finally:
        store.close()


if __name__ == "__main__":
    main()
