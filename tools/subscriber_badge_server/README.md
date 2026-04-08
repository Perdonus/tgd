# Astrogram Subscriber Badge Server

Server-side badge flow for Astrogram client.

## What it does
- Telegram bot grants/revokes badge by peer:
  - user id: `6603471853`
  - basic group chat id: `-123456789`
  - channel/supergroup Bot API id: `-1001234567890`
- FastAPI endpoint returns badge status for client:
  - `GET /api/astrogram/subscriber-badge?peer_id=<internal_peer_id>`
  - backward-compatible: `GET /api/astrogram/subscriber-badge?user_id=<id>`

Client-side expects JSON:
```json
{ "badge": true, "emoji_status_id": 1234567890 }
```

## Setup

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run API

```bash
export ASTRO_BADGE_DB=./badge.db
export ASTRO_BADGE_EMOJI_ID=0
uvicorn server:app --host 0.0.0.0 --port 8099
```

## Run bot

```bash
export ASTRO_BADGE_DB=./badge.db
export ASTRO_BADGE_BOT_TOKEN=<telegram_bot_token>
export ASTRO_BADGE_ADMIN_IDS=<your_numeric_telegram_id>
export ASTRO_BADGE_EMOJI_ID=<document_id_or_0>
python bot.py
```

## Bot commands
- `/grant <peer_or_chat_id> [emoji_status_id]`
- `/revoke <peer_or_chat_id>`
- `/check <peer_or_chat_id>`

Use reverse proxy to expose `/api/astrogram/subscriber-badge` on your production domain.
