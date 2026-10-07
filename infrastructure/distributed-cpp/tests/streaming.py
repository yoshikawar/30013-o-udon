"""Streaming regression tests. Loopback only; no competition API or credentials."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys

bridge, fixture = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
for mode in ('stream', 'backpressure', 'failure'):
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    base = dict(os.environ, APP_EXEC=fixture, TEST_MODE=mode,
                CLUSTER_SECRET='local-streaming-test-secret', PORT=str(port),
                MAIN_HOST='127.0.0.1', EXPECTED_WORKERS='2', STARTUP_WAIT_MS='5000',
                JOB_TIMEOUT_MS='5000', APP_READ_TIMEOUT_MS='10000', SEED='1')
    processes = []
    try:
        for role, node in [('main', 'main'), ('worker', 'worker-0'), ('worker', 'worker-1')]:
            processes.append(subprocess.Popen([bridge], env=dict(base, ROLE=role, NODE_ID=node),
                                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        _, stderr = processes[0].communicate(timeout=20)
        assert processes[0].returncode == 0, stderr
        events = [(int(parts[1]), json.loads(parts[2]))
                  for line in stderr.splitlines() if line.startswith('EVENT ')
                  for parts in [line.split(' ', 2)]]
        assert {event['jobId'] for _, event in events} == {'1', '2'}, events
        for job in ('1', '2'):
            batch = [(ms, event) for ms, event in events if event['jobId'] == job]
            assert batch[-1][1]['type'] == 'done', batch
            candidates = [(ms, event['result']) for ms, event in batch if event['type'] == 'candidate']
            terminal = [event['result'] for _, event in batch if event['type'] == 'worker_done']
            assert len(terminal) == 2 and len({r['nodeId'] for r in terminal}) == 2
            assert all(r['status'] == ('error' if mode == 'failure' else 'ok') for r in terminal), terminal
            assert all(r['status'] == 'unvalidated' for _, r in candidates)
            if mode == 'stream':
                assert len(candidates) == 4, batch
                assert candidates[0][0] < 1000, candidates  # before either solver exits
                assert batch[-1][0] >= 2500, batch
                assert {r['payload'] for _, r in candidates} == {f'task-{job}-first', f'task-{job}-better'}
            elif mode == 'backpressure':
                assert len(candidates) == 8, len(candidates)
                assert all(r['payload'] == '\x01' * 8192 for _, r in candidates)
            else:
                assert len(candidates) == 2, batch  # exit failure never certifies earlier candidates
        print(f'PASS {mode}: two jobs, early delivery / bounded slow reader / failure isolation')
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
                raise AssertionError('bridge failed to stop')
