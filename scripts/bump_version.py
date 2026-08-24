#!/usr/bin/env python3
"""
OptiScaler version bump helper.

Version lives in OptiScaler/resource.h as:
  #define VER_MAJOR_VERSION 10
  #define VER_MINOR_VERSION 0
  #define VER_HOTFIX_VERSION 0
  #define VER_BUILD_NUMBER 1

Workflows (build.yml, just_build*.yml, etc.) extract those four ints
via PowerShell and produce:
  OptiScaler_v10.0.0-pre1_YYYYMMDD.7z  (vMAJOR.MINOR.HOTFIX-preBUILD)

This script updates resource.h in-place and optionally stubs Changelog.md.
It is the canonical way to cut a new version on Linux/macOS without
opening Visual Studio.

Examples:
  python scripts/bump_version.py --bump-build
  python scripts/bump_version.py --bump-hotfix
  python scripts/bump_version.py --bump-minor
  python scripts/bump_version.py --bump-major
  python scripts/bump_version.py --set 10.0.1.5
  python scripts/bump_version.py --set 10.1.0.0 --changelog "Reproj telemetry phase 1"
  python scripts/bump_version.py --current            # just print
  python scripts/bump_version.py --bump-build --dry-run

Exit codes: 0 success, 1 no change, 2 error.
"""

from __future__ import annotations

import argparse
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
RESOURCE_H = REPO_ROOT / "OptiScaler" / "resource.h"
CHANGELOG = REPO_ROOT / "Changelog.md"

# Matches exactly the four defines; preserves surrounding whitespace/comments.
PATTERNS = {
    "major": re.compile(r"^(?P<prefix>\s*#define\s+VER_MAJOR_VERSION\s+)(?P<val>\d+)(?P<suffix>.*)$", re.MULTILINE),
    "minor": re.compile(r"^(?P<prefix>\s*#define\s+VER_MINOR_VERSION\s+)(?P<val>\d+)(?P<suffix>.*)$", re.MULTILINE),
    "hotfix": re.compile(r"^(?P<prefix>\s*#define\s+VER_HOTFIX_VERSION\s+)(?P<val>\d+)(?P<suffix>.*)$", re.MULTILINE),
    "build": re.compile(r"^(?P<prefix>\s*#define\s+VER_BUILD_NUMBER\s+)(?P<val>\d+)(?P<suffix>.*)$", re.MULTILINE),
}


def read_version(path: Path = RESOURCE_H) -> tuple[int, int, int, int]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    vals = {}
    for k, pat in PATTERNS.items():
        m = pat.search(text)
        if not m:
            raise RuntimeError(f"Could not find {k} define in {path}")
        vals[k] = int(m.group("val"))
    return vals["major"], vals["minor"], vals["hotfix"], vals["build"]


def write_version(
    major: int, minor: int, hotfix: int, build: int, path: Path = RESOURCE_H, dry_run: bool = False
) -> bool:
    text = path.read_text(encoding="utf-8", errors="ignore")
    orig = text
    for k, val in [("major", major), ("minor", minor), ("hotfix", hotfix), ("build", build)]:
        pat = PATTERNS[k]
        # replacement preserves prefix/suffix, only swaps int
        def repl(m, v=str(val)):
            return f"{m.group('prefix')}{v}{m.group('suffix')}"
        text, n = pat.subn(repl, text, count=1)
        if n == 0:
            raise RuntimeError(f"Failed to replace {k}")
    if text == orig:
        return False
    if not dry_run:
        path.write_text(text, encoding="utf-8")
    return True


def update_changelog(new_version: str, message: str | None, path: Path = CHANGELOG, dry_run: bool = False) -> None:
    if not path.exists():
        print(f"[warn] Changelog not found at {path}, skipping.", file=sys.stderr)
        return
    text = path.read_text(encoding="utf-8", errors="ignore")
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    header = f"## v{new_version}"
    if header in text:
        print(f"[info] Changelog already contains {header}, not duplicating.", file=sys.stderr)
        return
    entry = f"{header} ({today})\n"
    if message:
        entry += f"* {message}\n"
    else:
        entry += f"* Bumped version to v{new_version}\n"
    entry += "\n"
    # Insert after the first line "## Release and Build Change Log..."
    marker = "## Release and Build Change Log"
    idx = text.find(marker)
    if idx != -1:
        # find end of that line
        eol = text.find("\n", idx)
        if eol != -1:
            insert_at = eol + 1
            # skip one blank line if present
            if text[insert_at:insert_at+1] == "\n":
                insert_at += 1
            new_text = text[:insert_at] + entry + text[insert_at:]
        else:
            new_text = text + "\n" + entry
    else:
        new_text = entry + "\n" + text
    if not dry_run:
        path.write_text(new_text, encoding="utf-8")
    print(f"[changelog] {'would insert' if dry_run else 'inserted'} {header}")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Bump OptiScaler version in OptiScaler/resource.h")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--bump-major", action="store_true", help="major+1, minor=0, hotfix=0, build=1")
    g.add_argument("--bump-minor", action="store_true", help="minor+1, hotfix=0, build=1")
    g.add_argument("--bump-hotfix", action="store_true", help="hotfix+1, build=1")
    g.add_argument("--bump-build", action="store_true", help="build+1")
    g.add_argument("--set", metavar="M.m.h.b", help="set explicit version e.g. 10.0.1.5")
    g.add_argument("--current", action="store_true", help="print current version and exit")

    p.add_argument("--major", type=int, help="set major")
    p.add_argument("--minor", type=int, help="set minor")
    p.add_argument("--hotfix", type=int, help="set hotfix")
    p.add_argument("--build", type=int, help="set build")

    p.add_argument("--changelog", metavar="MSG", nargs="?", const="", help="also stub Changelog.md (optional message)")
    p.add_argument("--dry-run", action="store_true", help="print what would change, don't write")
    p.add_argument("--resource", type=Path, default=RESOURCE_H, help=f"path to resource.h (default {RESOURCE_H})")
    p.add_argument("--changelog-path", type=Path, default=CHANGELOG, help=f"path to Changelog (default {CHANGELOG})")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        cur_major, cur_minor, cur_hotfix, cur_build = read_version(args.resource)
    except Exception as e:
        print(f"[error] {e}", file=sys.stderr)
        return 2

    cur_str = f"{cur_major}.{cur_minor}.{cur_hotfix}.{cur_build}"
    cur_file = f"v{cur_major}.{cur_minor}.{cur_hotfix}-pre{cur_build}"
    print(f"[current] {cur_str}  ({cur_file})  in {args.resource}")

    if args.current:
        return 0

    # Determine new version
    new_major, new_minor, new_hotfix, new_build = cur_major, cur_minor, cur_hotfix, cur_build

    if args.set:
        parts = args.set.strip().split(".")
        if len(parts) != 4 or not all(p.isdigit() for p in parts):
            print(f"[error] --set expects M.m.h.b with 4 ints, got '{args.set}'", file=sys.stderr)
            return 2
        new_major, new_minor, new_hotfix, new_build = map(int, parts)
    else:
        if args.bump_major:
            new_major = cur_major + 1
            new_minor = 0
            new_hotfix = 0
            new_build = 1
        elif args.bump_minor:
            new_minor = cur_minor + 1
            new_hotfix = 0
            new_build = 1
        elif args.bump_hotfix:
            new_hotfix = cur_hotfix + 1
            new_build = 1
        elif args.bump_build:
            new_build = cur_build + 1
        else:
            # individual --major/--minor flags
            if args.major is not None:
                new_major = args.major
            if args.minor is not None:
                new_minor = args.minor
            if args.hotfix is not None:
                new_hotfix = args.hotfix
            if args.build is not None:
                new_build = args.build
            # if nothing specified, default to bump-build (most common)
            if all(v is None for v in [args.major, args.minor, args.hotfix, args.build]) and not any(
                [args.bump_major, args.bump_minor, args.bump_hotfix, args.bump_build, args.set]
            ):
                new_build = cur_build + 1

    new_str = f"{new_major}.{new_minor}.{new_hotfix}.{new_build}"
    new_file = f"v{new_major}.{new_minor}.{new_hotfix}-pre{new_build}"

    if (new_major, new_minor, new_hotfix, new_build) == (cur_major, cur_minor, cur_hotfix, cur_build):
        print("[info] No version change requested.", file=sys.stderr)
        return 1

    # Validate
    for v, name in [(new_major, "major"), (new_minor, "minor"), (new_hotfix, "hotfix"), (new_build, "build")]:
        if not (0 <= v <= 65535):
            print(f"[error] {name} {v} out of range 0..65535", file=sys.stderr)
            return 2

    print(f"[new]     {new_str}  ({new_file})")

    if args.dry_run:
        print("[dry-run] would write resource.h, not actually writing.", file=sys.stderr)
    changed = write_version(new_major, new_minor, new_hotfix, new_build, args.resource, dry_run=args.dry_run)
    if not changed:
        print("[info] resource.h unchanged.", file=sys.stderr)
        return 1

    print(f"[write] {'would update' if args.dry_run else 'updated'} {args.resource}")

    # Changelog
    if args.changelog is not None:
        msg = args.changelog if args.changelog != "" else None
        update_changelog(new_str, msg, args.changelog_path, dry_run=args.dry_run)

    # Summary for CI
    print(f"[done] {cur_str} -> {new_str}")
    if not args.dry_run:
        print(f"[hint] Commit with: git add {args.resource} && git commit -m \"Bump version to v{new_str}\"")
        if args.changelog is not None:
            print(f"[hint] Also: git add {args.changelog_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
