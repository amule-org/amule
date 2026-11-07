#!/usr/bin/env python3
"""Report bundled dependencies that have a newer upstream release.

A newer version is not always a security fix, so this only reports. Read the linked
release notes, then bump packaging/linux/versions.env if a release fixes a security bug.
Exits non-zero only when it cannot do the check.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

VERSIONS_ENV = Path(__file__).resolve().parents[1] / "packaging/linux/versions.env"

DOTTED = r"(\d+(?:\.\d+)+)"

# versions.env prefix -> GitHub repository and the tag format of its releases.
PINS = {
    "WX": ("wxWidgets/wxWidgets", "v" + DOTTED),
    "CRYPTOPP": ("weidai11/cryptopp", r"CRYPTOPP_(\d+(?:_\d+)+)"),
    "LIBUPNP": ("pupnp/pupnp", "release-" + DOTTED),
    "APPINDICATOR": ("AyatanaIndicators/libayatana-appindicator", DOTTED),
    "LIBGD": ("libgd/libgd", "gd-" + DOTTED),
    "BOOST": ("boostorg/boost", "boost-" + DOTTED),
}

# Built only by CI jobs, so no bundle ships them.
NOT_BUNDLED = {"NGTCP2", "NGTCP2_LATEST"}


def version_key(version):
    return tuple(int(part) for part in re.findall(r"\d+", version))


def read_pins(text):
    """Return {prefix: version}; fail on a tarball pin this script doesn't know."""
    versions = dict(re.findall(r"^([A-Z0-9_]+)_VERSION=(\S+)$", text, re.M))
    tarballs = set(re.findall(r"^([A-Z0-9_]+)_TARBALL_URL=", text, re.M))
    unknown = sorted(tarballs - PINS.keys() - NOT_BUNDLED)
    if unknown:
        raise ValueError(f"add to PINS or NOT_BUNDLED: {', '.join(unknown)}")
    missing = sorted(PINS.keys() - versions.keys())
    if missing:
        raise ValueError(f"not found in versions.env: {', '.join(missing)}")
    return {name: versions[name] for name in PINS}


def newest_release(tags, tag_format):
    """Return the highest release version; the format rejects rc and beta tags."""
    matches = (re.fullmatch(tag_format, tag) for tag in tags)
    versions = [match.group(1) for match in matches if match]
    return max(versions, key=version_key).replace("_", ".") if versions else None


def remote_tags(repo):
    result = subprocess.run(
        ["git", "ls-remote", "--tags", "--refs", f"https://github.com/{repo}"],
        capture_output=True,
        text=True,
        timeout=120,
        check=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )
    return [line.rpartition("refs/tags/")[2] for line in result.stdout.splitlines()]


def check(pins, fetch_tags):
    """Return ([(name, pinned, newest, notes URL)], [error])."""
    newer, errors = [], []
    for name, pinned in pins.items():
        repo, tag_format = PINS[name]
        try:
            newest = newest_release(fetch_tags(repo), tag_format)
        except (OSError, subprocess.SubprocessError) as error:
            errors.append(f"{name}: {error}")
            continue
        if newest is None:
            errors.append(f"{name}: no release tags found in {repo}")
        elif version_key(newest) > version_key(pinned):
            newer.append((name, pinned, newest, f"https://github.com/{repo}/releases"))
    return newer, errors


def report(pins, newer, errors):
    print(f"{len(newer)} of {len(pins)} pins have a newer upstream release.")
    summary = ["## Dependency pins", ""]
    if newer:
        summary += ["| Dependency | Pinned | Newest | Release notes |", "|---|---|---|---|"]
    for name, pinned, newest, notes in newer:
        print(f"::notice::{name} is pinned to {pinned}, upstream has {newest}: {notes}")
        summary.append(f"| {name} | {pinned} | {newest} | {notes} |")
    if not newer:
        summary.append("All pins match the newest upstream release.")
    for error in errors:
        print(f"::warning::{error}")
        summary += ["", f"Check failed for {error}"]
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as output:
            output.write("\n".join(summary) + "\n")


def main():
    try:
        pins = read_pins(VERSIONS_ENV.read_text(encoding="utf-8"))
    except ValueError as error:
        print(f"::error::{error}")
        return 1
    newer, errors = check(pins, remote_tags)
    report(pins, newer, errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
