from __future__ import annotations

import json
import re
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse

from .storage import AstrogramServerStorage


PEER_BADGE_RE = re.compile(r"^/v1/peers/(?P<peer_id>[^/]+)/badge/?$")
USER_BADGE_RE = re.compile(r"^/v1/users/(?P<peer_id>[^/]+)/badge/?$")
CHANNEL_BADGE_RE = re.compile(r"^/v1/channels/(?P<channel_id>[^/]+)/badge/?$")
TRUSTED_SOURCE_RE = re.compile(r"^/v1/channels/(?P<channel_id>[^/]+)/trusted-source/?$")
PLUGIN_RE = re.compile(r"^/v1/plugins/(?P<sha256>[0-9a-fA-F]{64})/?$")


class AstrogramHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        server_address: tuple[str, int],
        storage: AstrogramServerStorage,
    ) -> None:
        super().__init__(server_address, AstrogramRequestHandler)
        self.storage = storage


class AstrogramRequestHandler(BaseHTTPRequestHandler):
    server: AstrogramHTTPServer

    def log_message(self, format: str, *args: Any) -> None:
        return

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query, keep_blank_values=True)

        if path == "/health":
            self._write_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "service": "astrogram_server",
                    "revision": self.server.storage.get_revision(),
                },
            )
            return

        if path == "/v1/meta":
            self._write_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "revision": self.server.storage.get_revision(),
                    "counts": self.server.storage.get_counts(),
                },
            )
            return

        if path == "/v1/snapshot":
            snapshot = self.server.storage.export_snapshot()
            self._write_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    **snapshot,
                },
            )
            return

        if path == "/v1/changes":
            since_revision = self._int_query_value(query, "since_revision", 0)
            timeout = min(max(self._float_query_value(query, "timeout", 0.0), 0.0), 30.0)
            if timeout > 0:
                self.server.storage.wait_for_revision_change(since_revision, timeout)
            payload = self.server.storage.get_changes_since(since_revision)
            self._write_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "changed": payload["revision"] > since_revision,
                    **payload,
                },
            )
            return

        peer_match = PEER_BADGE_RE.match(path)
        if peer_match:
            peer_id = peer_match.group("peer_id")
            badge = self._resolve_peer_badge(peer_id, query)
            self._write_badge_response(peer_id, badge)
            return

        user_badge_match = USER_BADGE_RE.match(path)
        if user_badge_match:
            peer_id = user_badge_match.group("peer_id")
            badge = self.server.storage.get_user_badge(peer_id)
            self._write_badge_response(peer_id, badge)
            return

        channel_badge_match = CHANNEL_BADGE_RE.match(path)
        if channel_badge_match:
            peer_id = channel_badge_match.group("channel_id")
            badge = self.server.storage.get_channel_badge(peer_id)
            self._write_badge_response(peer_id, badge)
            return

        trusted_match = TRUSTED_SOURCE_RE.match(path)
        if trusted_match:
            channel_id = trusted_match.group("channel_id")
            trusted = self.server.storage.get_trusted_source(channel_id)
            self._write_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "revision": self.server.storage.get_revision(),
                    "channel_id": channel_id,
                    "found": trusted is not None,
                    "trusted_source": trusted,
                    "trusted_channel": trusted["peer"] if trusted else None,
                },
            )
            return

        plugin_match = PLUGIN_RE.match(path)
        if plugin_match:
            sha256 = plugin_match.group("sha256").lower()
            record = self.server.storage.get_plugin_record(sha256)
            trusted_source = record["trusted_source"] if record else None
            trusted_channel = None
            channel_title = ""
            channel_username = ""
            if record:
                trusted_channel = record["source_peer"]
                channel_title = record["channel_title"]
                channel_username = record["channel_username"]
            status = "unverified"
            if record and trusted_source:
                status = "verified"
            elif record:
                status = "known_record"
            self._write_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "revision": self.server.storage.get_revision(),
                    "sha256": sha256,
                    "found": record is not None,
                    "status": status,
                    "channel_title": channel_title,
                    "channel_username": channel_username,
                    "plugin_record": record,
                    "trusted_source": trusted_source,
                    "trusted_channel": trusted_channel,
                },
            )
            return

        self._write_json(
            HTTPStatus.NOT_FOUND,
            {
                "ok": False,
                "error": "not_found",
                "path": path,
            },
        )

    def _write_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True).encode(
            "utf-8"
        )
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(encoded)

    def _write_badge_response(
        self,
        peer_id: str,
        badge: dict[str, Any] | None,
    ) -> None:
        self._write_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "revision": self.server.storage.get_revision(),
                "peer_id": peer_id,
                "found": badge is not None,
                "badge": badge,
            },
        )

    def _resolve_peer_badge(
        self,
        peer_id: str,
        query: dict[str, list[str]],
    ) -> dict[str, Any] | None:
        storage = self.server.storage
        if badge := storage.get_peer_badge(peer_id):
            return badge

        peer_ref = self._string_query_value(query, "peer_ref")
        if peer_ref and (badge := storage.get_peer_badge(peer_ref)):
            return badge

        peer_type = self._resolve_peer_type(peer_id, peer_ref, query)
        if peer_type == "user":
            user_id = (
                self._string_query_value(query, "user_id")
                or self._string_query_value(query, "bare_id")
                or self._string_query_value(query, "peer_bare_id")
                or self._extract_peer_ref_value(peer_ref, "user")
                or peer_id
            )
            return storage.get_user_badge(user_id)
        if peer_type == "channel":
            channel_id = (
                self._normalize_channel_id(self._string_query_value(query, "channel_id"))
                or self._normalize_channel_id(self._string_query_value(query, "bare_id"))
                or self._normalize_channel_id(self._string_query_value(query, "peer_bare_id"))
                or self._normalize_channel_id(self._extract_peer_ref_value(peer_ref, "channel"))
                or self._normalize_channel_id(peer_id)
            )
            return storage.get_channel_badge(channel_id) if channel_id else None
        if peer_type == "chat":
            chat_id = (
                self._string_query_value(query, "chat_id")
                or self._string_query_value(query, "bare_id")
                or self._string_query_value(query, "peer_bare_id")
                or self._extract_peer_ref_value(peer_ref, "chat")
            )
            if chat_id:
                return storage.get_peer_badge(f"chat:{chat_id}")
        return None

    @staticmethod
    def _string_query_value(query: dict[str, list[str]], name: str) -> str:
        return query.get(name, [""])[0].strip()

    @staticmethod
    def _extract_peer_ref_value(peer_ref: str, expected_type: str) -> str:
        if not peer_ref:
            return ""
        prefix = f"{expected_type}:"
        if not peer_ref.startswith(prefix):
            return ""
        return peer_ref[len(prefix) :].strip()

    @classmethod
    def _resolve_peer_type(
        cls,
        peer_id: str,
        peer_ref: str,
        query: dict[str, list[str]],
    ) -> str:
        raw = cls._string_query_value(query, "peer_type").lower()
        if raw in {"user", "channel", "chat"}:
            return raw
        for expected in ("user", "channel", "chat"):
            if cls._extract_peer_ref_value(peer_ref, expected):
                return expected
        if peer_id.startswith("-100"):
            return "channel"
        return ""

    @staticmethod
    def _normalize_channel_id(value: str) -> str:
        value = (value or "").strip()
        if not value:
            return ""
        if value.startswith("-100"):
            return value
        if value.startswith("channel:"):
            value = value.split(":", 1)[1].strip()
        return f"-100{value.lstrip('+')}"

    @staticmethod
    def _int_query_value(query: dict[str, list[str]], name: str, default: int) -> int:
        raw = query.get(name, [str(default)])[0]
        try:
            return int(raw)
        except ValueError:
            return default

    @staticmethod
    def _float_query_value(query: dict[str, list[str]], name: str, default: float) -> float:
        raw = query.get(name, [str(default)])[0]
        try:
            return float(raw)
        except ValueError:
            return default


def serve_http(
    storage: AstrogramServerStorage,
    host: str = "127.0.0.1",
    port: int = 8099,
) -> None:
    server = AstrogramHTTPServer((host, port), storage)
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        server.server_close()


def start_http_in_thread(
    storage: AstrogramServerStorage,
    host: str = "127.0.0.1",
    port: int = 8099,
) -> tuple[AstrogramHTTPServer, threading.Thread]:
    server = AstrogramHTTPServer((host, port), storage)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread
