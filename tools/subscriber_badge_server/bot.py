#!/usr/bin/env python3
import os
import sqlite3
import time
from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

BOT_TOKEN = os.environ.get("ASTRO_BADGE_BOT_TOKEN", "")
DB_PATH = os.environ.get("ASTRO_BADGE_DB", "./badge.db")
ADMIN_IDS = {
    int(x.strip())
    for x in os.environ.get("ASTRO_BADGE_ADMIN_IDS", "").split(",")
    if x.strip().isdigit()
}
DEFAULT_EMOJI_STATUS_ID = int(os.environ.get("ASTRO_BADGE_EMOJI_ID", "0"))
CHAT_TYPE_MASK = (1 << 48) - 1
BOT_API_CHANNEL_OFFSET = 10**12


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


def describe_peer_id(peer_id: int) -> str:
    shift = (int(peer_id) >> 48) & 0xFF
    bare = int(peer_id) & CHAT_TYPE_MASK
    if shift == 2:
        return f"channel(-100{bare})"
    if shift == 1:
        return f"chat(-{bare})"
    return f"user({bare})"


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


def is_admin(user_id: int) -> bool:
    return user_id in ADMIN_IDS if ADMIN_IDS else False


async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await update.message.reply_text(
        "Astrogram Badge Bot.\n"
        "Commands:\n"
        "/grant <peer_or_chat_id> [emoji_status_id]\n"
        "/revoke <peer_or_chat_id>\n"
        "/check <peer_or_chat_id>\n"
        "\n"
        "Examples:\n"
        "/grant 6603471853\n"
        "/grant -1003814280064"
    )


async def grant(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id if update.effective_user else 0
    if not is_admin(uid):
        await update.message.reply_text("Access denied.")
        return

    if not context.args:
        await update.message.reply_text("Usage: /grant <peer_or_chat_id> [emoji_status_id]")
        return

    try:
        peer_id = normalize_peer_id(int(context.args[0]))
        emoji_id = int(context.args[1]) if len(context.args) > 1 else DEFAULT_EMOJI_STATUS_ID
    except ValueError:
        await update.message.reply_text("Invalid ids")
        return

    conn = db_conn()
    try:
        conn.execute(
            "INSERT INTO subscribers(peer_id, emoji_status_id, updated_at) VALUES(?,?,?) "
            "ON CONFLICT(peer_id) DO UPDATE SET emoji_status_id=excluded.emoji_status_id, updated_at=excluded.updated_at",
            (peer_id, emoji_id, int(time.time())),
        )
        conn.commit()
    finally:
        conn.close()

    await update.message.reply_text(
        f"Granted badge for {describe_peer_id(peer_id)} / peer_id={peer_id}, emoji_status_id={emoji_id}"
    )


async def revoke(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id if update.effective_user else 0
    if not is_admin(uid):
        await update.message.reply_text("Access denied.")
        return

    if not context.args:
        await update.message.reply_text("Usage: /revoke <peer_or_chat_id>")
        return

    try:
        peer_id = normalize_peer_id(int(context.args[0]))
    except ValueError:
        await update.message.reply_text("Invalid peer/chat id")
        return

    conn = db_conn()
    try:
        conn.execute("DELETE FROM subscribers WHERE peer_id = ?", (peer_id,))
        conn.commit()
    finally:
        conn.close()

    await update.message.reply_text(
        f"Revoked badge for {describe_peer_id(peer_id)} / peer_id={peer_id}"
    )


async def check(update: Update, context: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id if update.effective_user else 0
    if not is_admin(uid):
        await update.message.reply_text("Access denied.")
        return

    if not context.args:
        await update.message.reply_text("Usage: /check <peer_or_chat_id>")
        return

    try:
        peer_id = normalize_peer_id(int(context.args[0]))
    except ValueError:
        await update.message.reply_text("Invalid peer/chat id")
        return

    conn = db_conn()
    try:
        row = conn.execute(
            "SELECT emoji_status_id, updated_at FROM subscribers WHERE peer_id = ?",
            (peer_id,),
        ).fetchone()
    finally:
        conn.close()

    if not row:
        await update.message.reply_text("No badge")
        return

    await update.message.reply_text(
        f"Badge enabled for {describe_peer_id(peer_id)} / peer_id={peer_id}: "
        f"emoji_status_id={row[0]}, updated_at={row[1]}"
    )


def main():
    if not BOT_TOKEN:
        raise RuntimeError("Set ASTRO_BADGE_BOT_TOKEN")
    if not ADMIN_IDS:
        raise RuntimeError("Set ASTRO_BADGE_ADMIN_IDS, comma-separated telegram numeric ids")

    app = Application.builder().token(BOT_TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CommandHandler("grant", grant))
    app.add_handler(CommandHandler("revoke", revoke))
    app.add_handler(CommandHandler("check", check))
    app.run_polling(allowed_updates=Update.ALL_TYPES)


if __name__ == "__main__":
    main()
