"""The extracted AppImage must dispatch helper invocations to the helper."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[3]


class AppRunMediaTests(unittest.TestCase):
    def test_helper_invocation_uses_helper_instead_of_gui(self):
        with tempfile.TemporaryDirectory(prefix='amule relocated AppDir ') as scratch:
            root = Path(scratch)
            shutil.copy2(SOURCE / 'packaging/linux/appimage/AppRun', root / 'AppRun')
            (root / 'AppRun').chmod(0o755)
            (root / 'usr/bin').mkdir(parents=True)
            for binary, output in (('amule-media-probe', 'helper'), ('amule', 'gui')):
                path = root / 'usr/bin' / binary
                path.write_text('#!/bin/sh\nprintf "%s %s\\n" ' + output + ' "$1"\n')
                path.chmod(0o755)
            env = dict(os.environ, ARGV0='/relocated/amule-media-probe.AppImage')
            env.pop('LD_PRELOAD', None)
            reply = subprocess.run([str(root / 'AppRun'), '--version'], env=env,
                                   text=True, capture_output=True, check=True)
            self.assertEqual(reply.stdout, 'helper --version\n')


if __name__ == '__main__':
    unittest.main()
