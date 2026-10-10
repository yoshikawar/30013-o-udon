#!/usr/bin/env python3
"""Notify Discord when results for locally entered arena rounds are published."""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "run" / "arena-repeat"
BASE_URL = "https://procon37arena.online"
POLL_SECONDS = 60


def log(message):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), message, flush=True)


def fetch_matches():
    response = subprocess.run([
        "curl", "--fail", "--silent", "--show-error", "--max-time", "20",
        BASE_URL + "/api/matches?limit=100&offset=0",
    ], capture_output=True, check=True)
    return json.loads(response.stdout)["matches"]


def summarize_log(local):
    """Export submission metadata only, never raw logs or credentials."""
    lines = []
    try:
        code = int((local / "exit-code").read_text().strip())
        lines.append("終了: " + ("正常" if code == 0 else f"異常（code={code}）"))
    except (OSError, ValueError):
        lines.append("終了: 未確認（実行中またはログ未作成）")
    try:
        session = json.loads((local / "session" / "session.json").read_text())
        days = {}
        for record in session.get("submissions", []):
            day = record.get("day")
            if type(day) is not int or day < 0:
                continue
            counts = days.setdefault(day, [0, 0, 0])
            outcome = record.get("submissionAttempted")
            counts[0 if outcome is True else 1 if outcome is False else 2] += 1
        for day, (accepted, rejected, unknown) in sorted(days.items()):
            lines.append(f"{day + 1}日目: 受理{accepted} / 不受理・未送信{rejected} / 結果不明{unknown}")
        if any(counts[2] for counts in days.values()):
            lines.append("停止理由: POST結果不明（RecoveryRequired）。通信の詳細原因は記録なし。")
        if not days:
            lines.append("日次提出記録なし")
    except (OSError, ValueError, TypeError, AttributeError):
        lines.append("提出ログを読み取れませんでした")
    # Keep the webhook message comfortably within Discord's content limit.
    summary = "\n".join(lines)
    if len(summary) > 1600:
        summary = summary[:1500] + "\n…省略…\n" + lines[-1][:90]
    return summary


def format_result(match):
    content = (
        "試合の結果が出ました\n"
        f"site: {BASE_URL}/arena/matches/{int(match['id'])}\n\n"
        "提出ログ（受理件数は待機プランを含む）:\n"
        + summarize_log(RUNTIME / str(int(match["departure"])))
    )
    return {"content": content, "allowed_mentions": {"parse": ["everyone"]}}


def deliver(webhook, payload):
    # Pass the secret URL through stdin, never command-line arguments or logs.
    config = "url = " + json.dumps(webhook + "?wait=true") + "\n"
    config += 'header = "Content-Type: application/json"\n'
    config += "data = " + json.dumps(json.dumps(payload, ensure_ascii=False), ensure_ascii=False) + "\n"
    result = subprocess.run([
        "curl", "--silent", "--show-error", "--max-time", "20",
        "--config", "-", "--output", "/dev/null", "--write-out", "%{http_code}",
    ], input=config, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError("Discord transport failed")
    status = int(result.stdout)
    if not 200 <= status < 300:
        raise RuntimeError(f"Discord HTTP {status}")


def main():
    os.umask(0o077)
    RUNTIME.mkdir(parents=True, exist_ok=True)
    with (RUNTIME / "notifier.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit("Notifier already running")
        (RUNTIME / "notifier.pid").write_text(str(os.getpid()) + "\n")
        log(f"Discord result notifier started; polling every {POLL_SECONDS} seconds")
        while True:
            try:
                for match in fetch_matches():
                    local = RUNTIME / str(int(match["departure"]))
                    if not local.is_dir() or not match.get("results"):
                        continue
                    marker = local / f"discord-{int(match['id'])}.sent"
                    if marker.exists():
                        continue
                    webhook = (RUNTIME / "discord-webhook.secret").read_text().strip()
                    if not webhook.startswith("https://discord.com/api/webhooks/"):
                        raise ValueError("Invalid Discord webhook configuration")
                    deliver(webhook, format_result(match))
                    marker.write_text(time.strftime("%Y-%m-%dT%H:%M:%S%z") + "\n")
                    log(f"Discord result sent: match {int(match['id'])}")
            except Exception as error:
                log("Notification check failed: " + type(error).__name__)
            time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
