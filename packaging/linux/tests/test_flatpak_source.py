"""Ensure local Flatpak builds use the reviewed commit, including its notices."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[3]


class FlatpakSourceTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('envsubst'), 'Flatpak rendering requires gettext-base')
    def test_flatpak_clones_current_commit_with_media_notices(self):
        with tempfile.TemporaryDirectory(prefix='amule flatpak source ') as scratch:
            root = Path(scratch)
            packaging = root / 'packaging' / 'linux'
            (packaging / 'flatpak').mkdir(parents=True)
            for name in ('build.sh', 'versions.env', 'flatpak/org.amule.aMule.yaml.in'):
                shutil.copy2(SOURCE / 'packaging' / 'linux' / name, packaging / name)
            notices = root / 'packaging' / 'media' / 'licenses'
            shutil.copytree(SOURCE / 'packaging' / 'media' / 'licenses', notices)
            helper = root / 'src' / 'mediaprobe' / 'main.cpp'
            helper.parent.mkdir(parents=True)
            helper.write_text('reviewed helper')
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            subprocess.run(['git', '-C', str(root), 'add', 'packaging', 'src'], check=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test',
                            '-c', 'user.email=test@example.invalid',
                            'commit', '-qm', 'reviewed sources'], check=True)
            commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                                             text=True).strip()
            helper.write_text('uncommitted helper')
            commands = root / 'commands'
            commands.mkdir()
            scripts = {
                'flatpak-builder': r'''#!/usr/bin/env python3
import json
from pathlib import Path
import re
import subprocess
import sys
manifest = Path(sys.argv[-1]).read_text().split('  - name: amule\n', 1)[1]
url = re.search(r'^        url: (.+)$', manifest, re.M).group(1)
commit = re.search(r'^        commit: ([0-9a-f]{40})$', manifest, re.M).group(1)
assert url.startswith('file://'), url
root = Path.cwd().parents[2]
clone = root / 'cloned source'
subprocess.run(['git', 'clone', '-q', '--no-checkout', url, str(clone)], check=True)
subprocess.run(['git', '-C', str(clone), 'checkout', '-q', commit], check=True)
assert (clone / 'src/mediaprobe/main.cpp').read_text() == 'reviewed helper'
notices = sorted(p.name for p in (clone / 'packaging/media/licenses').iterdir())
(root / 'source-result.json').write_text(json.dumps({'commit': commit, 'notices': notices}))
''',
                'flatpak': '''#!/usr/bin/env python3
from pathlib import Path
import sys
assert sys.argv[1] == 'build-bundle', sys.argv
Path(sys.argv[-3]).touch()
''',
            }
            for name, contents in scripts.items():
                path = commands / name
                path.write_text(contents)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(['bash', str(packaging / 'build.sh'), 'flatpak', 'host'],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            source = json.loads((root / 'source-result.json').read_text())
            self.assertEqual(source['commit'], commit)
            self.assertEqual(source['notices'], sorted(p.name for p in notices.iterdir()))
            self.assertIn('MediaInfoLib.html', source['notices'])
            self.assertEqual(len(list((root / 'dist').glob('*.flatpak'))), 1)


if __name__ == '__main__':
    unittest.main()
