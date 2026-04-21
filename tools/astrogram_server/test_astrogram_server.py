from __future__ import annotations

import json
import tempfile
import unittest
from unittest import mock
from urllib.request import ProxyHandler, build_opener

from tools.astrogram_server.bot import AstrogramBadgeBot, DEFAULT_ADMIN_USER_ID
from tools.astrogram_server.http_api import start_http_in_thread
from tools.astrogram_server.storage import AstrogramServerStorage


TEST_SHA256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
TEST_CHANNEL_ID = "-1003814280064"
TEST_CHANNEL_TITLE = "Astro Plugins"
TEST_CHANNEL_USERNAME = "astroplugins"


class FakeBadgeBot(AstrogramBadgeBot):
    def __init__(self, storage: AstrogramServerStorage) -> None:
        super().__init__(storage, token="test-token", poll_timeout=1)
        self.chat_payloads: dict[str, dict[str, object]] = {}
        self.sent_messages: list[dict[str, object]] = []
        self.command_payloads: list[dict[str, object]] = []

    def _api(self, method: str, payload: dict[str, object]) -> dict[str, object]:
        if method == "getChat":
            chat_id = str(payload["chat_id"])
            if chat_id not in self.chat_payloads:
                raise RuntimeError(f"missing chat metadata for {chat_id}")
            return {"ok": True, "result": self.chat_payloads[chat_id]}
        if method == "setMyCommands":
            self.command_payloads.append(dict(payload))
            return {"ok": True, "result": True}
        if method == "sendMessage":
            self.sent_messages.append(dict(payload))
            return {"ok": True, "result": {"message_id": len(self.sent_messages)}}
        raise AssertionError(f"unexpected method: {method}")


class AstrogramServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.storage = AstrogramServerStorage(self.temp_dir.name)
        self.http = build_opener(ProxyHandler({}))

    def tearDown(self) -> None:
        self.storage.close()
        self.temp_dir.cleanup()

    def test_http_plugin_trust_includes_trusted_channel_metadata(self) -> None:
        self.storage.set_channel_badge(
            TEST_CHANNEL_ID,
            "server",
            label="Astrogram Server",
            actor="test",
            title=TEST_CHANNEL_TITLE,
            username=TEST_CHANNEL_USERNAME,
        )
        self.storage.set_trusted_source(
            TEST_CHANNEL_ID,
            label="AstroPlugins",
            actor="test",
            title=TEST_CHANNEL_TITLE,
            username=TEST_CHANNEL_USERNAME,
        )
        self.storage.set_plugin_record(
            TEST_SHA256,
            TEST_CHANNEL_ID,
            777,
            label="Astroku stable",
            actor="test",
        )

        server, thread = start_http_in_thread(self.storage, host="127.0.0.1", port=0)
        try:
            port = int(server.server_address[1])
            with self.http.open(
                f"http://127.0.0.1:{port}/v1/plugins/{TEST_SHA256}",
                timeout=5,
            ) as response:
                plugin_payload = json.loads(response.read().decode("utf-8"))
            with self.http.open(
                f"http://127.0.0.1:{port}/v1/channels/{TEST_CHANNEL_ID}/badge",
                timeout=5,
            ) as response:
                badge_payload = json.loads(response.read().decode("utf-8"))

            self.assertEqual(plugin_payload["status"], "verified")
            self.assertEqual(plugin_payload["channel_title"], TEST_CHANNEL_TITLE)
            self.assertEqual(plugin_payload["channel_username"], TEST_CHANNEL_USERNAME)
            self.assertEqual(plugin_payload["trusted_channel"]["title"], TEST_CHANNEL_TITLE)
            self.assertEqual(
                plugin_payload["trusted_source"]["username"],
                TEST_CHANNEL_USERNAME,
            )
            self.assertTrue(badge_payload["found"])
            self.assertEqual(badge_payload["badge"]["peer_type"], "channel")
            self.assertEqual(badge_payload["badge"]["title"], TEST_CHANNEL_TITLE)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_http_peer_badge_resolves_typed_lookup_from_query(self) -> None:
        test_user_id = "6603471853"
        self.storage.set_user_badge(
            test_user_id,
            "server",
            label="Astrogram",
            actor="test",
            title="Tomnaya Povesa",
        )

        server, thread = start_http_in_thread(self.storage, host="127.0.0.1", port=0)
        try:
            port = int(server.server_address[1])
            with self.http.open(
                "http://127.0.0.1:"
                f"{port}/v1/peers/987654321/badge"
                "?peer_type=user&user_id=6603471853&peer_ref=user:6603471853",
                timeout=5,
            ) as response:
                payload = json.loads(response.read().decode("utf-8"))

            self.assertTrue(payload["found"])
            self.assertEqual(payload["badge"]["peer_type"], "user")
            self.assertEqual(payload["badge"]["peer_id"], test_user_id)
            self.assertEqual(payload["badge"]["label"], "Astrogram")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_bot_responds_only_to_admin_and_enriches_channel_metadata(self) -> None:
        bot = FakeBadgeBot(self.storage)
        bot.chat_payloads[TEST_CHANNEL_ID] = {
            "id": int(TEST_CHANNEL_ID),
            "type": "channel",
            "title": TEST_CHANNEL_TITLE,
            "username": TEST_CHANNEL_USERNAME,
        }

        bot._handle_update(
            {
                "update_id": 1,
                "message": {
                    "message_id": 1,
                    "from": {"id": 1},
                    "chat": {"id": 1},
                    "text": "/set_trusted_source -1001 test",
                },
            }
        )
        self.assertEqual(bot.sent_messages, [])

        bot._handle_update(
            {
                "update_id": 2,
                "message": {
                    "message_id": 2,
                    "from": {"id": DEFAULT_ADMIN_USER_ID},
                    "chat": {"id": DEFAULT_ADMIN_USER_ID},
                    "text": f"/set_trusted_source {TEST_CHANNEL_ID} AstroPlugins",
                },
            }
        )

        trusted = self.storage.get_trusted_source(TEST_CHANNEL_ID)
        self.assertIsNotNone(trusted)
        self.assertEqual(trusted["title"], TEST_CHANNEL_TITLE)
        self.assertEqual(trusted["username"], TEST_CHANNEL_USERNAME)
        self.assertEqual(len(bot.sent_messages), 1)
        self.assertIn("trusted source set", str(bot.sent_messages[0]["text"]))

    def test_from_env_ignores_admin_override(self) -> None:
        with mock.patch.dict(
            "os.environ",
            {
                "ASTROGRAM_BADGE_BOT_TOKEN": "test-token",
                "ASTROGRAM_BADGE_ADMIN_USER_ID": "1",
            },
            clear=False,
        ):
            bot = AstrogramBadgeBot.from_env(self.storage, poll_timeout=1)
        self.assertEqual(bot._admin_user_id, DEFAULT_ADMIN_USER_ID)

    def test_bot_syncs_telegram_commands(self) -> None:
        bot = FakeBadgeBot(self.storage)
        bot._ensure_bot_setup()
        self.assertEqual(len(bot.command_payloads), 1)
        raw = bot.command_payloads[0]["commands"]
        commands = json.loads(str(raw))
        self.assertTrue(any(item["command"] == "set_trusted_source" for item in commands))
        self.assertTrue(any(item["command"] == "set_channel_badge" for item in commands))
        self.assertTrue(bot._commands_synced)


if __name__ == "__main__":
    unittest.main()
