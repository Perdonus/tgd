# Trusted Plugins Sync

Этот бот автоматически пополняет список доверенных `.tgd`-пакетов для Astrogram.

Что он делает:

- слушает `channel_post` из канала-проверяльщика;
- берёт только документы с расширением `.tgd`;
- скачивает бинарник через Bot API;
- считает exact `SHA-256`;
- сохраняет запись в SQLite;
- каждый раз пересобирает готовый JSON-блок для `appConfig`.

Важно:

- бот видит только новые посты после того, как его добавили в канал;
- старые `.tgd`, опубликованные до добавления бота, Telegram Bot API сам по себе не отдаст;
- если нужно добрать старые пакеты, их надо либо перепостить в checker-канал, либо сделать отдельный backfill уже через user-session, не через бота.

## Настройка

Переменные окружения:

- `ASTROGRAM_TRUST_BOT_TOKEN` или `ASTRO_TRUST_BOT_TOKEN`
- `ASTROGRAM_TRUST_BOT_TOKEN_FILE` или `ASTRO_TRUST_BOT_TOKEN_FILE`
- `ASTROGRAM_TRUST_DB` или `ASTRO_TRUST_DB`
- `ASTROGRAM_TRUST_OUTPUT` или `ASTRO_TRUST_OUTPUT`
- `ASTROGRAM_TRUST_CHANNEL_IDS` или `ASTRO_TRUST_CHANNEL_IDS`
- `ASTROGRAM_TRUST_CHANNEL_LABELS` или `ASTRO_TRUST_CHANNEL_LABELS`
- `ASTROGRAM_TRUST_ADMIN_IDS` или `ASTRO_TRUST_ADMIN_IDS`

Если токен не задан через env, бот попытается прочитать его из:

`~/.config/astrogram/trusted_plugins_bot_token`

Если `DB` и `OUTPUT` не заданы, по умолчанию он пишет сюда:

- `~/.local/state/astrogram/trusted_plugins.db`
- `~/.local/state/astrogram/trusted_plugins.export.json`

Пример:

```bash
export ASTROGRAM_TRUST_CHANNEL_IDS="-1003814280064"
export ASTROGRAM_TRUST_CHANNEL_LABELS="-1003814280064=AstroPlugins"
export ASTROGRAM_TRUST_ADMIN_IDS="123456789"
export ASTROGRAM_TRUST_OUTPUT="/srv/astrogram/trusted_plugins.export.json"
python3 tools/trusted_plugins_sync/bot.py
```

`ASTROGRAM_TRUST_CHANNEL_LABELS` попадает в клиентский UI как человекочитаемая метка доверенной записи. Лучше указывать там короткое нормальное имя источника, например `AstroPlugins`.

## Формат экспорта

Бот пишет JSON, который можно почти напрямую подставить в серверный `appConfig`:

```json
{
  "astrogram_trusted_plugin_channel_ids": [
    -1003814280064
  ],
  "astrogram_trusted_plugin_records": [
    "sha256|channelId|messageId|label"
  ]
}
```

Каждая запись означает:

```text
sha256|channelId|messageId|label
```

Именно такой exact hash уже используется клиентом для синей/красной плашки источника.

## Команды

- `/stats` — сколько сообщений и уникальных hash уже накоплено
- `/export` — переписать экспорт и прислать текущий JSON в чат админа

Команды работают только для `ASTROGRAM_TRUST_ADMIN_IDS` / `ASTRO_TRUST_ADMIN_IDS`.

## Одноразовая пересборка JSON

Если база уже есть и нужно просто переписать export:

```bash
python3 tools/trusted_plugins_sync/bot.py --export-only
```
