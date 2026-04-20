#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


API_ACCEPT = "application/vnd.github+json"
RAW_BASE = "https://raw.githubusercontent.com/Perdonus/tgd/builds"
PAGES_BASE = "https://perdonus.github.io/tgd"


def now() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")


def log_line(log_path: Path, message: str) -> None:
    line = f"[{now()}] {message}\n"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as handle:
        handle.write(line)


def fetch_json(url: str) -> dict:
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "codex-watch-astrogram-release",
            "Accept": API_ACCEPT,
        },
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def fetch_text(url: str) -> str:
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "codex-watch-astrogram-release"},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read().decode("utf-8")


def fetch_run_state_from_html(repo: str, run_id: str) -> dict:
    html = fetch_text(f"https://github.com/{repo}/actions/runs/{run_id}")
    normalized = re.sub(r"\s+", " ", html)
    if "In progress" in normalized:
        return {"status": "in_progress", "conclusion": None}
    if "Failure" in normalized:
        return {"status": "completed", "conclusion": "failure"}
    if "Cancelled" in normalized:
        return {"status": "completed", "conclusion": "cancelled"}
    if "Success" in normalized:
        return {"status": "completed", "conclusion": "success"}
    raise RuntimeError("unable to infer run status from GitHub run page HTML")


def download(url: str, destination: Path) -> None:
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "codex-watch-astrogram-release"},
    )
    with urllib.request.urlopen(request, timeout=300) as response, destination.open("wb") as handle:
        shutil.copyfileobj(response, handle)


def wait_for_run(repo: str, run_id: str, log_path: Path, poll_seconds: int) -> dict:
    last_state: tuple[str, str | None] | None = None
    while True:
        try:
            payload = fetch_json(f"https://api.github.com/repos/{repo}/actions/runs/{run_id}")
        except urllib.error.HTTPError as error:
            if error.code != 403:
                raise
            payload = fetch_run_state_from_html(repo, run_id)
            payload["source"] = "html"
        else:
            payload["source"] = "api"
        state = (payload["status"], payload.get("conclusion"))
        if state != last_state:
            log_line(
                log_path,
                f"run={run_id} status={payload['status']} conclusion={payload.get('conclusion')} source={payload.get('source')}",
            )
            last_state = state
        if payload["status"] == "completed":
            return payload
        time.sleep(poll_seconds)


def wait_for_public_build(target_sha: str, log_path: Path, poll_seconds: int) -> tuple[dict, str]:
    last_message = None
    candidates = [
        ("builds", f"{RAW_BASE}/metadata.json"),
        ("pages", f"{PAGES_BASE}/metadata.json"),
    ]
    while True:
        for source_name, metadata_url in candidates:
            try:
                payload = json.loads(fetch_text(metadata_url))
                state = f"{source_name}:{payload.get('source_sha')}"
                if state != last_message:
                    log_line(
                        log_path,
                        f"{source_name} metadata source_sha={payload.get('source_sha')} run_id={payload.get('run_id')}",
                    )
                    last_message = state
                if payload.get("source_sha") == target_sha:
                    return payload, source_name
            except urllib.error.HTTPError as error:
                message = f"{source_name} metadata unavailable http={error.code}"
                if message != last_message:
                    log_line(log_path, message)
                    last_message = message
            except Exception as error:  # noqa: BLE001
                message = f"{source_name} metadata error={error!r}"
                if message != last_message:
                    log_line(log_path, message)
                    last_message = message
        time.sleep(poll_seconds)


def backup_existing(exe_path: Path, log_path: Path) -> None:
    if not exe_path.exists():
        return
    stamp = datetime.now().strftime("%Y-%m-%d-%H%M%S")
    backup = exe_path.with_name(f"Astrogram-{stamp}.exe")
    exe_path.rename(backup)
    log_line(log_path, f"backed up existing exe to {backup}")


def install_public_build(target_dir: Path, target_sha: str, source_name: str, log_path: Path) -> None:
    target_dir.mkdir(parents=True, exist_ok=True)
    exe_path = target_dir / "Astrogram.exe"
    metadata_path = target_dir / "metadata.json"
    source_base = RAW_BASE if source_name == "builds" else PAGES_BASE

    temp_dir = Path(tempfile.mkdtemp(prefix="astrogram-watch-"))
    try:
        temp_exe = temp_dir / "Astrogram.exe"
        temp_metadata = temp_dir / "metadata.json"

        download(f"{source_base}/Astrogram.exe", temp_exe)
        download(f"{source_base}/metadata.json", temp_metadata)

        payload = json.loads(temp_metadata.read_text(encoding="utf-8"))
        if payload.get("source_sha") != target_sha:
            raise RuntimeError(
                f"public build sha mismatch: expected {target_sha}, got {payload.get('source_sha')}"
            )

        backup_existing(exe_path, log_path)
        os.replace(temp_exe, exe_path)
        os.replace(temp_metadata, metadata_path)
        log_line(
            log_path,
            f"installed new Astrogram.exe from {source_name} for source_sha={target_sha}",
        )
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default="Perdonus/tgd")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--target-sha", required=True)
    parser.add_argument("--target-dir", default="/root/tgd/dist/astrogram-windows-release")
    parser.add_argument("--log-path", default="/root/tgd/dist/astrogram-windows-release/watch.log")
    parser.add_argument("--run-poll-seconds", type=int, default=300)
    parser.add_argument("--publish-poll-seconds", type=int, default=60)
    args = parser.parse_args()

    log_path = Path(args.log_path)
    log_line(
        log_path,
        f"watcher start repo={args.repo} run={args.run_id} target_sha={args.target_sha}",
    )

    run = wait_for_run(args.repo, args.run_id, log_path, args.run_poll_seconds)
    if run.get("conclusion") != "success":
        log_line(log_path, f"watcher stop: run failed conclusion={run.get('conclusion')}")
        return 1

    log_line(log_path, "run completed successfully, waiting for public build endpoint")
    _, source_name = wait_for_public_build(args.target_sha, log_path, args.publish_poll_seconds)
    install_public_build(Path(args.target_dir), args.target_sha, source_name, log_path)
    log_line(log_path, "watcher done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
