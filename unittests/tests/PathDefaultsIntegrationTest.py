#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep display-string defaults separate from serialized filesystem bytes on Linux."""
import configparser
from contextlib import contextmanager
import os
from pathlib import Path
import sys
import tempfile

from AllSearchIntegrationTest import (C, free_ports, integer, log_snapshot, new_log_text,
                                      retry_port_collisions, running_daemon, string, tag)
from DaemonConfig import daemon_env, write_amule_conf


def to_univ(path):
    # CPath::ToUniv maps each filesystem byte to a Latin-1 character; wxFileConfig
    # then writes those characters as UTF-8. Do not interpret filename bytes.
    return os.fsencode(path).decode('latin-1')


def read_config(root):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config.read(root / 'amule.conf', encoding='utf-8')
    return config['eMule']


def write_config(root, paths):
    root.mkdir()
    ec_port, peer_port = free_ports(2, peer_index=1)
    # The test checks amuled's own path defaults, so only the given paths are written.
    write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0,
                     **dict({'TempDir': None, 'IncomingDir': None}, **paths))
    return ec_port, peer_port


@contextmanager
def daemon(binary, root, ports, locale='C.UTF-8'):
    """running_daemon that yields the EC client and then checks no path failed."""
    snapshot = log_snapshot(root / 'logfile')
    console = root / 'console.log'
    console_size = console.stat().st_size if console.exists() else 0
    env = daemon_env(root, LC_ALL=locale, LANG=locale)
    with running_daemon(binary, root, ports, env=env) as (_, ec):
        yield ec
    log = (new_log_text(root / 'logfile', snapshot)
           + console.read_bytes()[console_size:].decode(errors='replace'))
    assert "couldn't be created" not in log, log
    assert 'Error accessing directory' not in log, log
    assert 'Could not create the ' not in log, log


def check_paths(root, expected):
    config = read_config(root)
    for key, path in expected.items():
        assert config[key] == to_univ(path), (key, config[key], to_univ(path))
        assert os.path.isdir(path), repr(path)


def save_and_check_paths(ec, root, expected):
    # Force an actual save after loading. Shutdown alone need not save prefs,
    # and a first-run save cannot prove that the reloaded value round-trips.
    assert ec.call(C['EC_OP_SET_PREFERENCES'])[0] == C['EC_OP_NOOP']
    check_paths(root, expected)


def get_directories(ec):
    op, tags = ec.call(C['EC_OP_GET_PREFERENCES'], [
        integer(C['EC_TAG_SELECT_PREFS'], C['EC_PREFS_DIRECTORIES'])])
    assert op == C['EC_OP_SET_PREFERENCES'], (op, tags)
    return tags[C['EC_TAG_PREFS_DIRECTORIES']][1]


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-path-defaults-') as scratch:
        base = Path(scratch)
        for name in ('prof ü', 'prof д'):
            root = base / name
            ports = write_config(root, {})
            expected = {'TempDir': root / 'Temp', 'IncomingDir': root / 'Incoming'}
            for _ in range(2):
                with daemon(binary, root, ports) as ec:
                    # Inspect before shutdown: the constructor's first save must
                    # already contain the correct defaults, before any fallback.
                    check_paths(root, expected)
                    assert read_config(root)['OSDirectory'] == to_univ(str(root) + '/')
                    dirs = get_directories(ec)
                    assert dirs[C['EC_TAG_DIRECTORIES_TEMP']][0] == os.fsencode(expected['TempDir']) + b'\0'
                    assert dirs[C['EC_TAG_DIRECTORIES_INCOMING']][0] == os.fsencode(expected['IncomingDir']) + b'\0'
                check_paths(root, expected)
            # Missing keys in an existing profile must work without the new-user
            # constructor save; remove directories too so startup must use them.
            config_path = root / 'amule.conf'
            config = config_path.read_text(encoding='utf-8')
            config_path.write_text('\n'.join(
                line for line in config.splitlines()
                if not line.startswith(('TempDir=', 'IncomingDir=', 'OSDirectory='))) + '\n',
                encoding='utf-8')
            for path in expected.values():
                path.rmdir()
            with daemon(binary, root, ports) as ec:
                assert 'TempDir' not in read_config(root)
                assert 'IncomingDir' not in read_config(root)
                save_and_check_paths(ec, root, expected)
                assert read_config(root)['OSDirectory'] == to_univ(str(root) + '/')

        root = base / 'empty д'
        ports = write_config(root, {'TempDir': '', 'IncomingDir': '', 'OSDirectory': ''})
        expected = {'TempDir': root / 'Temp', 'IncomingDir': root / 'Incoming'}
        with daemon(binary, root, ports):
            check_paths(root, expected)
            # Only the two required directories opt into empty fallback.
            assert read_config(root)['OSDirectory'] == ''
        check_paths(root, expected)
        # With preferences.dat already present, loading does not save. Empty
        # settings must still work at runtime, then be repaired on the next save.
        config_path = root / 'amule.conf'
        config = config_path.read_text(encoding='utf-8')
        for key, path in expected.items():
            config = config.replace(f'{key}={to_univ(path)}', f'{key}=')
        config_path.write_text(config, encoding='utf-8')
        with daemon(binary, root, ports) as ec:
            dirs = get_directories(ec)
            assert dirs[C['EC_TAG_DIRECTORIES_TEMP']][0] == os.fsencode(expected['TempDir']) + b'\0'
            assert dirs[C['EC_TAG_DIRECTORIES_INCOMING']][0] == os.fsencode(expected['IncomingDir']) + b'\0'
            assert read_config(root)['TempDir'] == ''
            assert read_config(root)['IncomingDir'] == ''
            assert read_config(root)['OSDirectory'] == ''
            save_and_check_paths(ec, root, expected)
            assert read_config(root)['OSDirectory'] == ''
        check_paths(root, expected)

        root = base / 'ec-saved'
        ports = write_config(root, {})
        # A remote folder change needs an allowed root.
        (root / 'allowed-remote-dirs.dat').write_text(f'{root}\n')
        expected = {'TempDir': root / 'Temp ü', 'IncomingDir': root / 'Incoming д'}
        with daemon(binary, root, ports) as ec:
            # Full detail: without it, aMule reads absent directory flags as false.
            op, tags = ec.call(C['EC_OP_SET_PREFERENCES'], [
                tag(C['EC_TAG_DETAIL_LEVEL'], bytes([C['EC_DETAIL_FULL']]), 2),
                tag(C['EC_TAG_PREFS_DIRECTORIES'], children=[
                    string(C['EC_TAG_DIRECTORIES_TEMP'], str(expected['TempDir'])),
                    string(C['EC_TAG_DIRECTORIES_INCOMING'], str(expected['IncomingDir']))])])
            assert op == C['EC_OP_NOOP'], (op, tags)
            config = read_config(root)
            for key, path in expected.items():
                assert config[key] == to_univ(path), (key, config[key])
        for locale in ('C.UTF-8', 'C'):
            with daemon(binary, root, ports, locale) as ec:
                check_paths(root, expected)
                save_and_check_paths(ec, root, expected)
            check_paths(root, expected)

        root = base / 'raw-bytes'
        expected = {key: os.fsencode(root) + b'/' + name + b' \xe9'
                    for key, name in (('TempDir', b'Temp'), ('IncomingDir', b'Incoming'))}
        ports = write_config(root, {key: to_univ(path) for key, path in expected.items()})
        # An existing plain-text interpretation must never override saved bytes.
        for path in expected.values():
            Path(path.decode('latin-1')).mkdir()
        for locale in ('C.UTF-8', 'C'):
            with daemon(binary, root, ports, locale) as ec:
                check_paths(root, expected)
                save_and_check_paths(ec, root, expected)
            check_paths(root, expected)
            for path in expected.values():
                os.rmdir(path)
    print('Path defaults, empty recovery, EC save/reload and raw filename bytes passed')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
