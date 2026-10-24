#!/usr/bin/env python3
"""Exercise recoverable async backups and verify writing leaves the calling thread."""
import os
import subprocess
import sys

result = subprocess.run([sys.argv[1], "[TraceryBackup]"],
                        env=dict(os.environ, TRACERY_PERF_TRACE="1", TRACERY_BACKUP_BENCH_MB="64"),
                        text=True, capture_output=True, check=True)
print(result.stdout.strip())
print(result.stderr.strip())
records = [dict(token.split("=", 1) for token in line.split()[1:])
           for line in result.stderr.splitlines() if line.startswith("TRACERY_UI_PERF ")]
threads = {r["scope_id"]: r["value"] for r in records if r.get("metric") == "thread_id"}
capture = [r for r in records if r.get("phase") == "backup_snapshot_prepare" and r.get("kind") == "total"]
writes = [r for r in records if r.get("phase") == "backup_write_background" and r.get("kind") == "total"]
assert len(capture) == 8 and len(writes) == 8, "Expected backup, serialization failure, I/O failure and retry for both V2 and V3"
assert len({threads[r["scope_id"]] for r in writes}) == 1, "Backups must share the serialized worker"
assert {threads[r["scope_id"]] for r in capture}.isdisjoint({threads[r["scope_id"]] for r in writes}), \
    "File serialization/compression still runs on the snapshot caller's thread"
assert [r["status"] for r in writes] == ["completed", "failed", "failed", "completed"] * 2
print("PASS: backup writes run on a separate serialized worker; restore, failed-write preservation and retry passed")
for r in capture + writes:
    print(f'{r["phase"]}: {r["elapsed_ms"]} ms ({r["status"]})')
