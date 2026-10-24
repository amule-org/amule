#!/usr/bin/env python3
"""Flatpak permission contract, including validation by flatpak build-finish when available."""
import configparser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
MANIFESTS = [ROOT / "packaging/flathub/org.amule.aMule.yaml",
             ROOT / "packaging/linux/flatpak/org.amule.aMule.yaml.in"]


def finish_args(manifest):
    block = manifest.read_text().split("finish-args:\n", 1)[1].split("\ncleanup:", 1)[0]
    return [line.strip()[2:] for line in block.splitlines() if line.strip().startswith("- ")]


class Permissions(unittest.TestCase):
    def test_manifests_agree(self):
        self.assertEqual(finish_args(MANIFESTS[0]), finish_args(MANIFESTS[1]))

    def test_only_legacy_profile_is_exposed_by_default(self):
        for manifest in MANIFESTS:
            args = finish_args(manifest)
            self.assertEqual([arg for arg in args if arg.startswith("--filesystem=")],
                             ["--filesystem=~/.aMule:create"])
            self.assertNotIn("--talk-name=org.freedesktop.Flatpak", args)
            self.assertNotIn("--socket=session-bus", args)

    def test_flatpak_accepts_permissions_and_preserves_profile_path(self):
        flatpak = os.environ.get("FLATPAK_CLI") or shutil.which("flatpak")
        if not flatpak:
            self.skipTest("flatpak CLI is unavailable")
        with tempfile.TemporaryDirectory(prefix="amule-flatpak-permissions-") as directory:
            build = Path(directory)
            (build / "files").mkdir()
            (build / "metadata").write_text("[Application]\nname=org.amule.aMule\n"
                "runtime=org.gnome.Platform/x86_64/49\nsdk=org.gnome.Sdk/x86_64/49\n")
            subprocess.run([flatpak, "build-finish", *finish_args(MANIFESTS[0]), directory],
                check=True, capture_output=True, text=True)
            metadata = configparser.ConfigParser(interpolation=None)
            metadata.read(build / "metadata")
            self.assertEqual(metadata["Context"]["filesystems"], "~/.aMule:create;")


if __name__ == "__main__":
    unittest.main()
