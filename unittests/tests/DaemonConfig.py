# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""The amule.conf every integration test writes for the amuled it starts."""
import hashlib
import os
from pathlib import Path

EC_PASSWORD = 'regression'

# Offline by default: no update, GeoIP, IP filter, server list or Kad nodes download,
# no network join and no UPnP. Each test turns on only what it exercises. A finished
# HTTP download can also crash amuled on wx older than 3.2.12 (#1136).
OFFLINE = {
    'Nick': 'regression',
    'FirstRunWizardDone': 1,
    # The checks match English log lines; LC_ALL alone does not override a translated LANG.
    'Language': 'en_US',
    'Address': '127.0.0.1',
    'ConnectToKad': 0,
    'ConnectToED2K': 0,
    'Reconnect': 0,
    'Serverlist': 0,
    'Ed2kServersUrl': '',
    'KadNodesUrl': '',
    'AddServerListFromServer': 0,
    'AddServerListFromClient': 0,
    'NewVersionCheck': 0,
    'GeoIPEnabled': 0,
    'IPFilterAutoLoad': 0,
    'UPnPEnabled': 0,
}


def daemon_env(home, **extra):
    """The environment for a binary a test starts: HOME and XDG under home, C.UTF-8.

    LD_PRELOAD is dropped so a sanitizer or fault-injection preload in the caller's shell
    reaches only the tests that pass it in extra. A key set to None in extra is removed.
    """
    env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(Path(home) / 'xdg'),
               LC_ALL='C.UTF-8')
    env.pop('LD_PRELOAD', None)
    for key, value in extra.items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = str(value)
    return env


def write_amule_conf(root, ec_port, *, ec_password=EC_PASSWORD, sections=None, **emule):
    """Write root/amule.conf: OFFLINE updated with emule, then EC and any extra sections.

    TempDir and IncomingDir default to root/Temp and root/Incoming. A key set to None is
    left out, so amuled applies its own default.
    """
    root = Path(root)
    values = dict(OFFLINE, TempDir=root / 'Temp', IncomingDir=root / 'Incoming')
    values.update(emule)
    all_sections = {
        'eMule': values,
        'ExternalConnect': {
            'AcceptExternalConnections': 1,
            'ECAddress': '127.0.0.1',
            'ECPort': ec_port,
            'ECPassword': hashlib.md5(ec_password.encode()).hexdigest(),
        },
    }
    all_sections.update(sections or {})
    _write_sections(root / 'amule.conf', all_sections)


def write_remote_conf(root, ec_port, *, ec_password=EC_PASSWORD, **emule):
    """Write root/remote.conf for amulegui: the EC target, then OFFLINE's language and emule."""
    _write_sections(Path(root) / 'remote.conf', {
        'EC': {
            'Host': '127.0.0.1',
            'Port': ec_port,
            'Password': hashlib.md5(ec_password.encode()).hexdigest(),
        },
        'eMule': dict(Language=OFFLINE['Language'], **emule),
    })


def _write_sections(path, sections):
    text = ''.join(f'[{name}]\n' + ''.join(f'{key}={value}\n' for key, value in keys.items()
                                         if value is not None)
                   for name, keys in sections.items())
    path.write_text(text, encoding='utf-8')
