#!/usr/bin/env python3
import os
import sqlite3
from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse

DB_PATH = os.environ.get("ASTRO_BADGE_DB", "./badge.db")
DEFAULT_EMOJI_STATUS_ID = int(os.environ.get("ASTRO_BADGE_EMOJI_ID", "0"))
CHAT_TYPE_MASK = (1 << 48) - 1
BOT_API_CHANNEL_OFFSET = 10**12

app = FastAPI(title="Astrogram Subscriber Badge API")


def make_peer_id(shift: int, bare: int) -> int:
    if bare <= 0:
        raise ValueError("bare peer id must be positive")
    return int((shift << 48) | bare)


def normalize_peer_id(value: int) -> int:
    if value > CHAT_TYPE_MASK:
        return int(value)
    if value <= -BOT_API_CHANNEL_OFFSET:
        bare = abs(value) - BOT_API_CHANNEL_OFFSET
        return make_peer_id(2, bare)
    if value < 0:
        return make_peer_id(1, abs(value))
    return make_peer_id(0, value)


def ensure_schema(conn: sqlite3.Connection) -> None:
    columns = [
        row[1]
        for row in conn.execute("PRAGMA table_info(subscribers)").fetchall()
    ]
    if not columns:
        conn.execute(
            """
            CREATE TABLE subscribers (
                peer_id INTEGER PRIMARY KEY,
                emoji_status_id INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL
            )
            """
        )
        conn.commit()
        return
    if "peer_id" in columns:
        return
    if "user_id" in columns:
        conn.execute("ALTER TABLE subscribers RENAME TO subscribers_legacy")
        conn.execute(
            """
            CREATE TABLE subscribers (
                peer_id INTEGER PRIMARY KEY,
                emoji_status_id INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL
            )
            """
        )
        conn.execute(
            """
            INSERT INTO subscribers(peer_id, emoji_status_id, updated_at)
            SELECT user_id, emoji_status_id, updated_at
            FROM subscribers_legacy
            """
        )
        conn.execute("DROP TABLE subscribers_legacy")
        conn.commit()
        return
    raise RuntimeError("Unsupported subscribers schema")


def db_conn():
    conn = sqlite3.connect(DB_PATH)
    ensure_schema(conn)
    return conn


@app.get("/api/astrogram/subscriber-badge")
def subscriber_badge(
    peer_id: int | None = Query(None),
    user_id: int | None = Query(None, gt=0),
):
    lookup = peer_id if peer_id is not None else user_id
    if lookup is None:
        return JSONResponse({"badge": False, "emoji_status_id": 0}, status_code=400)
    normalized = normalize_peer_id(int(lookup))
    conn = db_conn()
    try:
        row = conn.execute(
            "SELECT emoji_status_id FROM subscribers WHERE peer_id = ?",
            (normalized,),
        ).fetchone()
    finally:
        conn.close()

    if not row:
        return JSONResponse({"badge": False, "emoji_status_id": 0})

    emoji_id = int(row[0]) if row[0] is not None else 0
    if emoji_id <= 0:
        emoji_id = DEFAULT_EMOJI_STATUS_ID
    return JSONResponse({"badge": True, "emoji_status_id": emoji_id})
