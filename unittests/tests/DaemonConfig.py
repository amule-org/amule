# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""The amule.conf every integration test writes for the amuled it starts."""
import hashlib
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


def write_amule_conf(root, ec_port, *, ec_password=EC_PASSWORD, sections=None, **emule):
    """Write root/amule.conf: OFFLINE updated with emule, then EC and any extra sections.

    TempDir and IncomingDir default to root/Temp and root/Incoming.
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
    text = ''.join(f'[{name}]\n' + ''.join(f'{key}={value}\n' for key, value in keys.items())
                   for name, keys in all_sections.items())
    (root / 'amule.conf').write_text(text, encoding='utf-8')
