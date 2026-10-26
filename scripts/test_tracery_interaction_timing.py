#!/usr/bin/env python3
"""Verify deferred milestones without requiring a running GUI or timed sleeps."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "libslic3r/TraceryInteractionTiming.hpp"
#include <cassert>
#include <stdexcept>
using T = Slic3r::TraceryInteractionTiming;
int main() {
    int canvas = 0, other = 0;
    auto first = T::begin(&canvas, 101);
    { T::Activity hidden("must_not_trace_before_generation", &canvas); }
    T::background_scheduled();
    T::frame_presented(&canvas); // a visible result alone must not finish it
    T::generation_finished(first);
    T::idle(&canvas); // still awaiting the delayed background callback
    T::milestone("background_request_received_canvas");
    T::milestone("background_timer_armed_canvas");
    T::milestone("background_timer_armed_global"); // a restart must remain visible
    {
        T::Activity callback("gap_test.idle", &canvas);
        callback.stage("gap_test.toolbar");
        { T::Activity wrong_canvas("must_not_trace_other_canvas", &other); }
        { T::Activity render("gap_test.render", &canvas); render.stage("gap_test.draw"); }
    }
    T::milestone("background_timer_fired");
    auto ticket = T::background_started();
    assert(ticket == first);
    T::idle(&canvas); // background callback is still running
    T::background_finished(ticket);
    T::idle(&other); // only the originating canvas may finish the interaction
    T::idle(&canvas);
    T::idle(&canvas); // no double reporting
    { T::Activity hidden("must_not_trace_after_completion", &canvas); }
    T::milestone("must_not_trace_inactive");

    auto obsolete = T::begin(&canvas, 102);
    auto current = T::begin(&canvas, 103);
    T::generation_finished(obsolete); // stale callbacks cannot finish a new stroke
    T::background_finished(obsolete);
    T::fail(obsolete);
    T::frame_presented(&canvas);
    T::idle(&canvas);
    T::generation_finished(current);
    T::idle(&canvas);

    {
        T::Stroke stroke(&canvas, 104);
        T::fail_current(); // swallowed exception before a modal dialog
    }
    try {
        T::Stroke stroke(&canvas, 105);
        throw std::runtime_error("expected");
    } catch (...) {}
    T::frame_presented(&canvas);
    T::idle(&canvas);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="tracery-interaction-") as directory:
        source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(SOURCE)
        release_binary = Path(directory) / "test-release"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), str(source), "-o", str(release_binary)], check=True)
        release = subprocess.run([str(release_binary)], env=dict(os.environ, TRACERY_PERF_TRACE="1"),
                                 capture_output=True, text=True, check=True)
        assert not release.stderr, "Release must not emit traces even when the environment enables them"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), str(source), "-o", str(binary)], check=True)
        for enabled in ("0", "1"):
            result = subprocess.run([str(binary)], env=dict(os.environ, TRACERY_PERF_TRACE=enabled),
                                    capture_output=True, text=True, check=True)
            if enabled == "0":
                assert not result.stderr
                continue
            rows = [dict(token.split("=", 1) for token in line.split()[1:])
                    for line in result.stderr.splitlines() if line.startswith("TRACERY_INTERACTION_PERF ")]
            perf = [dict(token.split("=", 1) for token in line.split()[1:])
                    for line in result.stderr.splitlines() if line.startswith("TRACERY_UI_PERF ")]
            assert "must_not_trace" not in result.stderr
            totals = {r["phase"]: r for r in perf if r.get("kind") == "total"}
            assert set(totals) == {"gap_test.idle", "gap_test.render"}
            assert totals["gap_test.render"]["parent_id"] == totals["gap_test.idle"]["scope_id"]
            for total in totals.values():
                metrics = {r["metric"]: r["value"] for r in perf
                           if r.get("scope_id") == total["scope_id"] and "metric" in r}
                assert metrics["interaction_id"] == "1"
                assert int(metrics["interaction_start_us"]) >= 0
            ends = [r for r in rows if r["phase"] == "interaction_until_idle"]
            assert [r["status"] for r in ends] == ["completed", "superseded", "completed", "failed", "failed"]
            assert [r["stroke_scope_id"] for r in ends] == ["101", "102", "103", "104", "105"]
            first = [r["phase"] for r in rows if r["stroke_scope_id"] == "101"]
            assert first == ["stroke_started", "background_request_posted", "first_frame_presented", "stroke_finished",
                             "background_request_received_canvas", "background_timer_armed_canvas", "background_timer_armed_global",
                             "background_timer_fired", "background_started",
                             "background_finished", "interaction_until_idle"]
    print("PASS: deferred completion, scheduling milestones, nested gap activities, correct canvas, stale callbacks, superseded strokes, errors and disabled tracing")


if __name__ == "__main__":
    main()
