from __future__ import annotations

import json
import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


VALID_PEER_TYPES = {"user", "channel", "unknown"}


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def normalize_text(value: Any) -> str:
    return str(value or "").strip()


def normalize_username(value: Any) -> str:
    username = normalize_text(value)
    if username.startswith("@"):
        return username[1:]
    return username


def infer_peer_type(peer_id: str) -> str:
    peer_id = normalize_text(peer_id)
    if peer_id.startswith("-"):
        return "channel"
    return "user"


def load_json_dict(raw: str | None) -> dict[str, Any]:
    if not raw:
        return {}
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}


class AstrogramServerStorage:
    def __init__(self, state_dir: str | Path) -> None:
        self._state_dir = Path(state_dir).expanduser().resolve()
        self._state_dir.mkdir(parents=True, exist_ok=True)
        self._db_path = self._state_dir / "astrogram_server.sqlite3"
        self._bot_state_path = self._state_dir / "bot_state.json"
        self._condition = threading.Condition(threading.RLock())
        self._conn = sqlite3.connect(
            self._db_path,
            check_same_thread=False,
            isolation_level=None,
        )
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA foreign_keys=ON")
        self._init_schema()

    @property
    def state_dir(self) -> Path:
        return self._state_dir

    @property
    def db_path(self) -> Path:
        return self._db_path

    @property
    def bot_state_path(self) -> Path:
        return self._bot_state_path

    def close(self) -> None:
        with self._condition:
            self._conn.close()

    def _init_schema(self) -> None:
        with self._condition:
            self._conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS meta (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS peer_badges (
                    peer_id TEXT PRIMARY KEY,
                    badge_kind TEXT NOT NULL,
                    label TEXT NOT NULL DEFAULT '',
                    payload_json TEXT NOT NULL DEFAULT '{}',
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS trusted_sources (
                    channel_id TEXT PRIMARY KEY,
                    label TEXT NOT NULL DEFAULT '',
                    payload_json TEXT NOT NULL DEFAULT '{}',
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS plugin_records (
                    sha256 TEXT PRIMARY KEY,
                    channel_id TEXT NOT NULL,
                    message_id INTEGER NOT NULL,
                    label TEXT NOT NULL DEFAULT '',
                    payload_json TEXT NOT NULL DEFAULT '{}',
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS audit_log (
                    revision INTEGER PRIMARY KEY,
                    actor TEXT NOT NULL,
                    action TEXT NOT NULL,
                    subject_type TEXT NOT NULL,
                    subject_key TEXT NOT NULL,
                    payload_json TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL
                );
                """
            )
            self._ensure_meta_key("schema_version", "2")
            self._ensure_meta_key("revision", "0")
            self._conn.execute(
                "INSERT OR REPLACE INTO meta(key, value) VALUES ('schema_version', '2')"
            )

    def _ensure_meta_key(self, key: str, value: str) -> None:
        row = self._conn.execute(
            "SELECT value FROM meta WHERE key = ?",
            (key,),
        ).fetchone()
        if row is None:
            self._conn.execute(
                "INSERT INTO meta(key, value) VALUES (?, ?)",
                (key, value),
            )

    def _read_revision_locked(self) -> int:
        row = self._conn.execute(
            "SELECT value FROM meta WHERE key = 'revision'"
        ).fetchone()
        return int(row["value"]) if row else 0

    def get_revision(self) -> int:
        with self._condition:
            return self._read_revision_locked()

    def _next_revision_locked(self) -> int:
        revision = self._read_revision_locked() + 1
        self._conn.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES ('revision', ?)",
            (str(revision),),
        )
        return revision

    def _append_audit_locked(
        self,
        revision: int,
        actor: str,
        action: str,
        subject_type: str,
        subject_key: str,
        payload: dict[str, Any],
    ) -> None:
        self._conn.execute(
            """
            INSERT INTO audit_log(
                revision, actor, action, subject_type, subject_key, payload_json, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                revision,
                actor,
                action,
                subject_type,
                subject_key,
                json.dumps(payload, ensure_ascii=False, sort_keys=True),
                utc_now_iso(),
            ),
        )

    def _normalize_peer_type(self, peer_id: str, peer_type: str | None) -> str:
        candidate = normalize_text(peer_type).lower()
        if candidate in VALID_PEER_TYPES:
            return candidate
        return infer_peer_type(peer_id)

    def _build_peer_object(
        self,
        peer_id: str,
        peer_type: str,
        title: str = "",
        username: str = "",
    ) -> dict[str, Any]:
        return {
            "id": normalize_text(peer_id),
            "type": self._normalize_peer_type(peer_id, peer_type),
            "title": normalize_text(title),
            "username": normalize_username(username),
        }

    def _merge_peer_payload(
        self,
        peer_id: str,
        payload: dict[str, Any] | None = None,
        *,
        peer_type: str | None = None,
        title: str | None = None,
        username: str | None = None,
    ) -> dict[str, Any]:
        merged = dict(payload or {})
        merged["peer_type"] = self._normalize_peer_type(
            peer_id,
            peer_type or merged.get("peer_type"),
        )
        if title is not None:
            title_value = normalize_text(title)
            if title_value:
                merged["title"] = title_value
            else:
                merged.pop("title", None)
        elif merged.get("title") is not None:
            existing_title = normalize_text(merged.get("title"))
            if existing_title:
                merged["title"] = existing_title
            else:
                merged.pop("title", None)
        if username is not None:
            username_value = normalize_username(username)
            if username_value:
                merged["username"] = username_value
            else:
                merged.pop("username", None)
        elif merged.get("username") is not None:
            existing_username = normalize_username(merged.get("username"))
            if existing_username:
                merged["username"] = existing_username
            else:
                merged.pop("username", None)
        return merged

    def _merge_channel_payload(
        self,
        payload: dict[str, Any] | None = None,
        *,
        title: str | None = None,
        username: str | None = None,
    ) -> dict[str, Any]:
        merged = dict(payload or {})
        if title is not None:
            title_value = normalize_text(title)
            if title_value:
                merged["title"] = title_value
            else:
                merged.pop("title", None)
        elif merged.get("title") is not None:
            existing_title = normalize_text(merged.get("title"))
            if existing_title:
                merged["title"] = existing_title
            else:
                merged.pop("title", None)
        if username is not None:
            username_value = normalize_username(username)
            if username_value:
                merged["username"] = username_value
            else:
                merged.pop("username", None)
        elif merged.get("username") is not None:
            existing_username = normalize_username(merged.get("username"))
            if existing_username:
                merged["username"] = existing_username
            else:
                merged.pop("username", None)
        return merged

    def _merge_plugin_payload(
        self,
        payload: dict[str, Any] | None = None,
        *,
        channel_title: str | None = None,
        channel_username: str | None = None,
    ) -> dict[str, Any]:
        merged = dict(payload or {})
        if channel_title is not None:
            title_value = normalize_text(channel_title)
            if title_value:
                merged["channel_title"] = title_value
            else:
                merged.pop("channel_title", None)
        elif merged.get("channel_title") is not None:
            existing_title = normalize_text(merged.get("channel_title"))
            if existing_title:
                merged["channel_title"] = existing_title
            else:
                merged.pop("channel_title", None)
        if channel_username is not None:
            username_value = normalize_username(channel_username)
            if username_value:
                merged["channel_username"] = username_value
            else:
                merged.pop("channel_username", None)
        elif merged.get("channel_username") is not None:
            existing_username = normalize_username(merged.get("channel_username"))
            if existing_username:
                merged["channel_username"] = existing_username
            else:
                merged.pop("channel_username", None)
        return merged

    def _normalize_peer_row(self, row: sqlite3.Row | None) -> dict[str, Any] | None:
        if row is None:
            return None
        payload = load_json_dict(row["payload_json"])
        peer_type = self._normalize_peer_type(row["peer_id"], payload.get("peer_type"))
        title = normalize_text(payload.get("title"))
        username = normalize_username(payload.get("username"))
        result = {
            "peer_id": row["peer_id"],
            "peer_type": peer_type,
            "badge_kind": row["badge_kind"],
            "kind": row["badge_kind"],
            "label": row["label"],
            "title": title,
            "username": username,
            "peer": self._build_peer_object(row["peer_id"], peer_type, title, username),
            "payload": payload,
            "updated_at": row["updated_at"],
        }
        for key, value in payload.items():
            result.setdefault(key, value)
        if row["badge_kind"] in {"server", "verified", "staff", "partner", "custom"}:
            result.setdefault("verified", True)
            result.setdefault("trusted", True)
            result.setdefault("confirmed", True)
        elif row["badge_kind"] == "premium":
            result.setdefault("premium", True)
        return result

    def _normalize_trusted_row(self, row: sqlite3.Row | None) -> dict[str, Any] | None:
        if row is None:
            return None
        payload = load_json_dict(row["payload_json"])
        title = normalize_text(payload.get("title")) or normalize_text(row["label"])
        username = normalize_username(payload.get("username"))
        result = {
            "channel_id": row["channel_id"],
            "label": row["label"],
            "title": title,
            "name": title,
            "username": username,
            "channel_title": title,
            "channelTitle": title,
            "channel_username": username,
            "channelUsername": username,
            "verified": True,
            "trusted": True,
            "confirmed": True,
            "peer": self._build_peer_object(row["channel_id"], "channel", title, username),
            "payload": payload,
            "updated_at": row["updated_at"],
        }
        for key, value in payload.items():
            result.setdefault(key, value)
        return result

    def _normalize_plugin_row(self, row: sqlite3.Row | None) -> dict[str, Any] | None:
        if row is None:
            return None
        payload = load_json_dict(row["payload_json"])
        trusted = self.get_trusted_source(row["channel_id"])
        channel_title = normalize_text(payload.get("channel_title"))
        channel_username = normalize_username(payload.get("channel_username"))
        if trusted is not None:
            channel_title = trusted["title"]
            channel_username = trusted["username"]
        result = {
            "sha256": row["sha256"],
            "channel_id": row["channel_id"],
            "message_id": row["message_id"],
            "label": row["label"],
            "channel_title": channel_title,
            "channel_username": channel_username,
            "source_peer": self._build_peer_object(
                row["channel_id"],
                "channel",
                channel_title,
                channel_username,
            ),
            "payload": payload,
            "trusted_source": trusted,
            "updated_at": row["updated_at"],
        }
        for key, value in payload.items():
            result.setdefault(key, value)
        return result

    def wait_for_revision_change(self, since_revision: int, timeout: float) -> int:
        with self._condition:
            current = self._read_revision_locked()
            if current > since_revision:
                return current
            self._condition.wait(timeout=max(0.0, timeout))
            return self._read_revision_locked()

    def get_peer_badge(self, peer_id: str) -> dict[str, Any] | None:
        with self._condition:
            row = self._conn.execute(
                "SELECT * FROM peer_badges WHERE peer_id = ?",
                (str(peer_id),),
            ).fetchone()
            return self._normalize_peer_row(row)

    def set_peer_badge(
        self,
        peer_id: str,
        badge_kind: str,
        label: str = "",
        payload: dict[str, Any] | None = None,
        actor: str = "cli",
        peer_type: str | None = None,
        title: str | None = None,
        username: str | None = None,
    ) -> tuple[dict[str, Any], int]:
        now = utc_now_iso()
        peer_id = str(peer_id)
        payload = self._merge_peer_payload(
            peer_id,
            payload,
            peer_type=peer_type,
            title=title,
            username=username,
        )
        with self._condition:
            self._conn.execute("BEGIN IMMEDIATE")
            self._conn.execute(
                """
                INSERT INTO peer_badges(peer_id, badge_kind, label, payload_json, updated_at)
                VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(peer_id) DO UPDATE SET
                    badge_kind = excluded.badge_kind,
                    label = excluded.label,
                    payload_json = excluded.payload_json,
                    updated_at = excluded.updated_at
                """,
                (
                    peer_id,
                    badge_kind,
                    label,
                    json.dumps(payload, ensure_ascii=False, sort_keys=True),
                    now,
                ),
            )
            revision = self._next_revision_locked()
            self._append_audit_locked(
                revision,
                actor,
                "set",
                "peer_badge",
                peer_id,
                {
                    "peer_id": peer_id,
                    "badge_kind": badge_kind,
                    "label": label,
                    "payload": payload,
                },
            )
            self._conn.commit()
            self._condition.notify_all()
            return self.get_peer_badge(peer_id) or {}, revision

    def get_typed_peer_badge(
        self,
        peer_id: str,
        peer_type: str,
    ) -> dict[str, Any] | None:
        badge = self.get_peer_badge(peer_id)
        if badge is None:
            return None
        expected_type = self._normalize_peer_type(peer_id, peer_type)
        if badge["peer_type"] != expected_type:
            return None
        return badge

    def set_user_badge(
        self,
        user_id: str,
        badge_kind: str,
        label: str = "",
        payload: dict[str, Any] | None = None,
        actor: str = "cli",
        title: str | None = None,
        username: str | None = None,
    ) -> tuple[dict[str, Any], int]:
        return self.set_peer_badge(
            peer_id=str(user_id),
            badge_kind=badge_kind,
            label=label,
            payload=payload,
            actor=actor,
            peer_type="user",
            title=title,
            username=username,
        )

    def set_channel_badge(
        self,
        channel_id: str,
        badge_kind: str,
        label: str = "",
        payload: dict[str, Any] | None = None,
        actor: str = "cli",
        title: str | None = None,
        username: str | None = None,
    ) -> tuple[dict[str, Any], int]:
        return self.set_peer_badge(
            peer_id=str(channel_id),
            badge_kind=badge_kind,
            label=label,
            payload=payload,
            actor=actor,
            peer_type="channel",
            title=title,
            username=username,
        )

    def clear_peer_badge(self, peer_id: str, actor: str = "cli") -> tuple[bool, int]:
        peer_id = str(peer_id)
        with self._condition:
            row = self._conn.execute(
                "SELECT 1 FROM peer_badges WHERE peer_id = ?",
                (peer_id,),
            ).fetchone()
            if row is None:
                return False, self._read_revision_locked()
            self._conn.execute("BEGIN IMMEDIATE")
            self._conn.execute(
                "DELETE FROM peer_badges WHERE peer_id = ?",
                (peer_id,),
            )
            revision = self._next_revision_locked()
            self._append_audit_locked(
                revision,
                actor,
                "clear",
                "peer_badge",
                peer_id,
                {"peer_id": peer_id},
            )
            self._conn.commit()
            self._condition.notify_all()
            return True, revision

    def clear_typed_peer_badge(
        self,
        peer_id: str,
        peer_type: str,
        actor: str = "cli",
    ) -> tuple[bool, int]:
        badge = self.get_typed_peer_badge(peer_id, peer_type)
        if badge is None:
            return False, self.get_revision()
        return self.clear_peer_badge(peer_id, actor=actor)

    def clear_user_badge(self, user_id: str, actor: str = "cli") -> tuple[bool, int]:
        return self.clear_typed_peer_badge(user_id, "user", actor=actor)

    def clear_channel_badge(
        self,
        channel_id: str,
        actor: str = "cli",
    ) -> tuple[bool, int]:
        return self.clear_typed_peer_badge(channel_id, "channel", actor=actor)

    def get_user_badge(self, user_id: str) -> dict[str, Any] | None:
        return self.get_typed_peer_badge(user_id, "user")

    def get_channel_badge(self, channel_id: str) -> dict[str, Any] | None:
        return self.get_typed_peer_badge(channel_id, "channel")

    def get_trusted_source(self, channel_id: str) -> dict[str, Any] | None:
        with self._condition:
            row = self._conn.execute(
                "SELECT * FROM trusted_sources WHERE channel_id = ?",
                (str(channel_id),),
            ).fetchone()
            return self._normalize_trusted_row(row)

    def set_trusted_source(
        self,
        channel_id: str,
        label: str = "",
        payload: dict[str, Any] | None = None,
        actor: str = "cli",
        title: str | None = None,
        username: str | None = None,
    ) -> tuple[dict[str, Any], int]:
        channel_id = str(channel_id)
        now = utc_now_iso()
        payload = self._merge_channel_payload(
            payload,
            title=title,
            username=username,
        )
        with self._condition:
            self._conn.execute("BEGIN IMMEDIATE")
            self._conn.execute(
                """
                INSERT INTO trusted_sources(channel_id, label, payload_json, updated_at)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(channel_id) DO UPDATE SET
                    label = excluded.label,
                    payload_json = excluded.payload_json,
                    updated_at = excluded.updated_at
                """,
                (
                    channel_id,
                    label,
                    json.dumps(payload, ensure_ascii=False, sort_keys=True),
                    now,
                ),
            )
            revision = self._next_revision_locked()
            self._append_audit_locked(
                revision,
                actor,
                "set",
                "trusted_source",
                channel_id,
                {
                    "channel_id": channel_id,
                    "label": label,
                    "payload": payload,
                },
            )
            self._conn.commit()
            self._condition.notify_all()
            return self.get_trusted_source(channel_id) or {}, revision

    def clear_trusted_source(
        self,
        channel_id: str,
        actor: str = "cli",
    ) -> tuple[bool, int]:
        channel_id = str(channel_id)
        with self._condition:
            row = self._conn.execute(
                "SELECT 1 FROM trusted_sources WHERE channel_id = ?",
                (channel_id,),
            ).fetchone()
            if row is None:
                return False, self._read_revision_locked()
            self._conn.execute("BEGIN IMMEDIATE")
            self._conn.execute(
                "DELETE FROM trusted_sources WHERE channel_id = ?",
                (channel_id,),
            )
            revision = self._next_revision_locked()
            self._append_audit_locked(
                revision,
                actor,
                "clear",
                "trusted_source",
                channel_id,
                {"channel_id": channel_id},
            )
            self._conn.commit()
            self._condition.notify_all()
            return True, revision

    def get_plugin_record(self, sha256: str) -> dict[str, Any] | None:
        with self._condition:
            row = self._conn.execute(
                "SELECT * FROM plugin_records WHERE sha256 = ?",
                (sha256.lower(),),
            ).fetchone()
            return self._normalize_plugin_row(row)

    def set_plugin_record(
        self,
        sha256: str,
        channel_id: str,
        message_id: int,
        label: str = "",
        payload: dict[str, Any] | None = None,
        actor: str = "cli",
        channel_title: str | None = None,
        channel_username: str | None = None,
    ) -> tuple[dict[str, Any], int]:
        sha256 = sha256.lower()
        channel_id = str(channel_id)
        now = utc_now_iso()
        payload = self._merge_plugin_payload(
            payload,
            channel_title=channel_title,
            channel_username=channel_username,
        )
        with self._condition:
            self._conn.execute("BEGIN IMMEDIATE")
            self._conn.execute(
                """
                INSERT INTO plugin_records(
                    sha256, channel_id, message_id, label, payload_json, updated_at
                )
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(sha256) DO UPDATE SET
                    channel_id = excluded.channel_id,
                    message_id = excluded.message_id,
                    label = excluded.label,
                    payload_json = excluded.payload_json,
                    updated_at = excluded.updated_at
                """,
                (
                    sha256,
                    channel_id,
                    int(message_id),
                    label,
                    json.dumps(payload, ensure_ascii=False, sort_keys=True),
                    now,
                ),
            )
            revision = self._next_revision_locked()
            self._append_audit_locked(
                revision,
                actor,
                "set",
                "plugin_record",
                sha256,
                {
                    "sha256": sha256,
                    "channel_id": channel_id,
                    "message_id": int(message_id),
                    "label": label,
                    "payload": payload,
                },
            )
            self._conn.commit()
            self._condition.notify_all()
            return self.get_plugin_record(sha256) or {}, revision

    def clear_plugin_record(self, sha256: str, actor: str = "cli") -> tuple[bool, int]:
        sha256 = sha256.lower()
        with self._condition:
            row = self._conn.execute(
                "SELECT 1 FROM plugin_records WHERE sha256 = ?",
                (sha256,),
            ).fetchone()
            if row is None:
                return False, self._read_revision_locked()
            self._conn.execute("BEGIN IMMEDIATE")
            self._conn.execute(
                "DELETE FROM plugin_records WHERE sha256 = ?",
                (sha256,),
            )
            revision = self._next_revision_locked()
            self._append_audit_locked(
                revision,
                actor,
                "clear",
                "plugin_record",
                sha256,
                {"sha256": sha256},
            )
            self._conn.commit()
            self._condition.notify_all()
            return True, revision

    def export_snapshot(self) -> dict[str, Any]:
        with self._condition:
            peer_badges = [
                self._normalize_peer_row(row)
                for row in self._conn.execute(
                    "SELECT * FROM peer_badges ORDER BY peer_id"
                ).fetchall()
            ]
            trusted_sources = [
                self._normalize_trusted_row(row)
                for row in self._conn.execute(
                    "SELECT * FROM trusted_sources ORDER BY channel_id"
                ).fetchall()
            ]
            plugin_records = [
                self._normalize_plugin_row(row)
                for row in self._conn.execute(
                    "SELECT * FROM plugin_records ORDER BY sha256"
                ).fetchall()
            ]
            return {
                "revision": self._read_revision_locked(),
                "peer_badges": peer_badges,
                "trusted_sources": trusted_sources,
                "plugin_records": plugin_records,
            }

    def get_changes_since(self, since_revision: int) -> dict[str, Any]:
        with self._condition:
            rows = self._conn.execute(
                """
                SELECT revision, subject_type, subject_key, action
                FROM audit_log
                WHERE revision > ?
                ORDER BY revision
                """,
                (int(since_revision),),
            ).fetchall()
            changes = {
                "peer_badges": [],
                "trusted_sources": [],
                "plugin_records": [],
            }
            seen: set[tuple[str, str]] = set()
            for row in rows:
                key = (row["subject_type"], row["subject_key"])
                if key in seen:
                    continue
                seen.add(key)
                subject_type = row["subject_type"]
                subject_key = row["subject_key"]
                if subject_type == "peer_badge":
                    current = self._conn.execute(
                        "SELECT * FROM peer_badges WHERE peer_id = ?",
                        (subject_key,),
                    ).fetchone()
                    changes["peer_badges"].append(
                        {
                            "peer_id": subject_key,
                            "present": current is not None,
                            "value": self._normalize_peer_row(current),
                        }
                    )
                elif subject_type == "trusted_source":
                    current = self._conn.execute(
                        "SELECT * FROM trusted_sources WHERE channel_id = ?",
                        (subject_key,),
                    ).fetchone()
                    changes["trusted_sources"].append(
                        {
                            "channel_id": subject_key,
                            "present": current is not None,
                            "value": self._normalize_trusted_row(current),
                        }
                    )
                elif subject_type == "plugin_record":
                    current = self._conn.execute(
                        "SELECT * FROM plugin_records WHERE sha256 = ?",
                        (subject_key,),
                    ).fetchone()
                    changes["plugin_records"].append(
                        {
                            "sha256": subject_key,
                            "present": current is not None,
                            "value": self._normalize_plugin_row(current),
                        }
                    )
            return {
                "revision": self._read_revision_locked(),
                "changes": changes,
            }

    def get_counts(self) -> dict[str, int]:
        with self._condition:
            return {
                "peer_badges": int(
                    self._conn.execute("SELECT COUNT(*) FROM peer_badges").fetchone()[0]
                ),
                "trusted_sources": int(
                    self._conn.execute("SELECT COUNT(*) FROM trusted_sources").fetchone()[0]
                ),
                "plugin_records": int(
                    self._conn.execute("SELECT COUNT(*) FROM plugin_records").fetchone()[0]
                ),
            }

    def load_bot_state(self) -> dict[str, Any]:
        if not self._bot_state_path.exists():
            return {"offset": 0}
        try:
            return json.loads(self._bot_state_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {"offset": 0}

    def save_bot_state(self, state: dict[str, Any]) -> None:
        encoded = json.dumps(state, ensure_ascii=False, indent=2, sort_keys=True)
        self._bot_state_path.write_text(encoded + "\n", encoding="utf-8")
