#!/usr/bin/env python3
"""
Reprojection Telemetry Analyzer
Parses OptiScaler.log for ReprojTelemetry v=1 lines and ReprojSlot dumps.
See telemetry_plan.md section 16 & 21.
"""

import re
import sys
import argparse
from collections import defaultdict, Counter
from pathlib import Path
import statistics

TELEMETRY_RE = re.compile(
    r"ReprojTelemetry v=1 slots=(?P<slots>\d+) presented=(?P<presented>\d+) missed=(?P<missed>\d+) legacyMissed=(?P<legacy>\d+) "
    r"newAnchor=(?P<newAnchor>\d+) repeatAnchor=(?P<repeat>\d+) skippedRep=(?P<skipped>\d+) "
    r"cause\.cpu=(?P<ccpu>\d+) cause\.wait=(?P<cwait>\d+) cause\.capture=(?P<ccap>\d+) cause\.queue=(?P<cqueue>\d+) cause\.gpu=(?P<cgpu>\d+) cause\.present=(?P<cpresent>\d+) cause\.clock=(?P<cclock>\d+) cause\.unknown=(?P<cunknown>\d+) "
    r"interval\.p50=(?P<ip50>[-\d\.NaN]+) interval\.p95=(?P<ip95>[-\d\.NaN]+) interval\.p99=(?P<ip99>[-\d\.NaN]+) interval\.max=(?P<imax>[-\d\.NaN]+) "
    r"wake\.p50=(?P<wp50>[-\d\.NaN]+) wake\.p95=(?P<wp95>[-\d\.NaN]+) wake\.p99=(?P<wp99>[-\d\.NaN]+) wake\.max=(?P<wmax>[-\d\.NaN]+) lateWakes=(?P<lateWakes>\d+) "
    r"wait\.p50=(?P<waitp50>[-\d\.NaN]+) wait\.p95=(?P<waitp95>[-\d\.NaN]+) wait\.p99=(?P<waitp99>[-\d\.NaN]+) "
    r"queue\.p50=(?P<qp50>[-\d\.NaN]+) queue\.p95=(?P<qp95>[-\d\.NaN]+) queue\.p99=(?P<qp99>[-\d\.NaN]+) queue\.max=(?P<qmax>[-\d\.NaN]+) "
    r"gpu\.p50=(?P<gp50>[-\d\.NaN]+) gpu\.p95=(?P<gp95>[-\d\.NaN]+) gpu\.p99=(?P<gp99>[-\d\.NaN]+) gpu\.max=(?P<gmax>[-\d\.NaN]+) gpuMargin\.p50=(?P<gm50>[-\d\.NaN]+) gpuSkipped=(?P<gskipped>\d+) calibFail=(?P<calibFail>\d+) calibValid=(?P<calibValid>\d) "
    r"present\.p50=(?P<pp50>[-\d\.NaN]+) present\.p95=(?P<pp95>[-\d\.NaN]+) present\.p99=(?P<pp99>[-\d\.NaN]+) present\.max=(?P<pmax>[-\d\.NaN]+) "
    r"mode\.mv=(?P<mmv>\d+) mode\.depth=(?P<mdepth>\d+) mode\.rotation=(?P<mrot>\d+) mode\.unwarped=(?P<munwarp>\d+) "
    r"source\.raw\.p50=(?P<sr50>[-\d\.NaN]+) source\.raw\.p95=(?P<sr95>[-\d\.NaN]+) source\.selected\.p50=(?P<ss50>[-\d\.NaN]+) source\.selected\.p95=(?P<ss95>[-\d\.NaN]+) ratio\.p50=(?P<rp50>[-\d\.NaN]+) ratio\.p95=(?P<rp95>[-\d\.NaN]+) "
    r"anchorAge\.p50=(?P<ap50>[-\d\.NaN]+) anchorAge\.p95=(?P<ap95>[-\d\.NaN]+) anchorAge\.max=(?P<amax>[-\d\.NaN]+) "
    r"step\.raw\.p50=(?P<stepRaw50>[-\d\.NaN]+) step\.raw\.p95=(?P<stepRaw95>[-\d\.NaN]+) step\.raw\.max=(?P<stepRawMax>[-\d\.NaN]+) step\.final\.p50=(?P<stepF50>[-\d\.NaN]+) step\.final\.p95=(?P<stepF95>[-\d\.NaN]+) step\.final\.max=(?P<stepFMax>[-\d\.NaN]+) step\.clamped=(?P<clamped>\d+) "
    r"camera=(?P<camAvail>\d+)/(?P<camTotal>\d+) depth=(?P<depthAvail>\d+)/(?P<depthTotal>\d+) depthConstants=(?P<dcAvail>\d+)/(?P<dcTotal>\d+) hudless=(?P<hAvail>\d+)/(?P<hTotal>\d+) "
    r"fps=(?P<fps>[-\d\.NaN]+)"
)

SLOT_RE = re.compile(
    r"ReprojSlot v=1 seq=(?P<seq>\d+) outcome=(?P<outcome>\d+) cause=(?P<cause>\d+) secondary=(?P<sec>[0-9a-fA-FxX]+) anchor=(?P<anchor>\d+) new=(?P<new>\d+) repeat=(?P<repeat>\d+) effMode=(?P<mode>\d+) wake=(?P<wake>[-\d\.A-Za-z]+) wait=(?P<wait>[-\d\.A-Za-z]+) queue=(?P<queue>[-\d\.A-Za-z]+) gpu=(?P<gpu>[-\d\.A-Za-z]+) present=(?P<present>[-\d\.A-Za-z]+) interval=(?P<interval>[-\d\.A-Za-z]+) age=(?P<age>[-\d\.A-Za-z]+) step=(?P<stepUnc>[-\d\.A-Za-z]+)/(?P<stepFinal>[-\d\.A-Za-z]+) vel=(?P<vel>\d+) depth=(?P<depth>\d+) cam=(?P<cam>\d+)"
)

def parse_float(s):
    try:
        v = float(s)
        if v != v:  # NaN
            return None
        return v
    except:
        return None

def load_log(path):
    telemetry = []
    slots = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = TELEMETRY_RE.search(line)
            if m:
                d = m.groupdict()
                # convert numeric
                for k in d:
                    if d[k] is None:
                        continue
                    try:
                        if "." in d[k] or "NaN" in d[k] or "nan" in d[k]:
                            d[k] = parse_float(d[k])
                        else:
                            d[k] = int(d[k])
                    except:
                        pass
                telemetry.append(d)
                continue
            m2 = SLOT_RE.search(line)
            if m2:
                d = m2.groupdict()
                for k in d:
                    try:
                        if "." in d[k] or "NaN" in d[k] or "nan" in d[k].lower():
                            d[k] = parse_float(d[k])
                        else:
                            if k == "sec":
                                d[k] = int(d[k], 0)
                            else:
                                d[k] = int(d[k])
                    except:
                        pass
                slots.append(d)
    return telemetry, slots

def percentile(data, p):
    if not data:
        return None
    s = sorted(data)
    k = int(round(p * (len(s)-1)))
    return s[k]

def analyze(path):
    telemetry, slots = load_log(path)
    print(f"Time range: {len(telemetry)} telemetry windows")
    if not telemetry:
        print("No telemetry found. Is Reproj.Telemetry=true?")
        if slots:
            print(f"Found {len(slots)} slot dumps without telemetry windows")
        return

    # Display rate distribution
    fps_vals = [t["fps"] for t in telemetry if isinstance(t["fps"], (int,float)) and t["fps"] is not None]
    print(f"Display FPS: p50 {percentile(fps_vals,0.5):.1f} p95 {percentile(fps_vals,0.95):.1f} max {max(fps_vals):.1f}" if fps_vals else "No FPS data")

    # Misses by cause
    cause_totals = Counter()
    for t in telemetry:
        cause_totals["cpu"] += t.get("ccpu",0) or 0
        cause_totals["wait"] += t.get("cwait",0) or 0
        cause_totals["capture"] += t.get("ccap",0) or 0
        cause_totals["queue"] += t.get("cqueue",0) or 0
        cause_totals["gpu"] += t.get("cgpu",0) or 0
        cause_totals["present"] += t.get("cpresent",0) or 0
        cause_totals["clock"] += t.get("cclock",0) or 0
        cause_totals["unknown"] += t.get("cunknown",0) or 0
    print("Misses by cause:", dict(cause_totals))
    total_missed = sum(cause_totals.values())
    total_sched = sum(t.get("slots",0) for t in telemetry)
    print(f"Total scheduled {total_sched} missed {total_missed} ({(total_missed/total_sched*100 if total_sched else 0):.1f}%)")

    # Interval percentiles
    ip50 = [t["ip50"] for t in telemetry if t.get("ip50") is not None]
    ip95 = [t["ip95"] for t in telemetry if t.get("ip95") is not None]
    print(f"Present interval p50: {percentile(ip50,0.5):.2f} p95: {percentile(ip95,0.95):.2f}" if ip50 else "No interval")

    # Queue and warp
    qp95 = [t["qp95"] for t in telemetry if t.get("qp95") is not None]
    gp95 = [t["gp95"] for t in telemetry if t.get("gp95") is not None]
    print(f"Queue p95: {percentile(qp95,0.5):.2f} Warp p95: {percentile(gp95,0.5):.2f}" if qp95 else "No queue/gpu")

    # Source rate
    sr50 = [t["sr50"] for t in telemetry if t.get("sr50") is not None]
    print(f"Source raw p50: {percentile(sr50,0.5):.1f} ms ({(1000/percentile(sr50,0.5) if percentile(sr50,0.5) else 0):.1f} FPS)" if sr50 else "No source")

    # Timestep
    stepF95 = [t["stepF95"] for t in telemetry if t.get("stepF95") is not None]
    print(f"Timestep final p95: {percentile(stepF95,0.95):.2f}" if stepF95 else "No timestep")
    clamped = sum(t.get("clamped",0) for t in telemetry)
    print(f"Clamped steps total: {clamped}")

    # Effective path
    mv = sum(t.get("mmv",0) for t in telemetry)
    depth = sum(t.get("mdepth",0) for t in telemetry)
    rot = sum(t.get("mrot",0) for t in telemetry)
    print(f"Effective path: MV {mv} depth {depth} rotation {rot}")

    # Correlation: queue delay vs missed
    # Simple: compute avg queue p95 for windows with misses vs without
    with_miss = [t["qp95"] for t in telemetry if t.get("missed",0)>0 and t.get("qp95") is not None]
    without = [t["qp95"] for t in telemetry if t.get("missed",0)==0 and t.get("qp95") is not None]
    if with_miss and without:
        print(f"Queue p95 with miss {statistics.mean(with_miss):.2f} without {statistics.mean(without):.2f} delta {statistics.mean(with_miss)-statistics.mean(without):.2f}")

    # Correlation source interval change vs timestep error: check if high source variance correlates with step variance
    # Placeholder: compare ratio p95
    ratio = [t["rp95"] for t in telemetry if t.get("rp95") is not None]
    if ratio:
        print(f"Source selected/raw ratio p95 {percentile(ratio,0.95):.2f}")

    # Worst ten windows by missed
    worst = sorted(telemetry, key=lambda x: x.get("missed",0), reverse=True)[:10]
    print("Worst 10 windows by missed:")
    for w in worst:
        print(f"  missed={w.get('missed')} queue.p95={w.get('qp95')} gpu.p95={w.get('gp95')} interval.p95={w.get('ip95')} fps={w.get('fps')}")

    # Detailed slots if present
    if slots:
        print(f"Detailed slots: {len(slots)}")
        def _slot_interval(s):
            v = s.get("interval")
            if isinstance(v, (int, float)):
                return v
            try:
                return float(v) if v is not None else 0
            except:
                return 0
        worst_slots = sorted(slots, key=_slot_interval, reverse=True)[:10]
        print("Worst slots by interval:")
        for s in worst_slots:
            print(f"  seq={s.get('seq')} outcome={s.get('outcome')} cause={s.get('cause')} interval={s.get('interval')} queue={s.get('queue')} gpu={s.get('gpu')}")

def main():
    p = argparse.ArgumentParser(description="Analyze OptiScaler reprojection telemetry")
    p.add_argument("log", help="Path to OptiScaler.log")
    args = p.parse_args()
    analyze(args.log)

if __name__ == "__main__":
    main()
