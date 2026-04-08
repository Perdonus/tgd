#!/usr/bin/env python3
from __future__ import annotations

import os

from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse

from peer_ids import describe_peer_ref, parse_peer_ref
from store import db_conn


DB_PATH = os.environ.get("ASTRO_BADGE_DB", "./badge.db")
DEFAULT_EMOJI_STATUS_ID = int(os.environ.get("ASTRO_BADGE_EMOJI_ID", "0"))

app = FastAPI(title="Astrogram Subscriber Badge API")


def resolve_lookup(
    *,
    peer_ref: str | None,
    peer_id: str | None,
    user_id: int | None,
    peer_type: str | None,
    bare_id: int | None,
):
    if peer_ref:
        return parse_peer_ref(peer_ref)
    if peer_type and bare_id is not None:
        return parse_peer_ref(f"{peer_type}:{bare_id}")
    if peer_id:
        return parse_peer_ref(peer_id)
    if user_id is not None:
        return parse_peer_ref(f"user:{int(user_id)}")
    return None


@app.get("/api/astrogram/subscriber-badge")
def subscriber_badge(
    peer_ref: str | None = Query(
        None,
        description="Explicit peer reference: user:123, chat:123, channel:381..., peer:5629...",
    ),
    peer_id: str | None = Query(
        None,
        description="Client/internal peer id or any supported peer reference string.",
    ),
    user_id: int | None = Query(None, gt=0),
    peer_type: str | None = Query(
        None,
        description="Optional explicit type for bare_id: user, chat or channel.",
    ),
    bare_id: int | None = Query(
        None,
        gt=0,
        description="Positive bare profile id used together with peer_type.",
    ),
    debug: bool = Query(False),
):
    try:
        lookup = resolve_lookup(
            peer_ref=peer_ref,
            peer_id=peer_id,
            user_id=user_id,
            peer_type=peer_type,
            bare_id=bare_id,
        )
    except ValueError as e:
        return JSONResponse(
            {"badge": False, "emoji_status_id": 0, "error": str(e)},
            status_code=400,
        )
    if lookup is None:
        return JSONResponse(
            {"badge": False, "emoji_status_id": 0, "error": "missing peer reference"},
            status_code=400,
        )

    conn = db_conn(DB_PATH)
    try:
        row = conn.execute(
            """
            SELECT peer_id, peer_type, bare_id, emoji_status_id, updated_at
            FROM subscribers
            WHERE peer_id = ?
            """,
            (lookup.peer_id,),
        ).fetchone()
    finally:
        conn.close()

    if not row:
        payload = {"badge": False, "emoji_status_id": 0}
        if debug:
            payload.update(
                {
                    "peer_id": lookup.peer_id,
                    "peer_type": lookup.peer_type,
                    "bare_id": lookup.bare_id,
                    "display_peer": lookup.display,
                    "canonical_ref": lookup.canonical_ref,
                }
            )
        return JSONResponse(payload)

    emoji_id = int(row["emoji_status_id"]) if row["emoji_status_id"] is not None else 0
    if emoji_id <= 0:
        emoji_id = DEFAULT_EMOJI_STATUS_ID

    payload = {"badge": True, "emoji_status_id": emoji_id}
    if debug:
        payload.update(
            {
                "peer_id": int(row["peer_id"]),
                "peer_type": str(row["peer_type"]),
                "bare_id": int(row["bare_id"]),
                "display_peer": describe_peer_ref(lookup),
                "canonical_ref": lookup.canonical_ref,
                "updated_at": int(row["updated_at"]),
            }
        )
    return JSONResponse(payload)
