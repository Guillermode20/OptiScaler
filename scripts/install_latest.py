#!/usr/bin/env python3
"""
Install latest OptiScaler artifact to test games.

Wraps the AGENTS.md Building → Installing flow:
  gh api repos/<owner>/<repo>/actions/runs?per_page=… → find latest successful
  gh api repos/.../artifacts/<id>/zip > build.7z  (archive:false → raw .7z)
  7z x build.7z
  copy OptiScaler.dll → dxgi.dll + OptiScaler/ + Licenses/ into game folders

Defaults match AGENTS.md:
  DRG:  /var/home/whick/.local/share/Steam/steamapps/common/Deep Rock Galactic/FSD/Binaries/Win64/
  KCD2: /var/home/whick/.local/share/Steam/steamapps/common/KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO/

Usage:
  python scripts/install_latest.py --both                          # DRG+KCD2, current branch async-timewarp
  python scripts/install_latest.py --drg --ref master
  python scripts/install_latest.py --kcd2 --ref my-feature --owner myfork
  python scripts/install_latest.py --both --run-id 32769762279     # explicit run
  python scripts/install_latest.py --both --dry-run

Requires: gh (logged in with repo+workflow), 7z, python3.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_DRD = Path("/var/home/whick/.local/share/Steam/steamapps/common/Deep Rock Galactic/FSD/Binaries/Win64")
DEFAULT_KCD2 = Path("/var/home/whick/.local/share/Steam/steamapps/common/KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO")
REPO = "Guillermode20/OptiScaler"  # fork default; pass --repo to override
BRANCH = "async-timewarp"
WORKFLOW = "Build (No Signing)"

def run(cmd, **kw):
    print(f"$ {' '.join(cmd)}")
    return subprocess.run(cmd, check=True, **kw)

def gh_api(path: str):
    """Return a GitHub API response, preserving gh's useful failure detail."""
    cmd = ["gh", "api", path]
    print(f"$ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no error output"
        raise RuntimeError(f"GitHub API request failed ({result.returncode}): {detail}")
    return json.loads(result.stdout) if result.stdout.strip().startswith(("{", "[")) else result.stdout

def find_latest_run(repo: str, branch: str, workflow: str = WORKFLOW) -> dict:
    # List workflows to get id
    data = gh_api(f"repos/{repo}/actions/workflows")
    wid = None
    for w in data.get("workflows", []):
        if w["name"] == workflow:
            wid = w["id"]
            break
    if wid is None:
        raise RuntimeError(f"Workflow '{workflow}' not found in {repo}")
    # runs for that workflow+branch, most recent first
    rdata = gh_api(f"repos/{repo}/actions/workflows/{wid}/runs?branch={branch}&per_page=5")
    for r in rdata.get("workflow_runs", []):
        if r["status"] == "completed" and r["conclusion"] == "success":
            return r
    raise RuntimeError(f"No successful runs for {workflow}@{branch}")

def download_artifact(repo: str, run_id: int, out_path: Path):
    data = gh_api(f"repos/{repo}/actions/runs/{run_id}/artifacts")
    if not data.get("artifacts"):
        raise RuntimeError(f"No artifacts for run {run_id}")
    art = data["artifacts"][0]
    aid = art["id"]
    name = art["name"]
    print(f"[artifact] {name} id={aid} size={art['size_in_bytes']}")
    # gh api repos/.../artifacts/<id>/zip > file (raw .7z because archive:false)
    with open(out_path, "wb") as f:
        subprocess.run(["gh", "api", f"repos/{repo}/actions/artifacts/{aid}/zip"], stdout=f, check=True)
    print(f"[download] -> {out_path} ({out_path.stat().st_size} bytes)")
    return name

def install_to_game(src_dir: Path, game_dir: Path, dry_run: bool = False):
    dll_src = src_dir / "OptiScaler.dll"
    if not dll_src.exists():
        raise RuntimeError(f"Missing {dll_src} (did 7z extract fail?)")
    print(f"[install] {game_dir}  (exists={game_dir.exists()})")
    if not game_dir.exists():
        print(f"[warn] Game dir not found, skipping {game_dir}", file=sys.stderr)
        return
    if dry_run:
        print(f"  would backup {game_dir/'dxgi.dll'} and copy {dll_src} -> {game_dir/'dxgi.dll'}")
        print(f"  would copy {src_dir/'OptiScaler'} -> {game_dir/'OptiScaler'}")
        print(f"  would copy {src_dir/'Licenses'} -> {game_dir/'Licenses'}")
        return
    # backup
    dxgi = game_dir / "dxgi.dll"
    if dxgi.exists():
        backup = game_dir / f"dxgi.dll.bak-auto-{int(time.time())}"
        shutil.copy2(dxgi, backup)
        print(f"  backup {dxgi} -> {backup}")
    shutil.copy2(dll_src, dxgi)
    # also keep OptiScaler.dll copy for diagnostics
    shutil.copy2(dll_src, game_dir / "OptiScaler.dll")
    for sub in ["OptiScaler", "Licenses"]:
        src = src_dir / sub
        dst = game_dir / sub
        if src.exists():
            if dst.exists():
                shutil.rmtree(dst, ignore_errors=True) if dst.is_dir() and sub == "Licenses" else None
            # copytree with dirs_exist_ok
            shutil.copytree(src, dst, dirs_exist_ok=True)
            print(f"  copied {src} -> {dst}")
    # patch INI telemetry on if requested? Leave INI untouched by default; user can use bump_version --changelog logic
    print(f"  installed {dll_src.name} -> {dxgi}  ({dxgi.stat().st_size} bytes)")

def main():
    ap = argparse.ArgumentParser(description="Install latest OptiScaler build to DRG/KCD2")
    ap.add_argument("--drg", action="store_true", help="install to DRG")
    ap.add_argument("--kcd2", action="store_true", help="install to KCD2")
    ap.add_argument("--both", action="store_true", help="install to both (default if no --drg/--kcd2)")
    ap.add_argument("--drg-dir", type=Path, default=DEFAULT_DRD, help=f"DRG dir (default {DEFAULT_DRD})")
    ap.add_argument("--kcd2-dir", type=Path, default=DEFAULT_KCD2, help=f"KCD2 dir (default {DEFAULT_KCD2})")
    ap.add_argument("--repo", default=REPO, help=f"repo owner/name (default {REPO})")
    ap.add_argument("--ref", default=BRANCH, help=f"branch (default {BRANCH})")
    ap.add_argument("--run-id", type=int, help="explicit run id (skip search)")
    ap.add_argument("--dry-run", action="store_true", help="don't write, just print")
    ap.add_argument("--workflow", default=WORKFLOW, help=f"workflow name (default '{WORKFLOW}')")
    args = ap.parse_args()

    if not args.drg and not args.kcd2 and not args.both:
        args.both = True
    targets = []
    if args.both or args.drg:
        targets.append(("DRG", args.drg_dir))
    if args.both or args.kcd2:
        targets.append(("KCD2", args.kcd2_dir))

    run_id = args.run_id
    if run_id is None:
        print(f"[search] latest successful '{args.workflow}' on {args.repo}@{args.ref} ...")
        run = find_latest_run(args.repo, args.ref, args.workflow)
        run_id = run["id"]
        print(f"[found] run {run_id} {run['html_url']} conclusion={run['conclusion']}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        zpath = tmp / "build.7z"
        name = download_artifact(args.repo, run_id, zpath)
        extract = tmp / "extract"
        extract.mkdir()
        print(f"[extract] 7z x {zpath} -o{extract}")
        subprocess.run(["7z", "x", str(zpath), f"-o{extract}"], check=True)
        # artifact 7z contains top-level files directly; or sometimes nested
        # find OptiScaler.dll
        candidates = list(extract.rglob("OptiScaler.dll"))
        src_dir = candidates[0].parent if candidates else extract
        # if extract contains single subfolder, use it
        if not (src_dir / "OptiScaler.dll").exists():
            # fallback to extract itself
            src_dir = extract
        print(f"[src] {src_dir} contains: {', '.join(p.name for p in src_dir.iterdir())}")
        for label, gdir in targets:
            install_to_game(src_dir, gdir, dry_run=args.dry_run)
    print("[done] Launch games with WINEDLLOVERRIDES=dxgi=n,b %COMMAND%")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
