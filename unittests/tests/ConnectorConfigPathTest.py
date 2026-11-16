#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise connector config paths offline, with every output in a temporary directory."""
import configparser
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from DaemonConfig import daemon_env


PASSWORD = 'config-path-secret'
DIGEST = hashlib.md5(PASSWORD.encode()).hexdigest()


def run(binary, root, *args, expect_failure=False, stdin=None):
    # Bytes, so Windows does not turn the test's line endings into CRLF.
    result = subprocess.run([binary, *args], cwd=root, env=daemon_env(root),
                            input=None if stdin is None else stdin.encode(),
                            stdin=None if stdin is not None else subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
    output = result.stdout.decode(errors='replace')
    if expect_failure is None:
        pass
    elif expect_failure:
        assert result.returncode == 1, (args, result.returncode, output)
    else:
        assert result.returncode == 0, (args, output)
    return output


def read_config(path):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str  # Preserve wxConfig key spelling when writing stale settings.
    text = path.read_text()
    # wxFileConfig also writes ungrouped keys (Locale) before the first section.
    config.read_string(text[text.index('['):])
    return config


def check_path(binary, root, argument, expected):
    args = [] if argument is None else [f'--config-file={argument}']
    run(binary, root, *args, '--write-config', '--port=48123', '--password-stdin',
        stdin=PASSWORD + '\n')
    config = read_config(expected)
    assert config['EC']['Port'] == '48123', (expected, config.items('EC'))
    assert config['EC']['PasswordMD5'].lower() == DIGEST, expected
    if os.name != 'nt':
        assert expected.stat().st_mode & 0o777 == 0o600, expected
    # Read the same selected file, then save it without starting a connection.
    config['EC']['Host'] = 'config-path-test.invalid'
    with expected.open('w') as stream:
        config.write(stream)
    if os.name != 'nt':
        expected.chmod(0o644)
    for encryption in ('0', '1'):
        config['EC']['Encryption'] = encryption
        with expected.open('w') as stream:
            config.write(stream)
        run(binary, root, *args, '--write-config', '--port=48124')
        config = read_config(expected)
        assert 'Encryption' not in config['EC'], (expected, config.items('EC'))
        assert config['EC']['Port'] == '48124', expected
        assert config['EC']['Host'] == 'config-path-test.invalid', expected
        if os.name != 'nt':
            assert expected.stat().st_mode & 0o777 == 0o600, expected


def check_password_config(binary, root):
    root.mkdir()
    (root / 'config').mkdir()
    remote = root / 'config/remote.conf'
    # The legacy key of remote.conf already holds a digest: it moves unchanged.
    remote.write_text(f'[EC]\nPassword={DIGEST}\nZLIB=1\nForceZLIB=1\n')
    run(binary, root, '--write-config', '--port=48125')
    config = read_config(remote)
    assert 'Password' not in config['EC'], ('legacy key kept', config.items('EC'))
    assert config['EC']['PasswordMD5'].lower() == DIGEST, ('digest not migrated', config.items('EC'))
    assert config['EC']['ZLIB'] == '1', 'migration changed saved compression'
    assert config['EC']['ForceZLIB'] == '1', 'migration changed saved compression override'
    run(binary, root, '--write-config', '--password-stdin', stdin='replacement\r\n')
    config = read_config(remote)
    assert config['EC']['PasswordMD5'].lower() == hashlib.md5(b'replacement').hexdigest(), (
        'stdin password not saved', config.items('EC'))
    # Empty input and EOF fail before anything is saved.
    for stdin in ('\n', ''):
        out = run(binary, root, '--write-config', '--port=48126', '--password-stdin',
                  stdin=stdin, expect_failure=True)
        assert read_config(remote)['EC']['Port'] == '48125', ('failed input saved', out)
    # Two nonempty keys: neither can be known to be current, so nothing runs or changes.
    text = f'[EC]\nPassword={DIGEST}\nPasswordMD5={DIGEST}\n'
    remote.write_text(text)
    out = run(binary, root, '--write-config', expect_failure=True)
    assert 'cannot both be nonempty' in out, out
    assert remote.read_text() == text, 'conflicting keys were changed'
    # A new password from stdin replaces both.
    run(binary, root, '--write-config', '--password-stdin', stdin='repaired\n')
    config = read_config(remote)
    assert 'Password' not in config['EC'], ('legacy key kept', config.items('EC'))
    assert config['EC']['PasswordMD5'].lower() == hashlib.md5(b'repaired').hexdigest(), (
        'conflict not repaired', config.items('EC'))
    if os.name != 'nt' and os.geteuid() != 0:
        # An unwritable file still logs in with the migrated digest for this run.
        remote.write_text(f'[EC]\nPassword={DIGEST}\n')
        (root / 'config').chmod(0o500)
        try:
            # --help exits without connecting, after the config is loaded.
            out = run(binary, root, '--help', expect_failure=None)
        finally:
            (root / 'config').chmod(0o700)
        assert 'this run only' in out, out
        assert f'Password={DIGEST}' in remote.read_text(), 'read-only file changed'
    print(f'{Path(binary).name}: password migration and stdin input passed', flush=True)


def main():
    binaries = [str(Path(arg).resolve()) for arg in sys.argv[1:]]
    assert binaries, 'Pass amulecmd paths'
    with tempfile.TemporaryDirectory(prefix='amule-config-path-') as scratch:
        for index, binary in enumerate(binaries):
            base = Path(scratch).resolve() / str(index)
            base.mkdir()
            check_password_config(binary, base / 'password-config')
            # The portable marker prevents touching the real user config on Windows/macOS.
            cases = ['default', 'bare', 'absolute', 'dot-relative', 'parent-relative', 'subdirectory',
                     'new-directory']
            if os.name == 'nt':
                cases += ['backslash', 'drive-relative', 'root-relative']
            if sys.platform.startswith('linux'):
                cases += ['home', 'user-default', 'user-bare', 'new-user-directory']
            for case in cases:
                root = base / case
                root.mkdir(parents=True)
                portable = root / 'config'
                portable.mkdir()
                (portable / 'remote.conf').write_text('')
                expected = root / 'selected.conf'
                if case == 'default':
                    argument, expected = None, portable / 'remote.conf'
                elif case == 'bare':
                    argument, expected = 'custom.conf', portable / 'custom.conf'
                elif case == 'absolute':
                    argument = expected.as_posix()
                elif case == 'dot-relative':
                    argument = './selected.conf'
                elif case == 'parent-relative':
                    argument, expected = '../selected.conf', root.parent / 'selected.conf'
                elif case == 'subdirectory':
                    (root / 'subdir').mkdir()
                    argument, expected = 'subdir/selected.conf', root / 'subdir/selected.conf'
                elif case in ('new-directory', 'new-user-directory'):
                    argument = './new/nested/selected.conf'
                    expected = root / 'new/nested/selected.conf'
                    if case == 'new-user-directory':
                        (portable / 'remote.conf').unlink()
                elif case == 'backslash':
                    argument = str(expected)
                elif case == 'drive-relative':
                    argument = root.drive + 'selected.conf'
                elif case == 'root-relative':
                    argument = str(expected)[len(root.drive):]
                elif case == 'home':
                    argument = '~/selected.conf'
                else:
                    (portable / 'remote.conf').unlink()
                    argument = None if case == 'user-default' else 'custom.conf'
                    expected = root / '.aMule' / (argument or 'remote.conf')
                check_path(binary, root, argument, expected)
                if case == 'new-user-directory':
                    assert not (root / '.aMule').exists(), 'Created an unrelated default directory'
                print(f'{Path(binary).name}: {case} passed', flush=True)

            root = base / 'new-write-config'
            root.mkdir()
            (root / 'config').mkdir()
            (root / 'config/remote.conf').write_text('')
            run(binary, root, '--config-file=./new/nested/selected.conf', '--write-config',
                '--host=config-path-test.invalid', '--port=48124')
            config = read_config(root / 'new/nested/selected.conf')
            assert config['EC']['Port'] == '48124'
            assert config['EC']['Host'] == 'config-path-test.invalid'
            print(f'{Path(binary).name}: new write-config passed', flush=True)

            # A regular file cannot serve as the selected file's parent directory.
            root = base / 'failed-write-config'
            root.mkdir()
            (root / 'config').mkdir()
            (root / 'config/remote.conf').write_text('')
            blocker = root / 'blocker'
            blocker.write_text('keep this file')
            run(binary, root, '--config-file=./blocker/selected.conf', '--write-config',
                expect_failure=True)
            assert blocker.read_text() == 'keep this file'
            print(f'{Path(binary).name}: failed write-config reported', flush=True)

            # Also exercise Flush() failing after a successful parent-directory check.
            (root / 'directory-target').mkdir()
            run(binary, root, '--config-file=./directory-target', '--write-config',
                expect_failure=True)
            assert (root / 'directory-target').is_dir()


if __name__ == '__main__':
    main()
