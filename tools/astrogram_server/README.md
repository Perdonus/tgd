# Astrogram Server Tooling

Автономный сервис для:

- серверных badge по `peer_id`
- trusted plugin sources по `channel_id`
- exact plugin records по `sha256 / channel_id / message_id`
- Telegram admin bot, который отвечает только user `6603471853`

Каталог полностью автономный и не требует правок клиентского C++.

## Хранилище

По умолчанию состояние лежит в `tools/astrogram_server/state`:

- `astrogram_server.sqlite3` — основная SQLite-база
- `bot_state.json` — offset для long-polling бота

Путь переопределяется через `ASTROGRAM_SERVER_STATE_DIR` или `--state-dir`.

## Переменные окружения

- `ASTROGRAM_BADGE_BOT_TOKEN` — обязательный токен Telegram-бота для `bot` и `run`
- `ASTROGRAM_SERVER_STATE_DIR` — каталог с SQLite и bot state

Admin user id не настраивается: бот отвечает только `6603471853`. Токен в код не вшивается.

## Запуск

HTTP:

```bash
python3 -m tools.astrogram_server --state-dir /srv/astrogram_server serve --host 0.0.0.0 --port 8099
```

Только бот:

```bash
export ASTROGRAM_BADGE_BOT_TOKEN=...
python3 -m tools.astrogram_server --state-dir /srv/astrogram_server bot
```

HTTP + бот:

```bash
export ASTROGRAM_BADGE_BOT_TOKEN=...
python3 -m tools.astrogram_server --state-dir /srv/astrogram_server run --host 0.0.0.0 --port 8099
```

## CLI

Generic peer badge:

```bash
python3 -m tools.astrogram_server badge-set 6603471853 --kind server --label "Astrogram Server"
python3 -m tools.astrogram_server badge-set -1003814280064 --peer-type channel --kind verified --label "Trusted channel" --title "Astro Plugins" --username astroplugins
python3 -m tools.astrogram_server badge-clear 6603471853
python3 -m tools.astrogram_server badge-show 6603471853
```

Typed badge commands:

```bash
python3 -m tools.astrogram_server user-badge-set 6603471853 --kind server --label "Astrogram"
python3 -m tools.astrogram_server user-badge-clear 6603471853
python3 -m tools.astrogram_server user-badge-show 6603471853

python3 -m tools.astrogram_server channel-badge-set -1003814280064 --kind verified --label "Astro Plugins" --title "Astro Plugins" --username astroplugins
python3 -m tools.astrogram_server channel-badge-clear -1003814280064
python3 -m tools.astrogram_server channel-badge-show -1003814280064
```

Trusted source:

```bash
python3 -m tools.astrogram_server trusted-source-set -1003814280064 --label "AstroPlugins" --title "Astro Plugins" --username astroplugins
python3 -m tools.astrogram_server trusted-source-clear -1003814280064
python3 -m tools.astrogram_server trusted-source-show -1003814280064
```

Plugin record:

```bash
python3 -m tools.astrogram_server plugin-record-set \
  0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
  -1003814280064 \
  777 \
  --label "Astroku stable" \
  --channel-title "Astro Plugins" \
  --channel-username astroplugins

python3 -m tools.astrogram_server plugin-record-clear \
  0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef

python3 -m tools.astrogram_server plugin-record-show \
  0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

Snapshot:

```bash
python3 -m tools.astrogram_server snapshot --pretty
```

## Telegram Bot

Бот отвечает только сообщению от user `6603471853`. Для всех остальных — полный ignore без ответа.

Поддерживаемые команды:

- `/help`
- `/ping`
- `/set_badge <peer_id> [kind] [label]`
- `/set_user_badge <user_id> [kind] [label]`
- `/set_channel_badge <channel_id> [kind] [label]`
- `/clear_badge <peer_id>`
- `/clear_user_badge <user_id>`
- `/clear_channel_badge <channel_id>`
- `/show_badge <peer_id>`
- `/show_user_badge <user_id>`
- `/show_channel_badge <channel_id>`
- `/set_trusted_source <channel_id> [label]`
- `/clear_trusted_source <channel_id>`
- `/show_trusted_source <channel_id>`
- `/set_plugin_record <sha256> <channel_id> <message_id> [label]`
- `/clear_plugin_record <sha256>`
- `/show_plugin_record <sha256>`
- `/snapshot`

Для channel-badge, trusted-source и plugin-record бот пытается дернуть `getChat`, чтобы автоматически сохранить `title` и `username` канала.

## HTTP API

Все ответы: JSON, `Cache-Control: no-store`, `Access-Control-Allow-Origin: *`.

### Service

- `GET /health`
- `GET /v1/meta`
- `GET /v1/snapshot`
- `GET /v1/changes?since_revision=<n>&timeout=<seconds>`

### Peer Badge

- `GET /v1/peers/<peer_id>/badge`
- `GET /v1/users/<user_id>/badge`
- `GET /v1/channels/<channel_id>/badge`

Пример:

```json
{
  "ok": true,
  "revision": 12,
  "peer_id": "-1003814280064",
  "found": true,
  "badge": {
    "peer_id": "-1003814280064",
    "peer_type": "channel",
    "badge_kind": "verified",
    "kind": "verified",
    "label": "Astro Plugins",
    "title": "Astro Plugins",
    "username": "astroplugins",
    "verified": true,
    "trusted": true,
    "confirmed": true,
    "peer": {
      "id": "-1003814280064",
      "type": "channel",
      "title": "Astro Plugins",
      "username": "astroplugins"
    },
    "payload": {
      "peer_type": "channel",
      "title": "Astro Plugins",
      "username": "astroplugins"
    },
    "updated_at": "2026-04-20T18:00:00Z"
  }
}
```

### Trusted Source

- `GET /v1/channels/<channel_id>/trusted-source`

Пример:

```json
{
  "ok": true,
  "revision": 12,
  "channel_id": "-1003814280064",
  "found": true,
  "trusted_channel": {
    "id": "-1003814280064",
    "type": "channel",
    "title": "Astro Plugins",
    "username": "astroplugins"
  },
  "trusted_source": {
    "channel_id": "-1003814280064",
    "label": "AstroPlugins",
    "title": "Astro Plugins",
    "name": "Astro Plugins",
    "username": "astroplugins",
    "channel_title": "Astro Plugins",
    "channelTitle": "Astro Plugins",
    "channel_username": "astroplugins",
    "channelUsername": "astroplugins",
    "verified": true,
    "trusted": true,
    "confirmed": true,
    "peer": {
      "id": "-1003814280064",
      "type": "channel",
      "title": "Astro Plugins",
      "username": "astroplugins"
    },
    "payload": {
      "title": "Astro Plugins",
      "username": "astroplugins"
    },
    "updated_at": "2026-04-20T18:00:00Z"
  }
}
```

### Plugin Trust

- `GET /v1/plugins/<sha256>`

Пример:

```json
{
  "ok": true,
  "revision": 12,
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "found": true,
  "status": "verified",
  "channel_title": "Astro Plugins",
  "channel_username": "astroplugins",
  "trusted_channel": {
    "id": "-1003814280064",
    "type": "channel",
    "title": "Astro Plugins",
    "username": "astroplugins"
  },
  "plugin_record": {
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "channel_id": "-1003814280064",
    "message_id": 777,
    "label": "Astroku stable",
    "channel_title": "Astro Plugins",
    "channel_username": "astroplugins",
    "source_peer": {
      "id": "-1003814280064",
      "type": "channel",
      "title": "Astro Plugins",
      "username": "astroplugins"
    },
    "payload": {},
    "trusted_source": {
      "channel_id": "-1003814280064",
      "label": "AstroPlugins",
      "title": "Astro Plugins",
      "username": "astroplugins"
    },
    "updated_at": "2026-04-20T18:00:00Z"
  },
  "trusted_source": {
    "channel_id": "-1003814280064",
    "label": "AstroPlugins",
    "title": "Astro Plugins",
    "username": "astroplugins"
  }
}
```

`status`:

- `verified` — есть plugin-record и его канал находится в trusted sources
- `known_record` — plugin-record найден, но канал не trusted
- `unverified` — записи по `sha256` нет
