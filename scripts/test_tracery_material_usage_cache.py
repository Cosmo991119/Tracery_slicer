#!/usr/bin/env python3
"""Use a Debug test binary to verify queries avoid re-parsing after a commit cache expires."""
import os
import subprocess
import sys


def main():
    env = dict(os.environ, TRACERY_PERF_TRACE="1")
    result = subprocess.run([sys.argv[1], "[TraceryMaterialUsageCache]"], env=env,
                            text=True, capture_output=True, check=True)
    records = [dict(token.split("=", 1) for token in line.split()[1:])
               for line in result.stderr.splitlines() if line.startswith("TRACERY_UI_PERF ")]
    assert records, "Timing verification requires a Debug test binary; Release intentionally disables tracing"
    totals = {r["scope_id"]: r for r in records if r.get("kind") == "total"}
    repeated = {i for i, r in totals.items() if r["phase"] == "material_usage_repeat_pass"}
    assert repeated, "The repeated material query scenario did not run"
    descendants = set(repeated)
    while True:
        children = {i for i, r in totals.items() if r["parent_id"] in descendants}
        if children <= descendants:
            break
        descendants.update(children)
    decodes = [r for r in records if r.get("phase") == "document_cache.decode"
               and r["scope_id"] in descendants]
    assert not decodes, f"Unchanged large material queries still parsed documents {len(decodes)} times"
    print(result.stdout.strip())
    print("PASS: repeated material queries perform zero document decodes after the commit scope ends")


if __name__ == "__main__":
    main()
