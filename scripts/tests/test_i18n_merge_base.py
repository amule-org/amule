#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the dictionary job against a shallow, two-parent PR merge."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DictionaryMergeBaseTest(unittest.TestCase):
    def test_upstream_translation_and_real_pr_addition(self):
        workflow = (ROOT / '.github/workflows/i18n.yml').read_text()
        job = workflow.split('  webapi-dictionaries:', 1)[1]
        self.assertIn('fetch-depth: 2', job)
        lines = job.splitlines()
        step = lines.index("      - name: Selecting the checked merge's target commit")
        start = lines.index('        run: |', step) + 1
        commands = []
        for line in lines[start:]:
            if line.strip() and not line.startswith('          '):
                break
            commands.append(line[10:])
        select_base = '\n'.join(commands)
        env = dict(os.environ)
        env.pop('I18N_BASE', None)

        def run(args, cwd, check=True, **extra_env):
            result = subprocess.run(args, cwd=cwd, env=dict(env, **extra_env),
                                    text=True, capture_output=True, timeout=30)
            if check:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result

        def git(cwd, *args):
            return run(['git', *args], cwd).stdout.strip()

        with tempfile.TemporaryDirectory(prefix='i18n-merge-') as temporary:
            root = Path(temporary)
            origin = root / 'origin'
            origin.mkdir()
            git(origin, 'init', '-b', 'main')
            git(origin, 'config', 'user.name', 'Dictionary regression')
            git(origin, 'config', 'user.email', 'fixture@example.invalid')
            # Use the real checker, dictionaries and preference-label inputs.
            for name in ('tools/check-i18n.mjs', 'static/i18n', 'static/js/i18n.js',
                         'static/js/views/preferences.js'):
                source = ROOT / 'src/webapi' / name
                dest = origin / 'src/webapi' / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                if source.is_dir():
                    shutil.copytree(source, dest)
                else:
                    shutil.copy2(source, dest)
            locales = origin / 'src/webapi/static/i18n'
            zh_path = locales / 'zh-Hans.json'
            zh = json.loads(zh_path.read_text())
            en = json.loads((locales / 'en.json').read_text())
            # Reproduce #2361: upstream Weblate deliberately keeps "Kad".
            self.assertEqual(en['networks_tab_kad'], 'Kad')
            zh.pop('networks_tab_kad', None)
            zh_path.write_text(json.dumps(zh, ensure_ascii=False, indent=2) + '\n')
            git(origin, 'add', '.')
            git(origin, 'commit', '-m', 'stale event base')
            stale = git(origin, 'rev-parse', 'HEAD')
            git(origin, 'checkout', '-b', 'pr')
            (origin / 'unrelated').write_text('unrelated PR change\n')
            git(origin, 'add', '.')
            git(origin, 'commit', '-m', 'unrelated PR')
            head = git(origin, 'rev-parse', 'HEAD')
            git(origin, 'checkout', 'main')
            zh['networks_tab_kad'] = 'Kad'
            zh_path.write_text(json.dumps(zh, ensure_ascii=False, indent=2) + '\n')
            git(origin, 'add', '.')
            git(origin, 'commit', '-m', 'upstream Weblate translation')
            target = git(origin, 'rev-parse', 'HEAD')
            git(origin, 'merge', '--no-ff', 'pr', '-m', 'checked merge')
            checkout = root / 'checkout'
            run(['git', 'clone', '--depth=2', origin.as_uri(), str(checkout)], root)
            self.assertEqual(git(checkout, 'rev-parse', '--is-shallow-repository'), 'true')
            # The old job explicitly fetched this stale event base.
            git(checkout, 'fetch', '--depth=1', 'origin', stale)
            checker = ['node', 'src/webapi/tools/check-i18n.mjs']
            failed = run(checker, checkout, check=False, I18N_BASE=stale)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn('zh-Hans.json: networks_tab_kad', failed.stderr)
            output = root / 'github-env'
            run(['bash', '-e', '-c', select_base], checkout,
                PR_HEAD_SHA=head, GITHUB_ENV=str(output))
            selected = output.read_text().strip().split('=', 1)[1]
            self.assertEqual(selected, target)
            run(checker, checkout, I18N_BASE=selected)
            output.unlink()
            wrong = run(['bash', '-e', '-c', select_base], checkout, check=False,
                        PR_HEAD_SHA=stale, GITHUB_ENV=str(output))
            self.assertNotEqual(wrong.returncode, 0)
            self.assertFalse(output.exists(), 'mismatched head must not select a base')
            # A new English copy in the PR must still fail with the correct base.
            key = next(k for k, v in en.items() if k not in zh and v not in zh.values())
            zh[key] = en[key]
            path = checkout / 'src/webapi/static/i18n/zh-Hans.json'
            path.write_text(json.dumps(zh, ensure_ascii=False, indent=2) + '\n')
            failed = run(checker, checkout, check=False, I18N_BASE=selected)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn('zh-Hans.json: ' + key, failed.stderr)


if __name__ == '__main__':
    unittest.main()
