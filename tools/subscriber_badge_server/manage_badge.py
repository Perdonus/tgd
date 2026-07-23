#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sys

from peer_ids import describe_peer_ref, parse_peer_ref
from store import db_conn, delete_badge, get_badge, list_badges, upsert_badge


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Manage Astrogram subscriber badges by peer reference."
    )
    parser.add_argument(
        "--db",
        default=os.environ.get("ASTRO_BADGE_DB", "./badge.db"),
        help="Path to SQLite database. Defaults to ASTRO_BADGE_DB or ./badge.db.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add_peer_argument(p: argparse.ArgumentParser) -> None:
        p.add_argument(
            "peer_ref",
            help=(
                "Peer reference. Examples: 6603471853, -1003814280064, "
                "channel:3814280064, chat:123456789, peer:562949953421312"
            ),
        )

    grant = sub.add_parser("grant", help="Grant or update a badge.")
    add_peer_argument(grant)
    grant.add_argument(
        "--emoji-status-id",
        type=int,
        default=int(os.environ.get("ASTRO_BADGE_EMOJI_ID", "0")),
        help="Emoji status document id. Defaults to ASTRO_BADGE_EMOJI_ID or 0.",
    )

    revoke = sub.add_parser("revoke", help="Revoke a badge.")
    add_peer_argument(revoke)

    check = sub.add_parser("check", help="Check one badge record.")
    add_peer_argument(check)

    normalize = sub.add_parser("normalize", help="Show normalized peer reference.")
    add_peer_argument(normalize)

    list_cmd = sub.add_parser("list", help="List recent badge records.")
    list_cmd.add_argument("--limit", type=int, default=20)

    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.command == "normalize":
        peer = parse_peer_ref(args.peer_ref)
        print(describe_peer_ref(peer))
        return 0

    conn = db_conn(args.db)
    try:
        if args.command == "grant":
            peer = parse_peer_ref(args.peer_ref)
            upsert_badge(conn, peer, int(args.emoji_status_id))
            print(
                f"granted: {describe_peer_ref(peer)} / emoji_status_id={int(args.emoji_status_id)}"
            )
            return 0

        if args.command == "revoke":
            peer = parse_peer_ref(args.peer_ref)
            delete_badge(conn, peer)
            print(f"revoked: {describe_peer_ref(peer)}")
            return 0

        if args.command == "check":
            peer = parse_peer_ref(args.peer_ref)
            row = get_badge(conn, peer)
            if not row:
                print(f"not found: {describe_peer_ref(peer)}")
                return 1
            print(
                f"found: {describe_peer_ref(peer)} / "
                f"emoji_status_id={int(row['emoji_status_id'])} / "
                f"updated_at={int(row['updated_at'])}"
            )
            return 0

        if args.command == "list":
            for row in list_badges(conn, args.limit):
                peer = parse_peer_ref(f"{row['peer_type']}:{int(row['bare_id'])}")
                print(
                    f"{describe_peer_ref(peer)} / "
                    f"emoji_status_id={int(row['emoji_status_id'])} / "
                    f"updated_at={int(row['updated_at'])}"
                )
            return 0

        raise RuntimeError(f"unsupported command: {args.command}")
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
