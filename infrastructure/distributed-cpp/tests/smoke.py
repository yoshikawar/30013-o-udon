"""Loopback integration check for the standalone bridge; no official API access."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys


bridge, application = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])


def scenario(name, worker_overrides, expected, wait_ms="5000", budget_ms="1000"):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    base = dict(os.environ, CLUSTER_SECRET="local-smoke-test-secret", PORT=str(port),
                APP_EXEC=application, EXPECTED_WORKERS="2", STARTUP_WAIT_MS=wait_ms,
                JOB_TIMEOUT_MS=budget_ms, MAIN_HOST="127.0.0.1")
    processes = []
    try:
        main = subprocess.Popen([bridge], env=dict(base, ROLE="main", NODE_ID="main", SEED="101"),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(main)
        for index, override in enumerate(worker_overrides):
            worker_env = dict(base, ROLE="worker", NODE_ID=f"worker-{index}", SEED=str(202 + index))
            worker_env.update(override)
            processes.append(subprocess.Popen([bridge], env=worker_env,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        _, stderr = main.communicate(timeout=12)
        assert main.returncode == 0, (name, main.returncode, stderr)
        events = [json.loads(line.removeprefix("remote event: ")) for line in stderr.splitlines()
                  if line.startswith("remote event: ")]
        assert events[-1] == {"type": "done", "jobId": "1"}, (name, events)
        assert all(event['jobId'] == '1' for event in events)
        results = sorted((event['result'] for event in events if event['type'] == 'worker_done'),
                         key=lambda item: item['nodeId'])
        assert [item["status"] for item in results] == expected, (name, results, stderr)
        assert len({item["nodeId"] for item in results}) == len(results)
        for item in results:
            assert item["seed"] in {"202", "203"}
        candidates = [event for event in events if event['type'] == 'candidate']
        assert all(event['result']['status'] == 'unvalidated' for event in candidates)
        if name == 'two workers and per-PC seeds':
            assert len(candidates) == 4, events
            first_done = next(i for i, event in enumerate(events) if event['type'] == 'worker_done')
            assert any(event['type'] == 'candidate' for event in events[:first_done])
        print(f"PASS {name}")
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise AssertionError(f"{name}: process failed to stop")


scenario("two workers and per-PC seeds", [{}, {}], ["ok", "ok"])
scenario("failed solver isolated", [{"APP_EXEC": "/usr/bin/false"}, {}], ["error", "ok"])
scenario("wrong secret rejected", [{"CLUSTER_SECRET": "different-smoke-secret"}], [], wait_ms="500")
scenario("no workers", [], [], wait_ms="0")
if len(sys.argv) > 3:
    scenario("slow solver bounded; other worker continues",
             [{"APP_EXEC": str(Path(sys.argv[3]).resolve())}, {}], ["unavailable", "ok"])
