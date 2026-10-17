"""The helper bounds seek loops while accepting finite forward seeks."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HELPER = str(Path(sys.argv.pop(1)).resolve())


class SeekLimitTests(unittest.TestCase):
    def test_seek_loop_is_a_file_verdict(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'seek fixture')
            source.seek(0)
            result = subprocess.run([HELPER], stdin=source, capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 11, result.stderr)
            self.assertEqual(result.stdout, b'')

    def test_full_scan_loop_keeps_first_pass_codec(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'seek fixture')
            source.seek(0)
            env = dict(os.environ, AMULE_TEST_SEEK_RETRY='1')
            result = subprocess.run([HELPER, '--json'], stdin=source, env=env,
                                    capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'"codec":"aac"', result.stdout)

    def test_many_forward_seeks_are_accepted(self):
        with tempfile.TemporaryFile() as source:
            source.truncate(6 * 1024 * 1024)
            env = dict(os.environ, AMULE_TEST_FORWARD_SEEKS='1')
            result = subprocess.run([HELPER, '--json'], stdin=source, env=env,
                                    capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'"length_seconds":1800', result.stdout)

    def test_full_scan_adds_duration_without_losing_first_pass_metadata(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'seek fixture')
            source.seek(0)
            env = dict(os.environ, AMULE_TEST_RETRY_MISSING_FIELDS='1')
            result = subprocess.run([HELPER, '--json'], stdin=source, env=env,
                                    capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data['length_seconds'], 42)
            self.assertEqual(data['bitrate_kbps'], 128)
            self.assertEqual(data['codec'], 'aac')
            self.assertEqual(data['title'], 'First pass title')

    def test_full_scan_adds_tags_without_duration_or_codec(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'seek fixture')
            source.seek(0)
            env = dict(os.environ, AMULE_TEST_RETRY_TAGS_ONLY='1')
            result = subprocess.run([HELPER, '--json'], stdin=source, env=env,
                                    capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data['length_seconds'], 0)
            self.assertEqual(data['codec'], 'aac')
            self.assertEqual(data['title'], 'Full pass title')


if __name__ == '__main__':
    unittest.main()
