"""Exercise mirror failure recovery without root, network access or package changes."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


INSTALLER = Path(__file__).resolve().parents[2] / '.github/scripts/install-ubuntu-deps.sh'
MOCK = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
command = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
source = next((a.split('=', 1)[1] for a in args
               if a.startswith('Dir::Etc::sourcelist=')), None)
sources = pathlib.Path(source).read_text() if source else ''
with open(os.environ['CALL_LOG'], 'a') as f:
    f.write(json.dumps([command, args, sources]) + '\n')
case = os.environ['CASE']
primary = 'https://archive.ubuntu.com/' in sources
download = '--download-only' in args
if command == 'timeout':
    if download and (case == 'both_timeout' or (primary and case == 'slow_primary')):
        sys.exit(124)
    sys.exit(subprocess.call(args[3:]))
if 'update' in args and primary and case == 'update_failure':
    sys.exit(100)
if '--no-download' in args and case == 'install_failure':
    sys.exit(100)
'''


class UbuntuDepsTest(unittest.TestCase):
    def run_installer(self, case='success', packages=('libboost-dev', 'libgnutls28-dev')):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for name in ('apt-get', 'timeout'):
                path = directory / name
                path.write_text(MOCK)
                path.chmod(0o755)
            log = directory / 'calls.jsonl'
            result = subprocess.run(
                ['bash', str(INSTALLER), *packages], text=True, capture_output=True,
                env=dict(os.environ, PATH=str(directory) + ':' + os.environ['PATH'],
                         CASE=case, CALL_LOG=str(log)), timeout=10)
            calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result, calls

    def test_success_downloads_before_installing_without_network(self):
        result, calls = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        apt = [args for command, args, _ in calls if command == 'apt-get']
        self.assertEqual(len(apt), 3)
        self.assertIn('--error-on=any', apt[0])
        self.assertIn('--download-only', apt[1])
        self.assertIn('--no-download', apt[2])
        for args in apt[1:]:
            self.assertIn('--no-install-recommends', args)
            self.assertEqual(args[-2:], ['libboost-dev', 'libgnutls28-dev'])
        for command, args, source in calls:
            self.assertIn('Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg', source)
            self.assertIn('https://archive.ubuntu.com/ubuntu/', source)
            self.assertIn('Dir::Etc::sourceparts=-', args)
            if command == 'timeout':
                self.assertEqual(args[:3], ['--signal=INT', '--kill-after=10s',
                                           '240s' if '--download-only' in args else '90s'])
        source_path = next(a.split('=', 1)[1] for a in apt[0]
                           if a.startswith('Dir::Etc::sourcelist='))
        self.assertFalse(Path(source_path).exists(), 'temporary sources must be removed')

    def test_slow_progress_switches_mirror_before_installing(self):
        result, calls = self.run_installer('slow_primary')
        self.assertEqual(result.returncode, 0, result.stderr)
        installs = [(args, source) for command, args, source in calls
                    if command == 'apt-get' and '--no-download' in args]
        self.assertEqual(len(installs), 1)
        self.assertIn('https://azure.archive.ubuntu.com/ubuntu/', installs[0][1])
        self.assertIn('exceeded its budget', result.stdout)

    def test_index_failure_switches_mirror(self):
        result, calls = self.run_installer('update_failure')
        self.assertEqual(result.returncode, 0, result.stderr)
        downloads = [source for command, args, source in calls
                     if command == 'apt-get' and '--download-only' in args]
        self.assertEqual(len(downloads), 1)
        self.assertIn('https://azure.archive.ubuntu.com/ubuntu/', downloads[0])

    def test_both_mirrors_timeout_without_starting_dpkg(self):
        result, calls = self.run_installer('both_timeout')
        self.assertEqual(result.returncode, 1)
        self.assertFalse(any('--no-download' in args for _, args, _ in calls))
        self.assertIn('both official mirrors', result.stderr)

    def test_install_failure_is_not_retried(self):
        result, calls = self.run_installer('install_failure')
        self.assertEqual(result.returncode, 100)
        self.assertFalse(any('azure.archive.ubuntu.com' in source for _, _, source in calls))

    def test_package_arguments_preserved(self):
        packages = ('libc6:amd64', 'libboost-dev=1.83.0.1ubuntu2')
        result, calls = self.run_installer(packages=packages)
        self.assertEqual(result.returncode, 0, result.stderr)
        for command, args, _ in calls:
            if command == 'apt-get' and 'install' in args:
                self.assertEqual(args[-2:], list(packages))

    def test_no_packages_rejected(self):
        result, calls = self.run_installer(packages=())
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
