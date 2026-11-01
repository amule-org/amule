#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise connector config paths offline, with every output in a temporary directory."""
import configparser
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

from DaemonConfig import daemon_env


PASSWORD = '0123456789abcdef0123456789abcdef'


def run(binary, root, *args, expect_failure=False):
    result = subprocess.run([binary, *args], cwd=root, env=daemon_env(root),
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', timeout=15)
    if expect_failure:
        assert result.returncode == 1, (args, result.returncode, result.stdout)
    else:
        assert result.returncode == 0, (args, result.stdout)
    return result.stdout


def read_config(path):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str  # Preserve wxConfig key spelling when writing stale settings.
    text = path.read_text()
    # wxFileConfig also writes ungrouped keys (Locale) before the first section.
    config.read_string(text[text.index('['):])
    return config


def check_path(binary, root, argument, expected):
    source = root / 'amule.conf'
    source.write_text(f'[ExternalConnect]\nECPort=48123\nECPassword={PASSWORD}\n'
                      '[WebServer]\nPort=48125\n')
    args = [] if argument is None else [f'--config-file={argument}']
    run(binary, root, *args, f'--create-config-from={source.as_posix()}')
    config = read_config(expected)
    assert config['EC']['Port'] == '48123', (expected, config.items('EC'))
    assert config['EC']['Password'].lower() == PASSWORD, expected
    if os.name != 'nt':
        assert expected.stat().st_mode & 0o777 == 0o600, expected
    if Path(binary).stem == 'amuleweb':
        assert config['WebServer']['Port'] == '48125', expected
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
    remote.write_text(f'[EC]\nPassword={PASSWORD}\nZLIB=1\nForceZLIB=1\n')
    source = root / 'amule.conf'
    # Import only tests the presence of the verifier key; it cannot be a client credential.
    source.write_text('[ExternalConnect]\nECPort=48123\nECPasswordVerifier=daemon-verifier\n')
    run(binary, root, f'--create-config-from={source}')
    config = read_config(remote)
    assert config['EC']['Password'].lower() == PASSWORD, 'import erased remembered password'
    assert config['EC']['ZLIB'] == '1', 'import changed saved compression'
    assert config['EC']['ForceZLIB'] == '1', 'import changed saved compression override'
    # A verifier-only daemon cannot provide a password, so an explicit -P must work.
    run(binary, root, f'--create-config-from={source}', '-P', 'replacement')
    config = read_config(remote)
    assert config['EC']['Password'].lower() == hashlib.md5(b'replacement').hexdigest(), (
        'import ignored explicitly supplied client password', config.items('EC'))
    # An explicit empty password must remove a remembered credential on a normal save.
    run(binary, root, '--write-config', '-P', '')
    assert read_config(remote)['EC']['Password'] == '', 'save retained explicitly cleared password'
    run(binary, root, f'--create-config-from={source}')
    assert read_config(remote)['EC']['Password'] == '', 'verifier import created a client credential'
    print(f'{Path(binary).name}: verifier import and explicit password writes passed', flush=True)


def main():
    binaries = [str(Path(arg).resolve()) for arg in sys.argv[1:]]
    assert binaries, 'Pass amulecmd and/or amuleweb paths'
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
            for option in ('create-config-from', 'write-config'):
                root = base / ('failed-' + option)
                root.mkdir()
                (root / 'config').mkdir()
                (root / 'config/remote.conf').write_text('')
                blocker = root / 'blocker'
                blocker.write_text('keep this file')
                source = root / 'amule.conf'
                source.write_text('[ExternalConnect]\nECPort=48123\n')
                action = (f'--create-config-from={source.as_posix()}'
                          if option == 'create-config-from' else '--write-config')
                run(binary, root, '--config-file=./blocker/selected.conf', action,
                    expect_failure=True)
                assert blocker.read_text() == 'keep this file'
                print(f'{Path(binary).name}: failed {option} reported', flush=True)

                # Also exercise Flush() failing after a successful parent-directory check.
                (root / 'directory-target').mkdir()
                run(binary, root, '--config-file=./directory-target', action,
                    expect_failure=True)
                assert (root / 'directory-target').is_dir()

            if Path(binary).stem == 'amuleweb':
                root = base / 'amule-config-file'
                root.mkdir()
                source = root / 'amule.conf'
                template = 'config-path-test-template'
                template_dir = root / 'webserver' / template
                template_dir.mkdir(parents=True)
                (template_dir / 'login.php').write_text('')
                # A missing template can fall back to an installed default. Supply our
                # own template and verify the selected config via its EC port instead.
                # Close the mock peer before authentication, so HTTP never starts.
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0))
                    listener.listen()
                    listener.settimeout(10)
                    port = listener.getsockname()[1]
                    source.write_text(f'[ExternalConnect]\nECPort={port}\n'
                                      f'ECPassword={PASSWORD}\n'
                                      f'[WebServer]\nTemplate={template}\n')
                    for argument in [source.as_posix(), './amule.conf']:
                        process = subprocess.Popen(
                            [binary, f'--amule-config-file={argument}'], cwd=root,
                            env=daemon_env(root),
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace')
                        try:
                            with listener.accept()[0] as peer:
                                peer.settimeout(5)
                                assert peer.recv(4096), 'Missing EC authentication request'
                            output, _ = process.communicate(timeout=5)
                            assert process.returncode == 1, (argument, process.returncode, output)
                            assert 'Enter password' not in output, (argument, output)
                        finally:
                            if process.poll() is None:
                                process.kill()
                            process.communicate()
                print('amuleweb: amule-config-file passed', flush=True)


if __name__ == '__main__':
    main()
