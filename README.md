# Astrogram Desktop

<p align="center">
  <img src="Telegram/Resources/art/icon_round512@2x.png" width="160" alt="Astrogram logo">
</p>

<p align="center">
  Astrogram — десктопный Telegram-клиент с нативной системой плагинов, локальным runtime API и отдельным центром настроек Astrogram.
</p>

<p align="center">
  <a href="https://github.com/Perdonus/tgd/actions/workflows/build-desktop.yml"><img alt="Windows Build" src="https://github.com/Perdonus/tgd/actions/workflows/build-desktop.yml/badge.svg"></a>
  <a href="https://github.com/Perdonus/tgd/actions/workflows/build-desktop-force.yml"><img alt="Force Build" src="https://github.com/Perdonus/tgd/actions/workflows/build-desktop-force.yml/badge.svg"></a>
  <a href="https://docs.astrogram.su"><img alt="Docs" src="https://img.shields.io/badge/docs-astrogram.su-1f8b4c?style=flat-square"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-GPLv3-2b2b2b?style=flat-square"></a>
</p>

![Astrogram Preview](docs/assets/preview.png)

## Что такое Astrogram

Astrogram — это десктопный Telegram-клиент на базе Telegram Desktop с отдельным слоем возможностей Astrogram поверх него. Проект делает упор на собственный брендинг, более глубокую настройку клиента, нативную систему плагинов, локальные интеграции и единый раздел настроек Astrogram.

## Основные направления

### Клиент

- Брендинг Astrogram в названии приложения, иконках, метаданных и документации.
- Отдельный вход в настройки Astrogram прямо внутри клиента.
- Дополнительные desktop-функции, визуальные улучшения и настройки приватности.
- Windows-сборка поставляется как `Astrogram.exe`.

### Плагины

- Нативный формат плагинов `.tgd`.
- Встроенный менеджер плагинов с безопасным режимом, логами диагностики и восстановлением после сбоев.
- Примеры плагинов и структура репозитория, рассчитанная на каталог и версии.
- Архитектура плагинов, рассчитанная на интеграцию с runtime API.

### Runtime API

- Локальная runtime-поверхность для автоматизации, диагностики и внешних инструментов.
- Упор на локальные десктопные интеграции, а не на облачные прокладки.
- Документируется вместе с системой плагинов и инфраструктурой Astrogram.

## Сообщество

- Документация: [docs.astrogram.su](https://docs.astrogram.su)
- Канал: [@astrogramchannel](https://t.me/astrogramchannel)
- Чат: [@astrogram_chat](https://t.me/astrogram_chat)

## Скриншоты

| Main Window | Dialogs |
| --- | --- |
| ![Slide 1](docs/assets/slide.01.jpg) | ![Slide 2](docs/assets/slide.02.jpg) |
| ![Slide 3](docs/assets/slide.03.jpg) | ![Slide 4](docs/assets/slide.04.jpg) |

## Лицензия

Проект использует GPLv3 с OpenSSL exception. Подробности смотри в [LICENSE](LICENSE) и [LEGAL](LEGAL).

Спасибо ❤️ AyuGram, exteraGram, Kotatogram, 64Gram, Forkgram.
