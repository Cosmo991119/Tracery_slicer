#!/usr/bin/env python3
"""Compile and check opt-in trace structure; no machine-specific time limits."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "libslic3r/TraceryPerformance.hpp"
#include <stdexcept>
using Slic3r::TraceryPerformanceScope;
int main() {
    TraceryPerformanceScope stroke("stroke");
    stroke.count("input_facets", 42);
    {
        TraceryPerformanceScope child("partition");
        child.stage("initialize");
        child.stage("loop_total");
        for (int i = 0; i < 3; ++i) {
            TraceryPerformanceScope::Sample sample(child, "loop_work");
        }
        child.finish();
        child.finish(); // Explicit finish plus destruction must not duplicate.
    }
    try {
        TraceryPerformanceScope failing("failing");
        failing.stage("throwing_stage");
        throw std::runtime_error("expected test exception");
    } catch (const std::exception&) {
        TraceryPerformanceScope dialog("error_dialog_wait");
    }
}
'''


def parse(text):
    return [dict(token.split("=", 1) for token in line.split()[1:])
            for line in text.splitlines() if line.startswith("TRACERY_UI_PERF ")]


def main():
    with tempfile.TemporaryDirectory(prefix="tracery-trace-test-") as directory:
        source = Path(directory) / "trace.cpp"
        binary = Path(directory) / "trace"
        source.write_text(SOURCE)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), str(source), "-o", str(binary)], check=True)
        env = os.environ.copy()
        for setting in (None, "0", "true"):
            env.pop("TRACERY_PERF_TRACE", None)
            if setting is not None:
                env["TRACERY_PERF_TRACE"] = setting
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True)
            assert not result.stderr, result.stderr
        env["TRACERY_PERF_TRACE"] = "1"
        result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True)
        records = parse(result.stderr)
        totals = {r["phase"]: r for r in records if r.get("kind") == "total"}
        assert len([r for r in records if r.get("kind") == "total"]) == 4
        assert totals["stroke"]["status"] == "failed"
        assert totals["partition"]["status"] == "completed"
        assert totals["failing"]["status"] == "exception"
        assert totals["error_dialog_wait"]["status"] == "completed"
        root_id = totals["stroke"]["scope_id"]
        assert totals["stroke"]["parent_id"] == "0"
        assert all(r["parent_id"] == root_id for name, r in totals.items() if name != "stroke")
        work = next(r for r in records if r.get("phase") == "loop_work")
        assert work["calls"] == "3" and work["scope_id"] == totals["partition"]["scope_id"]
        assert work["parent_phase"] == "loop_total"
        thrown = next(r for r in records if r.get("phase") == "throwing_stage")
        assert thrown["status"] == "exception"
        assert any(r.get("metric") == "input_facets" and r["value"] == "42" for r in records)
        assert all(float(r["elapsed_ms"]) >= 0 for r in records if "elapsed_ms" in r)
        print("PASS: disabled trace, hierarchy, aggregation, metrics, idempotent finish, exception and dialog separation")


if __name__ == "__main__":
    main()
