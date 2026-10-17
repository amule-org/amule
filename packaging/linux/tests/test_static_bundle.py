"""Exercise final tarball staging without building dependencies or running Docker."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[3]


class StaticBundleTests(unittest.TestCase):
    def test_static_tarball_includes_media_helper_and_notices(self):
        with tempfile.TemporaryDirectory(prefix='amule-static-bundle-') as scratch:
            root = Path(scratch)
            packaging = root / 'packaging' / 'linux'
            packaging.mkdir(parents=True)
            for name in ('build.sh', 'versions.env'):
                shutil.copy2(SOURCE / 'packaging' / 'linux' / name, packaging / name)
            webui = root / 'src' / 'webapi' / 'static'
            for name in ('index.html', 'css/app.css', 'js/app.js'):
                path = webui / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('fixture')
            commands = root / 'commands'
            commands.mkdir()
            scripts = {
                'docker': '''#!/usr/bin/env python3
from pathlib import Path
import sys
output = sys.argv[sys.argv.index('--output') + 1].split('dest=', 1)[1]
root = Path(output)
for name in ('amuled', 'amulecmd', 'amuleapi', 'amule-media-probe'):
    (root / name).write_text('fixture executable')
(root / 'licenses').mkdir()
(root / 'licenses' / 'MediaInfoLib.html').write_text('fixture notice')
''',
                'file': '#!/bin/sh\necho "ELF statically linked"\n',
                'git': '''#!/bin/sh
if [ "$1" = describe ]; then
    echo test-version
else
    printf '%s\\n' src/webapi/static/index.html src/webapi/static/css/app.css src/webapi/static/js/app.js
fi
''',
            }
            for name, content in scripts.items():
                path = commands / name
                path.write_text(content)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'])
            subprocess.run(['bash', str(packaging / 'build.sh'), 'static', 'host'],
                           env=env, capture_output=True, text=True, check=True)
            archives = list((root / 'dist').glob('*.tar.gz'))
            self.assertEqual(len(archives), 1)
            import tarfile
            with tarfile.open(archives[0]) as archive:
                names = {Path(name).relative_to(archive.getnames()[0]).as_posix()
                         for name in archive.getnames()}
            for name in ('amuled', 'amulecmd', 'amuleapi', 'amule-media-probe',
                         'licenses/MediaInfoLib.html', 'amuleapi-static/index.html'):
                self.assertIn(name, names)


if __name__ == '__main__':
    unittest.main()
