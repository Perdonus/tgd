#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass

CHAT_TYPE_MASK = (1 << 48) - 1
BOT_API_CHANNEL_OFFSET = 10**12

_TYPE_TO_SHIFT = {
    "user": 0,
    "chat": 1,
    "channel": 2,
}
_SHIFT_TO_TYPE = {value: key for key, value in _TYPE_TO_SHIFT.items()}
_ALIASES = {
    "user": "user",
    "u": "user",
    "chat": "chat",
    "group": "chat",
    "basicchat": "chat",
    "basic_group": "chat",
    "g": "chat",
    "channel": "channel",
    "supergroup": "channel",
    "sg": "channel",
    "ch": "channel",
    "peer": "peer",
    "peerid": "peer",
    "internal": "peer",
}


@dataclass(frozen=True)
class PeerRef:
    raw: str
    peer_id: int
    peer_type: str
    bare_id: int

    @property
    def bot_api_id(self) -> int:
        if self.peer_type == "channel":
            return -(BOT_API_CHANNEL_OFFSET + self.bare_id)
        if self.peer_type == "chat":
            return -self.bare_id
        return self.bare_id

    @property
    def display(self) -> str:
        if self.peer_type == "channel":
            return f"channel({self.bot_api_id})"
        if self.peer_type == "chat":
            return f"chat({self.bot_api_id})"
        return f"user({self.bare_id})"

    @property
    def canonical_ref(self) -> str:
        return f"{self.peer_type}:{self.bare_id}"


def _parse_int(value: str | int) -> int:
    if isinstance(value, int):
        return value
    text = str(value).strip().replace("_", "")
    if not text:
        raise ValueError("empty peer reference")
    return int(text, 10)


def make_peer_id(peer_type: str, bare_id: int) -> int:
    if bare_id <= 0:
        raise ValueError("bare peer id must be positive")
    shift = _TYPE_TO_SHIFT.get(peer_type)
    if shift is None:
        raise ValueError(f"unsupported peer type: {peer_type}")
    return int((shift << 48) | bare_id)


def unpack_peer_id(peer_id: int) -> tuple[str, int]:
    value = int(peer_id)
    if value <= 0:
        raise ValueError("peer id must be positive")
    if value <= CHAT_TYPE_MASK:
        return ("user", value)
    shift = (value >> 48) & 0xFF
    bare_id = value & CHAT_TYPE_MASK
    peer_type = _SHIFT_TO_TYPE.get(shift)
    if peer_type is None or bare_id <= 0:
        raise ValueError("unsupported internal peer id")
    return (peer_type, bare_id)


def build_peer_ref(peer_type: str, bare_id: int, raw: str | None = None) -> PeerRef:
    peer_type = _ALIASES.get(peer_type.lower().strip(), peer_type.lower().strip())
    if peer_type not in _TYPE_TO_SHIFT:
        raise ValueError(f"unsupported peer type: {peer_type}")
    bare_id = abs(int(bare_id))
    if bare_id <= 0:
        raise ValueError("bare peer id must be positive")
    return PeerRef(
        raw=(raw.strip() if isinstance(raw, str) else "") or f"{peer_type}:{bare_id}",
        peer_id=make_peer_id(peer_type, bare_id),
        peer_type=peer_type,
        bare_id=bare_id,
    )


def parse_peer_ref(value: str | int, *, default_positive_type: str = "user") -> PeerRef:
    raw = str(value).strip()
    if not raw:
        raise ValueError("empty peer reference")

    if ":" in raw:
        prefix, body = raw.split(":", 1)
        kind = _ALIASES.get(prefix.lower().strip())
        if kind is None:
            raise ValueError(
                "unknown peer prefix; use user:, chat:, channel: or peer:"
            )
        numeric = _parse_int(body)
        if kind == "peer":
            peer_type, bare_id = unpack_peer_id(numeric)
            return PeerRef(raw=raw, peer_id=int(numeric), peer_type=peer_type, bare_id=bare_id)
        if kind == "channel":
            if numeric <= -BOT_API_CHANNEL_OFFSET:
                return build_peer_ref("channel", abs(numeric) - BOT_API_CHANNEL_OFFSET, raw)
            return build_peer_ref("channel", numeric, raw)
        if kind == "chat":
            return build_peer_ref("chat", numeric, raw)
        if kind == "user":
            if numeric <= 0:
                raise ValueError("user id must be positive")
            return build_peer_ref("user", numeric, raw)
        raise ValueError("unsupported peer reference")

    numeric = _parse_int(raw)
    if numeric > CHAT_TYPE_MASK:
        peer_type, bare_id = unpack_peer_id(numeric)
        return PeerRef(raw=raw, peer_id=int(numeric), peer_type=peer_type, bare_id=bare_id)
    if numeric <= -BOT_API_CHANNEL_OFFSET:
        return build_peer_ref("channel", abs(numeric) - BOT_API_CHANNEL_OFFSET, raw)
    if numeric < 0:
        return build_peer_ref("chat", abs(numeric), raw)
    return build_peer_ref(default_positive_type, numeric, raw)


def describe_peer_ref(peer: PeerRef) -> str:
    return (
        f"{peer.display} / peer_id={peer.peer_id} / "
        f"bare_id={peer.bare_id} / ref={peer.canonical_ref}"
    )
