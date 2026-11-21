#!/usr/bin/env python3
"""Compares the .deb suites with the supported Debian and Ubuntu releases.

Prints a Markdown report of the suites to add or drop, or nothing when the
suites match. Usage: check-releases.py [--today YYYY-MM-DD]
"""

import argparse
import csv
import datetime
import io
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

DATA_URL = "https://salsa.debian.org/debian/distro-info-data/-/raw/main/{}.csv"
TAG_URL = "https://hub.docker.com/v2/namespaces/library/repositories/{}/tags/{}"
ROOT = Path(__file__).resolve().parents[3]
# Warn this long before a release, so the suite can be ready on the day.
NOTICE = datetime.timedelta(days=30)


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as resp:
        return resp.read().decode()


def parse_date(text):
    return datetime.date.fromisoformat(text) if text else None


def releases():
    """Yields (suite, image, release date, end of life) for every numbered release."""
    for distro in ("debian", "ubuntu"):
        for row in csv.DictReader(io.StringIO(fetch(DATA_URL.format(distro)))):
            version = row["version"].replace(" LTS", "")
            if not version:
                continue
            if distro == "debian":
                # Debian LTS still gets security updates, so the suite stays.
                eol = row.get("eol-lts") or row["eol"]
            else:
                eol = row["eol"]
            yield (f"{distro}{version}", f"{distro}:{version}",
                   parse_date(row["release"]), parse_date(eol))


def build_suites():
    text = (ROOT / "packaging/linux/build.sh").read_text()
    return set(re.findall(r"^\s*((?:debian|ubuntu)[0-9.]+)\)\s+image=", text, re.M))


def ci_suites():
    text = (ROOT / ".github/workflows/deb.yml").read_text()
    match = re.search(r"^\s*suite:\s*\[([^\]]*)\]", text, re.M)
    return {s.strip() for s in match.group(1).split(",")} if match else set()


def image_exists(image):
    repo, tag = image.split(":")
    try:
        with urllib.request.urlopen(TAG_URL.format(repo, tag), timeout=30):
            return True
    except urllib.error.HTTPError as err:
        if err.code == 404:
            return False
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--today", type=datetime.date.fromisoformat,
                        default=datetime.date.today())
    today = parser.parse_args().today

    built = build_suites()
    known = list(releases())
    rows = []
    for suite, image, release, eol in known:
        supported = release is not None and release <= today and (eol is None or eol > today)
        if supported and suite not in built:
            rows.append(("Add", suite, release, eol, image))
        elif not supported and suite in built:
            rows.append(("Drop", suite, release, eol, None))
        elif release is not None and today < release <= today + NOTICE:
            rows.append(("Coming", suite, release, eol, image))
    unknown = sorted(built - {r[0] for r in known})
    ci_only = sorted(ci_suites() ^ built)

    if not rows and not unknown and not ci_only:
        return

    print("<!-- deb-release-check -->")
    print("Debian and Ubuntu releases to handle in the `.deb` packaging "
          f"(checked {today}, from Debian's `distro-info-data`).")
    if rows:
        print()
        print("| Action | Suite | Release | End of life | Docker image |")
        print("|---|---|---|---|---|")
        for action, suite, release, eol, image in rows:
            if image is None:
                state = ""
            else:
                state = f"`{image}` " + ("available" if image_exists(image) else "not yet")
            print(f"| {action} | `{suite}` | {release or 'not set'} | {eol or 'not set'} | {state} |")
    if unknown:
        print()
        print("Not in `distro-info-data`: " + ", ".join(f"`{s}`" for s in unknown) + ".")
    if ci_only:
        print()
        print("In only one of `build.sh` and `deb.yml`: " + ", ".join(f"`{s}`" for s in ci_only) + ".")
    print()
    print("See \"Suites\" in `packaging/linux/deb/README.md` for the files to change.")


if __name__ == "__main__":
    sys.exit(main())
