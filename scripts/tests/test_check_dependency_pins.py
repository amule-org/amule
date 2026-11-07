"""Exercise check-dependency-pins.py without touching the network."""

import importlib.util
import subprocess
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check-dependency-pins.py"
SPEC = importlib.util.spec_from_file_location("check_dependency_pins", SCRIPT)
pins_check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pins_check)


class ReadPinsTest(unittest.TestCase):
    def test_every_tarball_in_versions_env_is_classified(self):
        pins = pins_check.read_pins(pins_check.VERSIONS_ENV.read_text(encoding="utf-8"))
        self.assertEqual(set(pins), set(pins_check.PINS))

    def test_unknown_tarball_pin_fails(self):
        text = pins_check.VERSIONS_ENV.read_text(encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "NEWLIB"):
            pins_check.read_pins(text + "NEWLIB_VERSION=1.0\nNEWLIB_TARBALL_URL=https://x\n")

    def test_missing_pin_fails(self):
        with self.assertRaisesRegex(ValueError, "CRYPTOPP"):
            pins_check.read_pins("WX_VERSION=3.2.12\n")


class NewestReleaseTest(unittest.TestCase):
    def test_compares_numerically_and_skips_prereleases(self):
        tags = ["v0.9.0", "v0.10.0", "v0.11.0-rc1", "v0.11.0.beta", "backup"]
        self.assertEqual(pins_check.newest_release(tags, "v" + pins_check.DOTTED), "0.10.0")

    def test_every_tag_format(self):
        samples = {
            "WX": "v3.2.12",
            "CRYPTOPP": "CRYPTOPP_8_9_0",
            "LIBUPNP": "release-22.0.4",
            "APPINDICATOR": "0.6.0",
            "LIBGD": "gd-2.3.3",
            "BOOST": "boost-1.87.0",
        }
        self.assertEqual(set(samples), set(pins_check.PINS))
        for name, tag in samples.items():
            with self.subTest(name):
                newest = pins_check.newest_release([tag, "junk"], pins_check.PINS[name][1])
                self.assertRegex(newest, r"^\d+(\.\d+)+$")


class CheckTest(unittest.TestCase):
    def test_reports_only_newer_releases(self):
        tags = {"libgd/libgd": ["gd-2.3.3", "gd-2.3.4"], "boostorg/boost": ["boost-1.87.0"]}
        newer, errors = pins_check.check({"LIBGD": "2.3.3", "BOOST": "1.87.0"}, tags.__getitem__)
        self.assertEqual(errors, [])
        self.assertEqual(newer, [("LIBGD", "2.3.3", "2.3.4", "https://github.com/libgd/libgd/releases")])

    def test_failed_lookup_keeps_checking_the_rest(self):
        def fetch(repo):
            if repo == "boostorg/boost":
                raise subprocess.TimeoutExpired("git", 120)
            return ["gd-2.3.4"]

        newer, errors = pins_check.check({"BOOST": "1.87.0", "LIBGD": "2.3.3"}, fetch)
        self.assertEqual([row[0] for row in newer], ["LIBGD"])
        self.assertEqual(len(errors), 1)
        self.assertIn("BOOST", errors[0])

    def test_no_matching_tags_is_an_error(self):
        newer, errors = pins_check.check({"BOOST": "1.87.0"}, lambda repo: ["junk"])
        self.assertEqual(newer, [])
        self.assertIn("no release tags", errors[0])


if __name__ == "__main__":
    unittest.main()
