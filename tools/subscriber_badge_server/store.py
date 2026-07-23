#!/usr/bin/env python3
from __future__ import annotations

import sqlite3
import time
from pathlib import Path

from peer_ids import PeerRef, build_peer_ref, unpack_peer_id


def ensure_schema(conn: sqlite3.Connection) -> None:
    columns = {
        row[1]
        for row in conn.execute("PRAGMA table_info(subscribers)").fetchall()
    }
    if not columns:
        conn.execute(
            """
            CREATE TABLE subscribers (
                peer_id INTEGER PRIMARY KEY,
                peer_type TEXT NOT NULL DEFAULT 'user',
                bare_id INTEGER NOT NULL DEFAULT 0,
                emoji_status_id INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL
            )
            """
        )
        conn.commit()
        return
    if "user_id" in columns and "peer_id" not in columns:
        conn.execute("ALTER TABLE subscribers RENAME TO subscribers_legacy")
        conn.execute(
            """
            CREATE TABLE subscribers (
                peer_id INTEGER PRIMARY KEY,
                peer_type TEXT NOT NULL DEFAULT 'user',
                bare_id INTEGER NOT NULL DEFAULT 0,
                emoji_status_id INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL
            )
            """
        )
        rows = conn.execute(
            "SELECT user_id, emoji_status_id, updated_at FROM subscribers_legacy"
        ).fetchall()
        for user_id, emoji_status_id, updated_at in rows:
            ref = build_peer_ref("user", int(user_id))
            conn.execute(
                """
                INSERT INTO subscribers(peer_id, peer_type, bare_id, emoji_status_id, updated_at)
                VALUES (?, ?, ?, ?, ?)
                """,
                (ref.peer_id, ref.peer_type, ref.bare_id, int(emoji_status_id), int(updated_at)),
            )
        conn.execute("DROP TABLE subscribers_legacy")
        conn.commit()
        return
    if "peer_id" not in columns:
        raise RuntimeError("Unsupported subscribers schema")
    if "peer_type" not in columns:
        conn.execute(
            "ALTER TABLE subscribers ADD COLUMN peer_type TEXT NOT NULL DEFAULT 'user'"
        )
    if "bare_id" not in columns:
        conn.execute(
            "ALTER TABLE subscribers ADD COLUMN bare_id INTEGER NOT NULL DEFAULT 0"
        )
    conn.commit()
    _backfill_peer_meta(conn)


def _backfill_peer_meta(conn: sqlite3.Connection) -> None:
    rows = conn.execute(
        """
        SELECT peer_id, peer_type, bare_id
        FROM subscribers
        WHERE bare_id = 0 OR peer_type = '' OR peer_type IS NULL
        """
    ).fetchall()
    if not rows:
        return
    for peer_id, _, _ in rows:
        peer_type, bare_id = unpack_peer_id(int(peer_id))
        conn.execute(
            """
            UPDATE subscribers
            SET peer_type = ?, bare_id = ?
            WHERE peer_id = ?
            """,
            (peer_type, bare_id, int(peer_id)),
        )
    conn.commit()


def db_conn(path: str | Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    ensure_schema(conn)
    return conn


def upsert_badge(
    conn: sqlite3.Connection,
    peer: PeerRef,
    emoji_status_id: int,
    *,
    updated_at: int | None = None,
) -> None:
    stamp = int(time.time()) if updated_at is None else int(updated_at)
    conn.execute(
        """
        INSERT INTO subscribers(peer_id, peer_type, bare_id, emoji_status_id, updated_at)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(peer_id) DO UPDATE SET
            peer_type = excluded.peer_type,
            bare_id = excluded.bare_id,
            emoji_status_id = excluded.emoji_status_id,
            updated_at = excluded.updated_at
        """,
        (peer.peer_id, peer.peer_type, peer.bare_id, int(emoji_status_id), stamp),
    )
    conn.commit()


def delete_badge(conn: sqlite3.Connection, peer: PeerRef) -> None:
    conn.execute("DELETE FROM subscribers WHERE peer_id = ?", (peer.peer_id,))
    conn.commit()


def get_badge(conn: sqlite3.Connection, peer: PeerRef) -> sqlite3.Row | None:
    return conn.execute(
        """
        SELECT peer_id, peer_type, bare_id, emoji_status_id, updated_at
        FROM subscribers
        WHERE peer_id = ?
        """,
        (peer.peer_id,),
    ).fetchone()


def list_badges(conn: sqlite3.Connection, limit: int = 20) -> list[sqlite3.Row]:
    safe_limit = max(1, min(int(limit), 200))
    return list(
        conn.execute(
            """
            SELECT peer_id, peer_type, bare_id, emoji_status_id, updated_at
            FROM subscribers
            ORDER BY updated_at DESC, peer_id DESC
            LIMIT ?
            """,
            (safe_limit,),
        )
    )
