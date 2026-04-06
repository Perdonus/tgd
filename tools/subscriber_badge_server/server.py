#!/usr/bin/env python3
import os
import sqlite3
from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse

DB_PATH = os.environ.get("ASTRO_BADGE_DB", "./badge.db")
DEFAULT_EMOJI_STATUS_ID = int(os.environ.get("ASTRO_BADGE_EMOJI_ID", "0"))

app = FastAPI(title="Astrogram Subscriber Badge API")


def db_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS subscribers (
            user_id INTEGER PRIMARY KEY,
            emoji_status_id INTEGER NOT NULL DEFAULT 0,
            updated_at INTEGER NOT NULL
        )
        """
    )
    conn.commit()
    return conn


@app.get("/api/astrogram/subscriber-badge")
def subscriber_badge(user_id: int = Query(..., gt=0)):
    conn = db_conn()
    try:
        row = conn.execute(
            "SELECT emoji_status_id FROM subscribers WHERE user_id = ?",
            (user_id,),
        ).fetchone()
    finally:
        conn.close()

    if not row:
        return JSONResponse({"badge": False, "emoji_status_id": 0})

    emoji_id = int(row[0]) if row[0] is not None else 0
    if emoji_id <= 0:
        emoji_id = DEFAULT_EMOJI_STATUS_ID
    return JSONResponse({"badge": True, "emoji_status_id": emoji_id})
