#!/usr/bin/env python3
from __future__ import annotations

import os
from pathlib import Path

from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

from peer_ids import describe_peer_ref, parse_peer_ref
from store import db_conn, delete_badge, get_badge, list_badges, upsert_badge


DEFAULT_TOKEN_FILE = Path.home() / ".config" / "astrogram" / "badge_bot_token"
BOT_TOKEN = os.environ.get("ASTRO_BADGE_BOT_TOKEN", "").strip()
DB_PATH = os.environ.get("ASTRO_BADGE_DB", "./badge.db")
ADMIN_IDS = {
    int(x.strip())
    for x in os.environ.get("ASTRO_BADGE_ADMIN_IDS", "").split(",")
    if x.strip().isdigit()
}
DEFAULT_EMOJI_STATUS_ID = int(os.environ.get("ASTRO_BADGE_EMOJI_ID", "0"))


def load_token() -> str:
    if BOT_TOKEN:
        return BOT_TOKEN
    configured = os.environ.get("ASTRO_BADGE_BOT_TOKEN_FILE", "").strip()
    if configured:
        path = Path(configured).expanduser()
        if path.is_file():
            return path.read_text(encoding="utf-8").strip()
    if DEFAULT_TOKEN_FILE.is_file():
        return DEFAULT_TOKEN_FILE.read_text(encoding="utf-8").strip()
    return ""


def is_admin(user_id: int) -> bool:
    return user_id in ADMIN_IDS if ADMIN_IDS else False


def require_admin(update: Update) -> bool:
    uid = update.effective_user.id if update.effective_user else 0
    return is_admin(uid)


def usage_text() -> str:
    return (
        "Astrogram Badge Bot.\n"
        "Commands:\n"
        "/grant <peer_ref> [emoji_status_id]\n"
        "/grant_user <user_id> [emoji_status_id]\n"
        "/grant_chat <chat_id> [emoji_status_id]\n"
        "/grant_channel <channel_profile_id_or_-100id> [emoji_status_id]\n"
        "/revoke <peer_ref>\n"
        "/revoke_user <user_id>\n"
        "/revoke_chat <chat_id>\n"
        "/revoke_channel <channel_profile_id_or_-100id>\n"
        "/check <peer_ref>\n"
        "/check_user <user_id>\n"
        "/check_chat <chat_id>\n"
        "/check_channel <channel_profile_id_or_-100id>\n"
        "/normalize <peer_ref>\n"
        "/list [limit]\n"
        "\n"
        "Supported peer_ref formats:\n"
        "- user id: 6603471853 or user:6603471853\n"
        "- basic group chat id: -123456789 or chat:123456789\n"
        "- channel/supergroup Bot API id: -1003814280064\n"
        "- channel bare profile id: channel:3814280064\n"
        "- internal client peer id: peer:562949953421312\n"
        "\n"
        "Important: a positive bare id without prefix is treated as user.\n"
        "For channels from profile ids, use channel:<id>."
    )


async def start(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    await update.message.reply_text(usage_text())


async def grant(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not require_admin(update):
        await update.message.reply_text("Access denied.")
        return
    if not context.args:
        await update.message.reply_text("Usage: /grant <peer_ref> [emoji_status_id]")
        return
    try:
        peer = parse_peer_ref(context.args[0])
        emoji_id = (
            int(context.args[1])
            if len(context.args) > 1
            else DEFAULT_EMOJI_STATUS_ID
        )
    except ValueError as e:
        await update.message.reply_text(f"Invalid input: {e}")
        return
    conn = db_conn(DB_PATH)
    try:
        upsert_badge(conn, peer, emoji_id)
    finally:
        conn.close()
    await update.message.reply_text(
        f"Granted badge for {describe_peer_ref(peer)} / emoji_status_id={emoji_id}"
    )


def _typed_ref(kind: str, raw: str) -> str:
    return f"{kind}:{raw}"


async def grant_user(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /grant_user <user_id> [emoji_status_id]")
        return
    context.args[0] = _typed_ref("user", context.args[0])
    await grant(update, context)


async def grant_chat(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /grant_chat <chat_id> [emoji_status_id]")
        return
    context.args[0] = _typed_ref("chat", context.args[0])
    await grant(update, context)


async def grant_channel(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text(
            "Usage: /grant_channel <channel_profile_id_or_-100id> [emoji_status_id]"
        )
        return
    context.args[0] = _typed_ref("channel", context.args[0])
    await grant(update, context)


async def revoke(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not require_admin(update):
        await update.message.reply_text("Access denied.")
        return
    if not context.args:
        await update.message.reply_text("Usage: /revoke <peer_ref>")
        return
    try:
        peer = parse_peer_ref(context.args[0])
    except ValueError as e:
        await update.message.reply_text(f"Invalid input: {e}")
        return
    conn = db_conn(DB_PATH)
    try:
        delete_badge(conn, peer)
    finally:
        conn.close()
    await update.message.reply_text(f"Revoked badge for {describe_peer_ref(peer)}")


async def revoke_user(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /revoke_user <user_id>")
        return
    context.args[0] = _typed_ref("user", context.args[0])
    await revoke(update, context)


async def revoke_chat(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /revoke_chat <chat_id>")
        return
    context.args[0] = _typed_ref("chat", context.args[0])
    await revoke(update, context)


async def revoke_channel(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /revoke_channel <channel_profile_id_or_-100id>")
        return
    context.args[0] = _typed_ref("channel", context.args[0])
    await revoke(update, context)


async def check(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not require_admin(update):
        await update.message.reply_text("Access denied.")
        return
    if not context.args:
        await update.message.reply_text("Usage: /check <peer_ref>")
        return
    try:
        peer = parse_peer_ref(context.args[0])
    except ValueError as e:
        await update.message.reply_text(f"Invalid input: {e}")
        return
    conn = db_conn(DB_PATH)
    try:
        row = get_badge(conn, peer)
    finally:
        conn.close()
    if not row:
        await update.message.reply_text(f"No badge for {describe_peer_ref(peer)}")
        return
    await update.message.reply_text(
        f"Badge enabled for {describe_peer_ref(peer)}: "
        f"emoji_status_id={int(row['emoji_status_id'])}, "
        f"updated_at={int(row['updated_at'])}"
    )


async def check_user(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /check_user <user_id>")
        return
    context.args[0] = _typed_ref("user", context.args[0])
    await check(update, context)


async def check_chat(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /check_chat <chat_id>")
        return
    context.args[0] = _typed_ref("chat", context.args[0])
    await check(update, context)


async def check_channel(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.args:
        await update.message.reply_text("Usage: /check_channel <channel_profile_id_or_-100id>")
        return
    context.args[0] = _typed_ref("channel", context.args[0])
    await check(update, context)


async def normalize(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not require_admin(update):
        await update.message.reply_text("Access denied.")
        return
    if not context.args:
        await update.message.reply_text("Usage: /normalize <peer_ref>")
        return
    try:
        peer = parse_peer_ref(context.args[0])
    except ValueError as e:
        await update.message.reply_text(f"Invalid input: {e}")
        return
    await update.message.reply_text(describe_peer_ref(peer))


async def list_command(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not require_admin(update):
        await update.message.reply_text("Access denied.")
        return
    limit = 20
    if context.args:
        try:
            limit = max(1, min(int(context.args[0]), 50))
        except ValueError:
            await update.message.reply_text("Usage: /list [limit]")
            return
    conn = db_conn(DB_PATH)
    try:
        rows = list_badges(conn, limit)
    finally:
        conn.close()
    if not rows:
        await update.message.reply_text("No badge records.")
        return
    lines = []
    for row in rows:
        peer = parse_peer_ref(f"{row['peer_type']}:{int(row['bare_id'])}")
        lines.append(
            f"- {describe_peer_ref(peer)} / "
            f"emoji_status_id={int(row['emoji_status_id'])}"
        )
    text = "Badge records:\n" + "\n".join(lines)
    await update.message.reply_text(text[:4000])


def main() -> None:
    token = load_token()
    if not token:
        raise RuntimeError(
            "Set ASTRO_BADGE_BOT_TOKEN or ASTRO_BADGE_BOT_TOKEN_FILE "
            f"(default file: {DEFAULT_TOKEN_FILE})"
        )
    if not ADMIN_IDS:
        raise RuntimeError("Set ASTRO_BADGE_ADMIN_IDS, comma-separated telegram numeric ids")

    app = Application.builder().token(token).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CommandHandler("grant", grant))
    app.add_handler(CommandHandler("grant_user", grant_user))
    app.add_handler(CommandHandler("grant_chat", grant_chat))
    app.add_handler(CommandHandler("grant_channel", grant_channel))
    app.add_handler(CommandHandler("revoke", revoke))
    app.add_handler(CommandHandler("revoke_user", revoke_user))
    app.add_handler(CommandHandler("revoke_chat", revoke_chat))
    app.add_handler(CommandHandler("revoke_channel", revoke_channel))
    app.add_handler(CommandHandler("check", check))
    app.add_handler(CommandHandler("check_user", check_user))
    app.add_handler(CommandHandler("check_chat", check_chat))
    app.add_handler(CommandHandler("check_channel", check_channel))
    app.add_handler(CommandHandler("normalize", normalize))
    app.add_handler(CommandHandler("list", list_command))
    app.run_polling(allowed_updates=Update.ALL_TYPES)


if __name__ == "__main__":
    main()
