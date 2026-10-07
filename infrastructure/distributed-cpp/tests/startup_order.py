"""Workers may start before main and reconnect without inbound worker ports."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

main_bridge, worker_bridge, application = (
    str(Path(value).resolve()) for value in sys.argv[1:4]
)
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
base = dict(os.environ, APP_EXEC=application,
            CLUSTER_SECRET="startup-order-local-secret", PORT=str(port),
            MAIN_HOST="127.0.0.1", EXPECTED_WORKERS="2", STARTUP_WAIT_MS="7000",
            JOB_TIMEOUT_MS="2000", APP_READ_TIMEOUT_MS="5000")
processes = []
try:
    for index in range(2):
        processes.append(subprocess.Popen([worker_bridge],
            env=dict(base, NODE_ID=f"early-worker-{index}", SEED=str(301 + index)),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
    time.sleep(0.5)
    main = subprocess.Popen([main_bridge],
        env=dict(base, NODE_ID="main", SEED="101"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    processes.append(main)
    _, stderr = main.communicate(timeout=12)
    assert main.returncode == 0, stderr
    assert stderr.count("worker connected:") == 2, stderr
    assert "workers=2" in stderr, stderr
    print("PASS workers started first and reconnected to late main")
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
            raise AssertionError("startup-order process failed to stop")
