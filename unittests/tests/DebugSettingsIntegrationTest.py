#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Change amuled's debug settings through amuleapi, with no restart."""
import configparser
from pathlib import Path
import sys
import tempfile
import threading

from AllSearchIntegrationTest import (C, configure_amuleapi, free_ports, http,
                                      retry_port_collisions, running_amuleapi, running_daemon,
                                      string)
from DaemonConfig import daemon_env, write_amule_conf
from WriteBufferPreferencesIntegrationTest import Core, CoreHandler


def emit(ec, root, message):
    assert ec.call(C['EC_OP_ADDDEBUGLOGLINE'], [
        string(C['EC_TAG_STRING'], message)])[0] == C['EC_OP_NOOP']
    # The whole-log EC read is a barrier for the logfile.
    assert ec.call(C['EC_OP_GET_LOG'])[0] == C['EC_OP_LOG']
    return [line for line in (root / 'logfile').read_text().splitlines() if message in line]


def patch(base, token, **advanced):
    status, result = http(base, '/preferences', token, 'PATCH', {'advanced': advanced})
    assert status == 200, result
    return result['advanced']


def live_daemon(daemon_binary, api_binary, temp):
    root = Path(temp) / 'live'
    core, api = root / 'core', root / 'api'
    core.mkdir(parents=True)
    api.mkdir()
    env = daemon_env(root)
    ec_port, peer_port, http_port = free_ports(3, peer_index=1)
    ports = ec_port, peer_port
    write_amule_conf(core, ec_port, Port=peer_port, UDPEnable=0, VerboseDebug=0,
                     VerboseDebugLogfile=0, sections={'Debug': {'Cat_General': 0, 'Cat_Hasher': 1, 'Cat_GUI': 1}})
    with running_daemon(daemon_binary, core, ports, env=env) as (_, ec), \
            (root / 'api.log').open('w') as log:
        configure_amuleapi(api_binary, api, env, log, admin='regression')
        with running_amuleapi(api_binary, api, ec_port, http_port, env, log,
                              password='regression') as (_, base, token):
            advanced = http(base, '/preferences', token)[1]['advanced']
            assert advanced['debug_categories_supported'] is True, advanced
            assert advanced['verbose_logging_logfile_only'] is False, advanced
            assert {'General', 'Hasher'} <= set(advanced['debug_categories']), advanced
            assert 'GUI' not in advanced['debug_categories'], advanced
            assert advanced['debug_categories_enabled'] == ['Hasher'], advanced
            assert not emit(ec, core, 'debug-settings-off')

            advanced = patch(base, token, verbose_logging=True,
                             debug_categories_enabled=['General'])
            assert advanced['debug_categories_enabled'] == ['General'], advanced
            assert '[log status=0 replay=1]' in emit(ec, core, 'debug-settings-on')[0]

            # A read-modify-write client sends the read-only list back.
            advanced = patch(base, token, debug_categories=[],
                             debug_categories_enabled=[])
            assert advanced['debug_categories_enabled'] == [], advanced
            assert advanced['debug_categories'], advanced
            assert not emit(ec, core, 'debug-settings-emptied')

            patch(base, token, debug_categories_enabled=['General'],
                  verbose_logging_logfile_only=True)
            assert '[log status=0 replay=0]' in emit(ec, core, 'debug-settings-logfile')[0]

            status, result = http(base, '/preferences', token, 'PATCH', {'advanced': {
                'debug_categories_enabled': ['No Such Category']}})
            assert status == 400 and result['error']['code'] == 'bad_request', result
            for bad in ('General', [1]):
                assert http(base, '/preferences', token, 'PATCH', {'advanced': {
                    'debug_categories_enabled': bad}})[0] == 400, bad
            advanced = http(base, '/preferences', token)[1]['advanced']
            assert advanced['debug_categories_enabled'] == ['General'], advanced

            # An update that does not name them leaves them alone.
            advanced = patch(base, token, max_new_connections_per_5_seconds=51)
            assert advanced['debug_categories_enabled'] == ['General'], advanced
            assert advanced['verbose_logging_logfile_only'] is True, advanced

    config = configparser.ConfigParser(interpolation=None)
    config.read(core / 'amule.conf')
    assert config['eMule'].getboolean('VerboseDebugLogfile')
    assert config['Debug'].getboolean('Cat_General')
    assert not config['Debug'].getboolean('Cat_Hasher')
    # amuled does not offer GUI, but a monolithic aMule on this directory reads it.
    assert config['Debug'].getboolean('Cat_GUI')
    with running_daemon(daemon_binary, core, ports, env=env) as (_, ec):
        assert '[log status=0 replay=0]' in emit(ec, core, 'debug-settings-restart')[0]


def older_core(api_binary, temp):
    root = Path(temp) / 'older'
    root.mkdir()
    env = daemon_env(root)
    with Core(('127.0.0.1', 0), CoreHandler) as core, (root / 'api.log').open('w') as log:
        core.supported, core.limit, core.connections, core.writes = False, 128, 50, []
        threading.Thread(target=core.serve_forever, daemon=True).start()
        configure_amuleapi(api_binary, root, env, log, admin='regression')
        http_port = free_ports(1)[0]
        with running_amuleapi(api_binary, root, core.server_address[1], http_port, env, log,
                              password='regression') as (_, base, token):
            advanced = http(base, '/preferences', token)[1]['advanced']
            assert advanced['debug_categories_supported'] is False, advanced
            assert advanced['debug_categories'] == [], advanced
            assert advanced['debug_categories_enabled'] == [], advanced
            for field, value in (('debug_categories_enabled', []),
                                 ('verbose_logging_logfile_only', True)):
                status, result = http(base, '/preferences', token, 'PATCH', {'advanced': {
                    field: value}})
                assert status == 409, (field, result)
                assert result['error']['code'] == 'option_not_supported', result
            assert not core.writes, 'an unsupported write reached the older core'
        core.shutdown()


def run(daemon_binary, api_binary):
    with tempfile.TemporaryDirectory(prefix='amule-debug-settings-') as temp:
        live_daemon(daemon_binary, api_binary, temp)
        older_core(api_binary, temp)
    print('Debug settings integration checks passed')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
