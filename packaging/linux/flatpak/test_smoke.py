#!/usr/bin/env python3
"""Offline regression checks for the GUI smoke driver's failure policy."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("smoke", Path(__file__).with_name("smoke.py"))
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class DrawingFailureTest(unittest.TestCase):
    def check_log(self, contents):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "amule.log"
            log.write_bytes(contents)
            smoke.check_drawing_failures(log)

    def test_unrelated_gtk_warning_is_allowed(self):
        self.check_log(b"Gtk-WARNING **: cannot open optional resource\nnormal shutdown\n")

    def test_empty_log_is_allowed(self):
        self.check_log(b"")

    def test_drawing_failure_fails(self):
        with self.assertRaisesRegex(RuntimeError, "drawing failure"):
            self.check_log(b"Gtk-WARNING **: drawing failure for widget 'wxPizza'\n")

    def test_only_drawing_failure_lines_are_reported(self):
        with self.assertRaises(RuntimeError) as result:
            self.check_log(b"unrelated warning\ndrawing failure: first\ndrawing failure: second\n")
        self.assertIn("first", str(result.exception))
        self.assertIn("second", str(result.exception))
        self.assertNotIn("unrelated warning", str(result.exception))

    def test_invalid_utf8_does_not_hide_failure(self):
        with self.assertRaisesRegex(RuntimeError, "drawing failure"):
            self.check_log(b"\xff\ndrawing failure: cairo\n")

    def test_missing_log_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                smoke.check_drawing_failures(Path(directory) / "missing.log")


class ProfileFixtureTest(unittest.TestCase):
    def test_uses_the_allowed_default_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            profile = smoke.prepare_profile(home, "gnome-wayland")
            self.assertEqual(profile, home / ".aMule")
            self.assertIn("Nick=Flatpak smoke profile", (profile / "amule.conf").read_text())

    def test_refuses_existing_profile_without_changing_it(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            profile = home / ".aMule"
            profile.mkdir()
            identity = profile / "preferences.dat"
            identity.write_bytes(b"existing identity")
            with self.assertRaisesRegex(RuntimeError, "empty aMule profile"):
                smoke.prepare_profile(home, "x11")
            self.assertEqual(identity.read_bytes(), b"existing identity")
            self.assertEqual(list(profile.iterdir()), [identity])

    def test_refuses_symlinked_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            target = home / "real-profile"
            target.mkdir()
            (home / ".aMule").symlink_to(target, target_is_directory=True)
            with self.assertRaisesRegex(RuntimeError, "symlinked"):
                smoke.prepare_profile(home, "x11")
            self.assertEqual(list(target.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
