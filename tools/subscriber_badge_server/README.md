# Astrogram Subscriber Badge Server

Server-side badge flow for Astrogram client.

## What it does
- Telegram bot and CLI grant/revoke/check badge by peer.
- FastAPI endpoint returns badge status for client:
  - `GET /api/astrogram/subscriber-badge?peer_id=<internal_peer_id>`
  - backward-compatible: `GET /api/astrogram/subscriber-badge?user_id=<id>`
  - admin/debug-friendly:
    - `GET /api/astrogram/subscriber-badge?peer_ref=channel:3814280064`
    - `GET /api/astrogram/subscriber-badge?peer_type=channel&bare_id=3814280064`

Client-side expects JSON:
```json
{ "badge": true, "emoji_status_id": 1234567890 }
```

## Supported peer reference formats

- user id:
  - `6603471853`
  - `user:6603471853`
- basic group chat:
  - `-123456789`
  - `chat:123456789`
- channel / supergroup:
  - `-1003814280064`
  - `channel:3814280064`
- internal client peer id:
  - `peer:562953767701376`

Important:

- a plain positive number is treated as a user id;
- profile pages in Astrogram usually show the positive bare id;
- if that positive bare id belongs to a channel/supergroup, use `channel:<id>`;
- if that positive bare id belongs to a basic group, use `chat:<id>`.

## Setup

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run API

```bash
cd tools/subscriber_badge_server
export ASTRO_BADGE_DB=./badge.db
export ASTRO_BADGE_EMOJI_ID=0
uvicorn server:app --host 0.0.0.0 --port 8099
```

## Run bot

```bash
cd tools/subscriber_badge_server
export ASTRO_BADGE_DB=./badge.db
export ASTRO_BADGE_ADMIN_IDS=<your_numeric_telegram_id>
export ASTRO_BADGE_EMOJI_ID=<document_id_or_0>
python bot.py
```

Bot token can be provided in one of three ways:

- `ASTRO_BADGE_BOT_TOKEN`
- `ASTRO_BADGE_BOT_TOKEN_FILE`
- default file: `~/.config/astrogram/badge_bot_token`

## CLI management

```bash
cd tools/subscriber_badge_server
export ASTRO_BADGE_DB=./badge.db
python manage_badge.py grant channel:3814280064 --emoji-status-id 0
python manage_badge.py check peer:562953767701376
python manage_badge.py normalize channel:3814280064
python manage_badge.py list --limit 20
```

## Bot commands
- `/grant <peer_ref> [emoji_status_id]`
- `/grant_user <user_id> [emoji_status_id]`
- `/grant_chat <chat_id> [emoji_status_id]`
- `/grant_channel <channel_profile_id_or_-100id> [emoji_status_id]`
- `/revoke <peer_ref>`
- `/revoke_user <user_id>`
- `/revoke_chat <chat_id>`
- `/revoke_channel <channel_profile_id_or_-100id>`
- `/check <peer_ref>`
- `/check_user <user_id>`
- `/check_chat <chat_id>`
- `/check_channel <channel_profile_id_or_-100id>`
- `/normalize <peer_ref>`
- `/list [limit]`

Use reverse proxy to expose `/api/astrogram/subscriber-badge` on your production domain.
