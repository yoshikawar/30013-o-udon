#!/usr/bin/env python3
"""Run the arena client once per advertised lobby (macOS/Linux)."""
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "run" / "arena-repeat"
BASE_URL = "https://procon37arena.online"
POLL_SECONDS = 60
child = None


def log(message):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), message, flush=True)


def notify_failure(match, departure, result):
    """Notify Discord once without exposing the webhook or raw client logs."""
    marker = match / "discord-failure.sent"
    if marker.exists():
        return
    webhook = (RUNTIME / "discord-webhook.secret").read_text().strip()
    if not webhook.startswith("https://discord.com/api/webhooks/"):
        raise ValueError("Invalid Discord webhook configuration")
    payload = {
        "content": (
            "定期参加が異常終了し、安全のため停止しました。\n"
            f"出発時刻: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(departure))}\n"
            f"終了コード: {result}\n"
            "STOPPEDを確認し、公式状態とローカルのSession/ログを照合してください。"
        ),
        "allowed_mentions": {"parse": ["everyone"]},
    }
    config = "url = " + json.dumps(webhook + "?wait=true") + "\n"
    config += 'header = "Content-Type: application/json"\n'
    config += "data = " + json.dumps(json.dumps(payload, ensure_ascii=False), ensure_ascii=False) + "\n"
    response = subprocess.run([
        "curl", "--silent", "--show-error", "--max-time", "20",
        "--config", "-", "--output", "/dev/null", "--write-out", "%{http_code}",
    ], input=config, text=True, capture_output=True)
    if response.returncode != 0 or not response.stdout.isdigit():
        raise RuntimeError("Discord transport failed")
    status = int(response.stdout)
    if not 200 <= status < 300:
        raise RuntimeError(f"Discord HTTP {status}")
    marker.write_text(time.strftime("%Y-%m-%dT%H:%M:%S%z") + "\n")
    log(f"Discord failure notification sent for match {departure}")


def stop(signum, frame):
    if child is not None and child.poll() is None:
        child.send_signal(signal.SIGTERM)
        child.wait()
    raise SystemExit(0)


def main():
    global child
    os.umask(0o077)
    # Deliberately never fall back to the official venue credential.
    os.environ.pop("PROCON_TOKEN", None)
    if not os.environ.get("token"):
        raise SystemExit("Environment variable token is not set.")
    RUNTIME.mkdir(parents=True, exist_ok=True)
    with (RUNTIME / "scheduler.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit("Arena scheduler is already running.")
        (RUNTIME / "scheduler.pid").write_text(str(os.getpid()) + "\n")
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        if (RUNTIME / "STOPPED").exists():
            raise SystemExit("Inspect the failed session and STOPPED before restarting.")
        log(f"Scheduler started; credential=token; checking lobby schedule every {POLL_SECONDS} seconds.")
        previous = None
        while True:
            try:
                response = subprocess.run([
                    "curl", "--fail", "--silent", "--show-error",
                    "--connect-timeout", "5", "--max-time", "15",
                    BASE_URL + "/api/next",
                ], capture_output=True, check=True)
                schedule = json.loads(response.stdout)
                departure = int(schedule["departure"])
                opens = int(schedule["lobby_opens_at"])
            except Exception as error:
                # Do not print exceptions that might contain request data.
                log("Schedule unavailable: " + type(error).__name__)
                time.sleep(POLL_SECONDS)
                continue
            if previous != departure:
                log("Next lobby=" + time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(opens)))
                previous = departure
            match = RUNTIME / str(departure)
            if not schedule.get("paused_until") and opens <= time.time() < departure and not match.exists():
                match.mkdir(mode=0o700)
                log(f"Starting match {departure}; session/log: {match}")
                with (match / "client-output.log").open("w") as output:
                    child = subprocess.Popen([
                        str(ROOT / "build" / "hexa_udon"), "auto",
                        "--base-url", BASE_URL, "--token-env", "token",
                        "--max-get-retries", "200", "--execute",
                        "--session-dir", str(match / "session"),
                        "--log-dir", str(match / "log"),
                    ], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
                    result = child.wait()
                    child = None
                (match / "exit-code").write_text(str(result) + "\n")
                log(f"Match {departure} exited with code {result}")
                if result != 0:
                    (RUNTIME / "STOPPED").write_text(f"Inspect {match}; exit={result}\n")
                    try:
                        notify_failure(match, departure, result)
                    except Exception as error:
                        log("Discord failure notification failed: " + type(error).__name__)
                    raise SystemExit("Stopped for inspection; no automatic POST retry.")
            time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
