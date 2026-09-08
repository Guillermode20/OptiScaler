#!/usr/bin/env python3
"""Grammar-lock tests for the RTv=2 reprojection telemetry grammar.

telemetry_plan.md owns the grammar; these fixtures keep the analyzer and any
future emitter honest. If one of these fails, either the log format drifted
(update the version + fixtures together, deliberately) or the emitter is
writing lines the analyzer cannot parse (fix the emitter).
"""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_telemetry as at


# Canonical lines from telemetry_plan.md section 1.2 / 1.3 / 1.4 — do not edit
# without bumping the grammar version and updating the plan together.
CANONICAL_EVENTS = [
    "ReprojEv v=2 ts=1042.3 ev=nps gap=25.1 age=29.4 q=2",
    "ReprojEv v=2 ts=1043.1 ev=gapSrc gap=33.2 ema=16.9 q=3",
    "ReprojEv v=2 ts=1101.7 ev=late interval=11.4 age=20.2 capWait=1 q=1",
    "ReprojEv v=2 ts=1120.0 ev=sess state=stop",
]
CANONICAL_HIT = "ReprojHit v=2 ts=1045.2 dur=3.4 N=14 ev=nps maxGap=33.2 age=31.0 q=2"
CANONICAL_META = "ReprojMeta v=2 ts=12.0 refresh=119.95 stage=1 ver=10.0.1.42 commit=abc1234 depthCap=0 iso=1"
CANONICAL_1HZ = (
    "Reproj: RTv=2 ts=1043.0 src=59.9 disp=119.6 new=59 rep=61 miss=1 drop=0 notRdy=2 cap=120 "
    "blk=0.42 age=12.1 step=0.9/1.6 gas=2.3 q=1 p50=8.33 p95=9.17 low1=113.9 evt=3 eMiss=2 eSrc=1 eOut=0"
)


def write_log(tmpdir: Path, name: str, lines: list[str]) -> Path:
    path = tmpdir / name
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def run_analyzer(mode: str, *paths: Path) -> tuple[int, str]:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = at.main(["analyze_telemetry.py", mode, *(str(p) for p in paths)])
    return code, buf.getvalue()


class ParseTests(unittest.TestCase):
    def test_parses_every_canonical_plan_example(self):
        for line in CANONICAL_EVENTS + [CANONICAL_HIT]:
            parsed = at.parse_line(line)
            self.assertIsNotNone(parsed, line)
            self.assertGreater(parsed.ts, 0.0, line)

    def test_meta_record_roundtrips_identity_keys(self):
        import tempfile
        path = Path(tempfile.mkdtemp()) / "meta.log"
        path.write_text(CANONICAL_META + "\n", encoding="utf-8")
        meta = at.parse_meta(path)
        self.assertEqual(meta["refresh"], "119.95")
        self.assertEqual(meta["stage"], "1")
        self.assertEqual(meta["ver"], "10.0.1.42")
        self.assertEqual(meta["commit"], "abc1234")
        self.assertEqual(meta["depthCap"], "0")

    def test_event_line_fields_roundtrip(self):
        parsed = at.parse_line(CANONICAL_EVENTS[0])
        self.assertEqual(parsed.ev, "nps")
        self.assertEqual(parsed.ts, 1042.3)
        self.assertEqual(parsed.kv["gap"], "25.1")
        self.assertEqual(parsed.kv["age"], "29.4")
        self.assertEqual(parsed.kv["q"], "2")
        self.assertNotIn("v", parsed.kv)  # version consumed, not a field

    def test_hit_line_carries_class_and_count(self):
        parsed = at.parse_line(CANONICAL_HIT)
        self.assertEqual(parsed.ev, "nps")
        self.assertEqual(parsed.kv["N"], "14")
        self.assertEqual(parsed.kv["dur"], "3.4")

    def test_non_telemetry_lines_return_none(self):
        self.assertIsNone(at.parse_line("Reproj: activated (async virtual swapchain)"))
        self.assertIsNone(at.parse_line("[2026-09-05 12:00:00.000] [info] some prose"))
        self.assertIsNone(at.parse_line(""))

    def test_unknown_version_fails_loudly(self):
        with self.assertRaises(at.GrammarError):
            at.parse_line("ReprojEv v=1 ts=1.0 ev=nps gap=1.0")
        with self.assertRaises(at.GrammarError):
            at.parse_line("ReprojEv v=3 ts=1.0 ev=nps gap=1.0")

    def test_unknown_event_class_fails_loudly(self):
        with self.assertRaises(at.GrammarError):
            at.parse_line("ReprojEv v=2 ts=1.0 ev=mystery gap=1.0")

    def test_missing_ts_fails_loudly(self):
        with self.assertRaises(at.GrammarError):
            at.parse_line("ReprojEv v=2 ev=nps gap=1.0")


class EpisodeTests(unittest.TestCase):
    def test_burst_merges_into_one_episode(self):
        events = [
            at.parse_line(f"ReprojEv v=2 ts={1000.0 + i * 0.1} ev=nps gap={20 + i}.0 age=30.0 q=2")
            for i in range(5)
        ]
        episodes = at.build_episodes(events)
        self.assertEqual(len(episodes), 1)
        ep = episodes[0]
        self.assertEqual(ep.cls, "nps")
        self.assertEqual(ep.count, 5)
        self.assertEqual(ep.max_num("gap"), 24.0)

    def test_isolated_event_is_its_own_episode(self):
        events = [at.parse_line(l) for l in [
            "ReprojEv v=2 ts=100.0 ev=nps gap=20.0 age=30.0 q=2",
            "ReprojEv v=2 ts=200.0 ev=nps gap=21.0 age=31.0 q=2",
        ]]
        self.assertEqual(len(at.build_episodes(events)), 2)

    def test_attribution_prefers_source_overlap(self):
        events = [at.parse_line(l) for l in [
            "ReprojEv v=2 ts=100.0 ev=nps gap=20.0 age=30.0 q=3",
            "ReprojEv v=2 ts=100.2 ev=gapSrc gap=40.0 ema=16.7 q=3",
        ]]
        episodes = at.build_episodes(events)
        at.attribute(episodes, events)
        self.assertIn("source-bound", episodes[0].verdict)

    def test_attribution_queue_pressure_before_stale_anchor(self):
        events = [at.parse_line(l) for l in [
            "ReprojEv v=2 ts=100.0 ev=nps gap=20.0 age=30.0 q=2",
        ]]
        episodes = at.build_episodes(events)
        at.attribute(episodes, events)
        self.assertIn("pressure", episodes[0].verdict)

    def test_attribution_stale_anchor_with_empty_queue(self):
        events = [at.parse_line(l) for l in [
            "ReprojEv v=2 ts=100.0 ev=late interval=11.0 age=41.0 capWait=1 q=0",
        ]]
        episodes = at.build_episodes(events)
        at.attribute(episodes, events)
        self.assertIn("stale anchors", episodes[0].verdict)

    def test_attribution_falls_back_to_presenter_bound(self):
        events = [at.parse_line(l) for l in [
            "ReprojEv v=2 ts=100.0 ev=late interval=11.0 age=10.0 capWait=0 q=0",
        ]]
        episodes = at.build_episodes(events)
        at.attribute(episodes, events)
        self.assertIn("presenter-bound", episodes[0].verdict)


class FileModeTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_story_on_canonical_log(self):
        log = write_log(self.tmp, "kcd2.log", [
            "some boot prose",
            CANONICAL_META,
            CANONICAL_EVENTS[3].replace("state=stop", "state=start"),
            CANONICAL_EVENTS[0],
            CANONICAL_EVENTS[1],
            CANONICAL_EVENTS[2],
            CANONICAL_1HZ,
            "more unrelated prose",
        ])
        code, out = run_analyzer("story", log)
        self.assertEqual(code, 0)
        self.assertIn("session None -> start", out)
        self.assertIn("[    nps]", out)
        self.assertIn("[ gapSrc]", out)
        self.assertIn("verdict: source-bound", out)
        self.assertIn("low1 fps: mean 113.9", out)
        self.assertIn("missed slots: total 1", out)

    def test_health_clean_log(self):
        log = write_log(self.tmp, "clean.log", [
            CANONICAL_META,
            CANONICAL_EVENTS[3].replace("state=stop", "state=start"),
            CANONICAL_1HZ.replace("miss=1", "miss=0").replace("low1=113.9", "low1=119.2"),
        ])
        code, out = run_analyzer("health", log)
        self.assertEqual(code, 0)
        self.assertIn("episodes: 0", out)
        self.assertIn("low1 fps: mean 119.2", out)

    def test_diff_reports_deltas_and_meta(self):
        before = write_log(self.tmp, "before.log", [
            CANONICAL_META.replace("ver=10.0.1.42", "ver=10.0.1.41"),
            CANONICAL_1HZ.replace("ts=1043.0", "ts=100.0").replace("disp=119.6", "disp=104.0").replace("miss=1", "miss=24"),
        ])
        after = write_log(self.tmp, "after.log", [CANONICAL_META, CANONICAL_1HZ.replace("ts=1043.0", "ts=100.0")])
        code, out = run_analyzer("diff", before, after)
        self.assertEqual(code, 0)
        self.assertIn("disp:", out)
        self.assertIn("miss:", out)
        self.assertIn("ver: 10.0.1.41 -> 10.0.1.42", out)

    def test_grammar_violation_exits_2(self):
        log = write_log(self.tmp, "bad.log", ["ReprojEv v=9 ts=1.0 ev=nps gap=1.0"])
        buf = io.StringIO()
        with contextlib.redirect_stderr(buf):
            code = at.main(["analyze_telemetry.py", "story", str(log)])
        self.assertEqual(code, 2)
        self.assertIn("grammar violation", buf.getvalue())

    def test_no_telemetry_exits_1(self):
        log = write_log(self.tmp, "empty.log", ["totally unrelated game log"])
        code, _ = run_analyzer("story", log)
        self.assertEqual(code, 1)


class WindowTests(unittest.TestCase):
    def test_1hz_line_parses_all_documented_keys(self):
        path = Path(self.tmpdir() ) / "w.log"
        path.write_text(CANONICAL_1HZ + "\n", encoding="utf-8")
        windows = at.parse_windows(path)
        self.assertEqual(len(windows), 1)
        w = windows[0]
        self.assertEqual(w.src, 59.9)
        self.assertEqual(w.disp, 119.6)
        self.assertEqual(w.miss, 1.0)
        self.assertEqual(w.notrdy, 2.0)
        self.assertEqual(w.low1, 113.9)
        self.assertEqual(w.p95, 9.17)

    def test_old_1hz_line_without_rtv_is_ignored_not_fatal(self):
        path = Path(self.tmpdir()) / "w2.log"
        path.write_text("Reproj: source=60.0 FPS display=120.0 FPS (new=60 repeat=60) missed=0\n", encoding="utf-8")
        self.assertEqual(at.parse_windows(path), [])

    def test_legacy_v1_line_from_real_log_parses(self):
        # Verbatim shape from a live KCD2 session (be90584, 2026-09-05 18:52).
        line = ("[18:52:27.994347] [I] AReproj_Dx12::LogMetricsIfDue Reproj: source=58.9 FPS "
                "display=112.9 FPS (new=58 repeat=55) missed=7 interval=8.63/10.20ms "
                "latchLead=3.00ms poseAge=37.9ms queue=3 late=113/113 maxDeg=1.20 dropAnchor=0 "
                "capC=59 capWait=55 (async virtual swapchain, block=2.44ms)")
        path = Path(self.tmpdir()) / "v1.log"
        path.write_text(line + "\n", encoding="utf-8")
        windows = at.parse_windows(path)
        self.assertEqual(len(windows), 1)
        w = windows[0]
        self.assertEqual(w.ts, 18 * 3600 + 52 * 60 + 27.994347)
        self.assertEqual(w.src, 58.9)
        self.assertEqual(w.disp, 112.9)
        self.assertEqual(w.miss, 7.0)
        self.assertEqual(w.age, 37.9)
        self.assertEqual(w.notrdy, 55.0)
        self.assertEqual(w.blk, 2.44)
        self.assertEqual(w.q, 3.0)
        # high capWait + sub-110 disp + misses -> suspect with a capture-latency hint
        why = at._window_suspect(w)
        self.assertIsNotNone(why)
        self.assertIn("capWait", why)

    def tmpdir(self):
        import tempfile
        return tempfile.mkdtemp()


if __name__ == "__main__":
    unittest.main()
