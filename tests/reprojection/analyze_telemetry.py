#!/usr/bin/env python3
"""Analyze OptiScaler reprojection telemetry (RTv=2 / RDv=2 / RHv=2 grammar).

Grammar owner: telemetry_plan.md  -- this tool is the contract enforcer.

Usage:
    python3 analyze_telemetry.py story  <OptiScaler.log>          # chronological narrative + verdicts
    python3 analyze_telemetry.py health <OptiScaler.log>          # session-wide verdict only
    python3 analyze_telemetry.py diff   <before.log> <after.log>  # A/B comparison

Accepts logs with the RTv=2 sparse-event grammar (telemetry_plan.md) AND logs from
current builds that only emit the legacy 1 Hz `Reproj:` line — the latter yields
cadence stats and heuristic suspect windows, but no episode verdicts (episodes need
the v2 emitter).

Exit codes: 0 = parsed, 1 = no telemetry found / empty verdict, 2 = grammar violation.
"""

from __future__ import annotations

import math
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

TELEMETRY_VERSION = 2  # fail loudly if the log uses a different grammar version

# ReprojMeta keys that identify a session for diff alignment.
META_IDENTITY_KEYS = ("refresh", "stage", "ver", "commit")

# thresholds mirrored from telemetry_plan.md section 4 (analysis-side only)
STALE_ANCHOR_AGE_MS = 25.0
PRESSURE_QUEUE_DEPTH = 2

EVENT_CLASSES = ("nps", "gapSrc", "late", "outlier", "blk", "sess")
EPISODE_CLASSES = ("nps", "gapSrc", "late", "outlier", "blk")


class GrammarError(Exception):
    pass


# --------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------

_KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?[\w./:+-]*)")


@dataclass
class ParsedLine:
    ts: float
    ev: str
    kv: dict
    kind: str = "ev"  # "ev" = raw ReprojEv event, "hit" = ReprojHit rollup (derived)


def _parse_kv_pairs(payload: str) -> dict:
    out = {}
    for match in _KV_RE.finditer(payload):
        key, value = match.group(1), match.group(2)
        out[key] = value
    return out


def _to_float(raw: str, where: str) -> float:
    try:
        return float(raw)
    except ValueError:
        raise GrammarError(f"{where}: non-numeric value {raw!r}") from None


def parse_line(line: str) -> ParsedLine | None:
    """Parse one telemetry line; returns None for non-telemetry lines."""
    stripped = line.strip()
    if "ReprojEv v=" in stripped:
        prefix, kind = "ReprojEv v=", "ev"
    elif "ReprojHit v=" in stripped:
        prefix, kind = "ReprojHit v=", "hit"
    else:
        return None
    payload = stripped.split(prefix, 1)[1]
    if not payload.startswith(f"{TELEMETRY_VERSION} "):
        raise GrammarError(
            f"unsupported telemetry version in: {stripped[:100]!r} "
            f"(expected v={TELEMETRY_VERSION})"
        )
    kv = _parse_kv_pairs(payload)
    # the version number itself was consumed as a keyless token; drop strays
    kv.pop(TELEMETRY_VERSION, None)
    where = f"{prefix}line"
    if "ts" not in kv:
        raise GrammarError(f"{where}: missing ts: {stripped[:100]!r}")
    ts = _to_float(kv.pop("ts"), where)
    ev = kv.pop("ev", "" if prefix.startswith("ReprojHit") else "")
    if prefix.startswith("ReprojEv") and ev not in EVENT_CLASSES:
        raise GrammarError(f"{where}: unknown ev={ev!r} in: {stripped[:100]!r}")
    if prefix.startswith("ReprojHit") and ev not in EPISODE_CLASSES:
        raise GrammarError(f"{where}: unknown episode ev={ev!r} in: {stripped[:100]!r}")
    return ParsedLine(ts=ts, ev=ev, kv=kv, kind=kind)


def parse_log(path: Path) -> list[ParsedLine]:
    lines: list[ParsedLine] = []
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            parsed = parse_line(raw)
        except GrammarError:
            raise
        if parsed is not None:
            lines.append(parsed)
    return lines


def parse_meta(path: Path) -> dict:
    """Return the first ReprojMeta v=2 record as a dict ({} if absent)."""
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if "ReprojMeta v=" not in raw:
            continue
        payload = raw.split("ReprojMeta v=", 1)[1]
        if not payload.startswith(f"{TELEMETRY_VERSION} "):
            raise GrammarError(f"unsupported ReprojMeta version in: {raw[:100]!r}")
        return _parse_kv_pairs(payload)
    return {}


# --------------------------------------------------------------------------
# episode reconstruction
# --------------------------------------------------------------------------

EPISODE_TRIGGER = 3          # same-class events within this window roll up
EPISODE_WINDOW_S = 1.0
EPISODE_MERGE_GAP_S = 1.0    # same-class events within this gap merge into one episode


@dataclass
class Episode:
    cls: str
    first_ts: float
    last_ts: float
    events: list = field(default_factory=list)

    @property
    def duration_s(self) -> float:
        return round(self.last_ts - self.first_ts + 1.0, 1)

    @property
    def count(self) -> int:
        return len(self.events)

    def max_num(self, key: str) -> float | None:
        vals = [float(e.kv[key]) for e in self.events if key in e.kv]
        return max(vals) if vals else None

    def mean_num(self, key: str) -> float | None:
        vals = [float(e.kv[key]) for e in self.events if key in e.kv]
        return sum(vals) / len(vals) if vals else None


def build_episodes(events: Sequence[ParsedLine]) -> list[Episode]:
    """Reconstruct episodes from ReprojEv lines.

    The DLL emits ReprojHit rollups too; the analyzer re-derives episodes from
    raw events only (kind == "ev") so rollup lines never double-count, and it
    works on logs from builds without rollup support.
    """
    episodes: list[Episode] = []
    by_class: dict[str, list[ParsedLine]] = {c: [] for c in EPISODE_CLASSES}
    for e in events:
        if e.kind == "ev" and e.ev in by_class:
            by_class[e.ev].append(e)
    for cls, evs in by_class.items():
        current: Episode | None = None
        for e in evs:
            if current is None or e.ts - current.last_ts > EPISODE_MERGE_GAP_S:
                current = Episode(cls=cls, first_ts=e.ts, last_ts=e.ts)
                episodes.append(current)
            current.last_ts = max(current.last_ts, e.ts)
            current.events.append(e)
    episodes.sort(key=lambda ep: ep.first_ts)
    return episodes


def attribute(episodes: Sequence[Episode], events: Sequence[ParsedLine]) -> None:
    """Attach a verdict to each episode (mutates .verdict), precedence per plan §3.3."""
    for ep in episodes:
        overlap = [
            e
            for e in events
            if ep.first_ts - EPISODE_MERGE_GAP_S <= e.ts <= ep.last_ts + EPISODE_MERGE_GAP_S
        ]
        overlap_src = [e for e in overlap if e.ev == "gapSrc"]
        overlap_blk = [e for e in overlap if e.ev == "blk"]
        q_vals = [float(e.kv.get("q", 0)) for e in ep.events]
        age_mean = ep.mean_num("age")
        q_max = max(q_vals) if q_vals else 0
        if ep.cls == "gapSrc":
            ep.verdict = (
                f"source-bound: the game published frames late "
                f"(gap {ep.max_num('gap'):.1f} ms vs EMA {ep.mean_num('ema'):.1f} ms)"
            )
        elif overlap_src:
            ep.verdict = (
                f"source-bound: {len(overlap_src)} source gap(s) overlap "
                f"(worst gap {max(float(e.kv.get('gap', 0)) for e in overlap_src):.1f} ms)"
            )
        elif q_max >= PRESSURE_QUEUE_DEPTH:
            ep.verdict = (
                f"capture/packet pressure: queue depth reached {q_max:.0f} "
                f"(packets backed up while the presenter needed one)"
            )
        elif age_mean is not None and age_mean > STALE_ANCHOR_AGE_MS:
            ep.verdict = (
                f"stale anchors: mean anchor age {age_mean:.1f} ms > "
                f"{STALE_ANCHOR_AGE_MS:.0f} ms with queue empty "
                f"(capture latency, not starvation)"
            )
        elif overlap_blk:
            ep.verdict = (
                f"game-thread block: {len(overlap_blk)} block event(s) overlap "
                f"(worst {max(float(e.kv.get('blk', 0)) for e in overlap_blk):.1f} ms)"
            )
        else:
            ep.verdict = "presenter-bound (no source gap, no queue pressure, no block evidence)"


# --------------------------------------------------------------------------
# 1 Hz window extraction (from the RTv=2 health line)
# --------------------------------------------------------------------------

@dataclass
class Window:
    ts: float
    src: float | None = None
    disp: float | None = None
    miss: float | None = None
    drop: float | None = None
    notrdy: float | None = None
    age: float | None = None
    low1: float | None = None
    p95: float | None = None
    blk: float | None = None
    q: float | None = None


# The documented legacy 1 Hz line (AGENTS.md "Reading the once-per-second log line",
# current emitter). Field names there are normative; this regex is allowed to grow
# with them. It exists so real logs from builds without the RTv=2 emitter stay
# auditable and A/B-diffable.
_V1_WINDOW_RE = re.compile(
    r"^\[(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2}(?:\.\d+)?)\].*?"
    r"Reproj: source=(?P<src>[\d.]+) FPS display=(?P<disp>[\d.]+) FPS "
    r"\(new=(?P<new>\d+) repeat=(?P<rep>\d+)\) missed=(?P<miss>\d+) "
    r"interval=(?P<intMean>[\d.]+)/(?P<intP95>[\d.]+)ms .*?"
    r"poseAge=(?P<age>[\d.]+)ms queue=(?P<q>\d+) .*?"
    r"dropAnchor=(?P<drop>\d+) capC=(?P<cap>\d+) capWait=(?P<notRdy>\d+) "
    r"\((?P<presenter>[^,]+), block=(?P<blk>[\d.]+)ms\)"
)


def parse_windows(path: Path) -> list[Window]:
    """Extract the once-per-second health line.

    Accepts both the planned RTv=2 line and the legacy v1 line (old format is
    recognized by `source=... FPS display=...`, never version-gated). Prose
    lines are skipped; a *mismatched RTv* is a grammar violation.
    """
    out: list[Window] = []
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if "Reproj:" not in raw:
            continue
        if "RTv=" in raw:
            payload = raw.split("Reproj:", 1)[1]
            kv = _parse_kv_pairs(payload)
            if kv.get("RTv") != str(TELEMETRY_VERSION):
                raise GrammarError(
                    f"unsupported RTv={kv.get('RTv')!r} in: {raw[:100]!r}"
                )
            w = Window(ts=_to_float(kv.get("ts", "0"), "Reproj: ts"))
            for key in ("src", "disp", "miss", "drop", "notRdy", "age", "low1", "p95", "blk", "q"):
                if key in kv:
                    setattr(w, key.lower() if key != "notRdy" else "notrdy",
                            _to_float(kv[key], f"Reproj: {key}"))
            out.append(w)
            continue
        match = _V1_WINDOW_RE.match(raw.strip())
        if match is None:
            continue  # some other Reproj: shape (activation banner etc.)
        g = match.groupdict()
        ts = int(g["h"]) * 3600 + int(g["m"]) * 60 + float(g["s"])
        out.append(Window(
            ts=ts,
            src=float(g["src"]),
            disp=float(g["disp"]),
            miss=float(g["miss"]),
            drop=float(g["drop"]),
            notrdy=float(g["notRdy"]),
            age=float(g["age"]),
            p95=float(g["intP95"]),
            blk=float(g["blk"]),
            q=float(g["q"]),
        ))
    return out


# legacy-window suspicion heuristics (plan §4 thresholds; heuristic, not verdicts)
_BAD_WINDOW_MISS = 3
_BAD_WINDOW_DISP = 110.0
_BLOCK_SUSPECT_MS = 4.0
_NOTRDY_SUSPECT = 40.0


def _window_suspect(w: Window) -> str | None:
    if (w.miss or 0) < _BAD_WINDOW_MISS and (w.disp is None or w.disp >= _BAD_WINDOW_DISP):
        return None
    if (w.blk or 0) > _BLOCK_SUSPECT_MS:
        return "game-thread block suspected"
    if (w.notrdy or 0) >= _NOTRDY_SUSPECT:
        return "capture latency suspected (high capWait)"
    return "presenter cadence suspect (no block/queue evidence)"


# --------------------------------------------------------------------------
# narratives
# --------------------------------------------------------------------------

def _fmt_worst_value(ep: Episode) -> str:
    key = {"nps": "gap", "gapSrc": "gap", "late": "interval", "outlier": "interval", "blk": "blk"}.get(ep.cls)
    if key:
        val = ep.max_num(key)
        if val is not None:
            return f"worst {key} {val:.1f} ms"
    age = ep.max_num("age")
    return f"age {age:.1f} ms" if age is not None else "no numeric fields"


def _verdict(ep: Episode) -> str:
    return getattr(ep, "verdict", "unattributed")


def build_story(events: Sequence[ParsedLine], episodes: Sequence[Episode], windows: Sequence[Window]) -> str:
    out: list[str] = []
    sess_events = [e for e in events if e.ev == "sess"]
    out.append(f"=== Reproj telemetry story: {len(sess_events)} session boundary event(s) ===")
    last_state = None
    for e in sess_events:
        state = e.kv.get("state", "?")
        out.append(f"  t={e.ts:>9.1f}s  session {last_state} -> {state}")
        last_state = state

    out.append(f"--- {len(windows)} health window(s) ---")
    suspect = [(w, _window_suspect(w)) for w in windows]
    suspect = [(w, why) for w, why in suspect if why]
    if suspect:
        out.append(f"  suspect windows: {len(suspect)} (legacy 1 Hz line: heuristic, not verdicts)")
        for w, why in suspect[:10]:
            out.append(
                f"    t={w.ts:>9.1f}  src={w.src:.1f} disp={w.disp:.1f} miss={w.miss:.0f} "
                f"age={w.age:.1f} blk={w.blk:.2f} q={w.q:.0f} — {why}"
            )
        if len(suspect) > 10:
            out.append(f"    … and {len(suspect) - 10} more")
    low1s = [w.low1 for w in windows if w.low1 is not None]
    if low1s:
        out.append(
            f"  low1 fps: mean {sum(low1s) / len(low1s):.1f}, "
            f"worst {min(low1s):.1f} (1% low = mean of worst 1% of display intervals)"
        )
    else:
        out.append("  low1 fps: n/a (needs the RTv=2 emitter; not present in this log)")
    misses = [w.miss for w in windows if w.miss is not None]
    if misses:
        out.append(
            f"  missed slots: total {sum(misses):.0f}, "
            f"worst window {max(misses):.0f}, mean {sum(misses) / len(misses):.1f}/s"
        )

    out.append(f"--- {len(episodes)} episode(s) ---")
    for ep in episodes:
        out.append(
            f"  [{ep.cls:>7}] t={ep.first_ts:>9.1f}s dur={ep.duration_s:.1f}s N={ep.count} "
            f"{_fmt_worst_value(ep)}"
        )
        out.append(f"             verdict: {_verdict(ep)}")
    if not episodes:
        out.append("  (none — clean log)")
    return "\n".join(out)


def build_health(events: Sequence[ParsedLine], episodes: Sequence[Episode], windows: Sequence[Window]) -> str:
    out: list[str] = []
    active = [ep for ep in episodes if ep.cls != "sess"]
    out.append(f"episodes: {len(active)} across {len(windows)} window(s)")
    per_class = Counter(ep.cls for ep in active)
    for cls, count in sorted(per_class.items()):
        time_in = sum(ep.duration_s for ep in active if ep.cls == cls)
        out.append(f"  {cls:>7}: {count} episode(s), ~{time_in:.1f}s affected")
    if active:
        worst = max(active, key=lambda ep: ep.count)
        out.append(
            f"worst episode: [{worst.cls}] t={worst.first_ts:.1f}s N={worst.count} — {_verdict(worst)}"
        )
    low1s = [w.low1 for w in windows if w.low1 is not None]
    if low1s:
        out.append(
            f"low1 fps: mean {sum(low1s) / len(low1s):.1f}, worst {min(low1s):.1f}"
        )
    if not active and not low1s:
        out.append("(no episodes and no low1 data — nothing to report)")
    return "\n".join(out)


def diff_sessions(before_path: Path, after_path: Path) -> str:
    """A/B two logs on the v1 1 Hz line + meta fields."""
    meta_a, meta_b = parse_meta(before_path), parse_meta(after_path)
    wa, wb = parse_windows(before_path), parse_windows(after_path)

    def stats(windows: Sequence[Window]) -> dict:
        out: dict = {}
        for key in ("src", "disp", "miss", "drop", "notrdy", "low1", "p95"):
            vals = [getattr(w, key) for w in windows if getattr(w, key) is not None]
            out[key] = sum(vals) / len(vals) if vals else None
        return out

    sa, sb = stats(wa), stats(wb)
    lines = ["A/B diff (mean per 1 Hz window):"]
    keys = [k for k in sa if sa[k] is not None and sb[k] is not None]
    if not keys:
        lines.append("  (no common numeric keys — nothing to compare)")
    for key in keys:
        delta = sb[key] - sa[key]
        sign = "+" if delta >= 0 else ""
        lines.append(f"  {key:>6}: {sa[key]:>7.2f} -> {sb[key]:>7.2f}  ({sign}{delta:.2f})")
    lines.append("meta: " + " ".join(
        f"{k}: {meta_a.get(k, '?')} -> {meta_b.get(k, '?')}" for k in META_IDENTITY_KEYS
    ))
    return "\n".join(lines)


# --------------------------------------------------------------------------
# entry point
# --------------------------------------------------------------------------

def main(argv: Sequence[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    mode = argv[1]
    try:
        if mode == "story":
            events = parse_log(Path(argv[2]))
            episodes = build_episodes(events)
            attribute(episodes, events)
            windows = parse_windows(Path(argv[2]))
            print(build_story(events, episodes, windows))
            return 0 if events or windows else 1
        if mode == "health":
            events = parse_log(Path(argv[2]))
            episodes = build_episodes(events)
            attribute(episodes, events)
            windows = parse_windows(Path(argv[2]))
            print(build_health(events, episodes, windows))
            return 0 if events or windows else 1
        if mode == "diff":
            if len(argv) < 4:
                print(__doc__)
                return 2
            print(diff_sessions(Path(argv[2]), Path(argv[3])))
            return 0
        print(f"unknown mode {mode!r}", file=sys.stderr)
        print(__doc__)
        return 2
    except GrammarError as exc:
        print(f"telemetry grammar violation: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
