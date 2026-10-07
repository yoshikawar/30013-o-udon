"""Check the role-fixed binaries used by the two container image targets."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys

main_bridge, worker_bridge, application = (
    str(Path(value).resolve()) for value in sys.argv[1:4]
)
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
base = dict(os.environ, APP_EXEC=application,
            CLUSTER_SECRET="fixed-role-local-test-secret", PORT=str(port),
            MAIN_HOST="127.0.0.1", EXPECTED_WORKERS="2", STARTUP_WAIT_MS="5000",
            JOB_TIMEOUT_MS="2000", APP_READ_TIMEOUT_MS="5000")
processes = []
try:
    # Deliberately invert ROLE. Compile-time fixed roles must win over environment.
    main = subprocess.Popen([main_bridge],
        env=dict(base, ROLE="worker", NODE_ID="main", SEED="101"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    processes.append(main)
    for index in range(2):
        processes.append(subprocess.Popen([worker_bridge],
            env=dict(base, ROLE="main", NODE_ID=f"worker-{index}", SEED=str(202 + index)),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
    _, stderr = main.communicate(timeout=12)
    assert main.returncode == 0, stderr
    assert stderr.count("worker connected:") == 2, stderr
    events = [json.loads(line.removeprefix("remote event: "))
              for line in stderr.splitlines() if line.startswith("remote event: ")]
    assert events[-1] == {"type": "done", "jobId": "1"}, events
    candidates = [event for event in events if event["type"] == "candidate"]
    assert {event["result"]["nodeId"] for event in candidates} == {"worker-0", "worker-1"}
    assert "fixed-role-local-test-secret" not in stderr
    print("PASS fixed main image role and two outbound-only worker roles")
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
            raise AssertionError("fixed-role process failed to stop")
