#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest URL privacy against disposable core/API processes and a loopback HTTP server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

from AllSearchIntegrationTest import (configure_amuleapi, free_ports, retry_port_collisions,
                                      start_amuleapi, start_daemon, stop_daemon, wait_amuleapi)
from DaemonConfig import daemon_env, write_amule_conf


class DownloadHandler(BaseHTTPRequestHandler):
    paths = []

    def do_GET(self):
        self.paths.append(self.path)
        if self.path.startswith('/disconnect'):
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if self.path.startswith('/oversize'):
            self.send_response(200)
            self.send_header('Content-Length', str(256 * 1024 * 1024 + 1))
            self.end_headers()
            self.wfile.flush()
            time.sleep(1)  # Let the size guard inspect the headers; send no large body.
            return
        status = 200 if self.path.startswith('/success') else 503
        self.send_response(status)
        self.send_header('Content-Length', '3')
        self.end_headers()
        self.wfile.write(b'bad')  # HTTP success, deliberately not an MMDB database.

    def log_message(self, *_args):
        pass


def stream_events(url, token):
    """Use only the standard library so the offline fixture needs no curl binary."""
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    request = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + token})
    with opener.open(request, timeout=120) as response:
        while True:
            chunk = response.read1(65536)
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()


def run(daemon_binary, api_binary):
    with tempfile.TemporaryDirectory(prefix='amule-url-privacy-') as temporary:
        root = Path(temporary)
        core, api = root / 'core', root / 'api'
        core.mkdir()
        api.mkdir()
        (core / 'Incoming').mkdir()
        (core / 'Temp').mkdir()
        port, http, peer_port = free_ports(3, peer_index=2)
        write_amule_conf(core, port, Port=peer_port, UDPEnable=0,
                         Nick='url-privacy-regression', VerboseDebug=1,
                         Autoconnect=0, GeoIPAutoUpdate=0, GeoIPSource='custom',
                         sections={'WebServer': {'Enabled': 0}, 'Debug': {'Cat_HTTP': 1}})
        env = daemon_env(root, HTTP_PROXY='', HTTPS_PROXY='', ALL_PROXY='', http_proxy='',
                         https_proxy='', all_proxy='', NO_PROXY='127.0.0.1,localhost,::1')
        processes = []
        with ThreadingHTTPServer(('127.0.0.1', 0), DownloadHandler) as downloads, \
                (root / 'fixture.log').open('w') as log:
            threading.Thread(target=downloads.serve_forever, daemon=True).start()
            try:
                daemon, ec = start_daemon(daemon_binary, core, (port, peer_port), env=env)
                processes.append(daemon)
                ec.sock.close()
                configure_amuleapi(api_binary, api, env, log)
                server = start_amuleapi(api_binary, api, port, http, env, log)
                processes.append(server)
                url = f'http://127.0.0.1:{http}/api/v1'
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

                def request(method, path, body=None, token=None):
                    headers = {'Content-Type': 'application/json'}
                    if token:
                        headers['Authorization'] = 'Bearer ' + token
                    req = urllib.request.Request(url + path, method=method, headers=headers,
                        data=json.dumps(body).encode() if body is not None else None)
                    try:
                        response = opener.open(req, timeout=5)
                    except urllib.error.HTTPError as error:
                        response = error
                    with response:
                        data = response.read()
                        return response.status, json.loads(data) if data else None

                def until(predicate):
                    deadline = time.monotonic() + 35
                    while time.monotonic() < deadline:
                        assert all(p.poll() is None for p in processes), 'fixture exited'
                        try:
                            value = predicate()
                            if value:
                                return value
                        except urllib.error.URLError:
                            pass
                        time.sleep(0.1)
                    raise AssertionError('fixture condition timed out')

                admin = wait_amuleapi(server, url, log.name)
                until(lambda: request('GET', '/preferences', token=admin)[0] == 200)
                status, result = request('PATCH', '/auth/passwords?include_token=true', {
                    'current_password': 'adminpass', 'guest_access_enabled': True,
                    'guest_password': 'guestpass'}, admin)
                assert status == 200, (status, result)
                admin = result['token']
                status, result = request('POST', '/auth/login?include_token=true',
                                         {'password': 'guestpass'})
                assert status == 200, (status, result)
                guest = result['token']
                base = f'http://127.0.0.1:{downloads.server_port}'
                secret_url = base.replace('http://', 'http://USER-1874:PASS-1874@') + '/failure-PATH-1874.mmdb?license_key=LICENSE-KEY-1874&token=TOKEN-1874#FRAGMENT-1874'
                patch = {'geoip': {'source': 'custom', 'custom_update_url': secret_url,
                                  'maxmind_license': 'MAXMIND-1874'},
                         'connection': {'proxy_user': 'PROXY-USER-1874'},
                         'servers': {'update_url': base + '/server.met?token=SERVER-1874',
                                     'update_urls': [base + '/server.met?token=SERVER-1874',
                                                     base + '/backup.met?token=BACKUP-1874']},
                         'security': {'ipfilter_update_url': base + '/filter?token=FILTER-1874'},
                         'kad': {'update_url': base + '/nodes.dat?token=KAD-1874'}}
                status, result = request('PATCH', '/preferences', patch, admin)
                assert status == 200, (status, result)
                # Admin GET and PATCH echoes must retain exact values for editing.
                for category, fields in patch.items():
                    for key, value in fields.items():
                        assert result[category][key] == value, (category, key, result)
                status, result = request('GET', '/preferences', token=admin)
                for category, fields in patch.items():
                    for key, value in fields.items():
                        assert result[category][key] == value, (category, key, result)
                status, result = request('GET', '/preferences', token=guest)
                assert status == 200
                for category, fields in patch.items():
                    for key in fields:
                        if key != 'source':
                            assert key not in result[category], (category, key, result)
                assert result['geoip']['source'] == 'custom'
                assert request('PATCH', '/preferences', patch, guest)[0] == 403
                print('PASS: guest fields omitted; admin GET/PATCH preserve credentials')

                # Subscribe before downloads; verify log_appended uses the same safe text.
                events_file = root / 'events.txt'
                with events_file.open('w') as events:
                    subscriber = subprocess.Popen([sys.executable, __file__, '--events',
                        url + '/events', guest], env=env, stdout=events, stderr=log)
                    processes.append(subscriber)
                    until(lambda: events_file.stat().st_size > 0)
                    assert request('PATCH', '/preferences', {'geoip': {'enabled': True}}, admin)[0] == 200
                    assert request('POST', '/geoip/update', token=admin)[0] == 202
                    until(lambda: 'Failed to download' in request(
                        'GET', '/preferences', token=guest)[1]['geoip']['last_update_status'])
                    assert '/failure-PATH-1874.mmdb?license_key=LICENSE-KEY-1874&token=TOKEN-1874' in DownloadHandler.paths
                    until(lambda: 'Failed to download geoip.mmdb from ' + base in events_file.read_text())
                    failure_status = request('GET', '/preferences', token=guest)[1]['geoip']['last_update_status']
                    assert failure_status.endswith(base), failure_status
                    for route, category, key, path in [
                            ('/servers_update', 'servers', 'update_url', '/server.met?token=SERVER-1874'),
                            ('/ipfilter/update', 'security', 'ipfilter_update_url', '/filter?token=FILTER-1874')]:
                        assert request('POST', route, {'url': patch[category][key]}, admin)[0] == 202
                        until(lambda: path in DownloadHandler.paths)
                    for expected in ['Failed to download ipfilter.dat', 'Failed to download server.met']:
                        until(lambda: expected in '\n'.join(request(
                            'GET', '/logs/amule', token=guest)[1]['lines']))
                    # A second successful HTTP transfer exercises the success log too.
                    success_url = base + '/success.mmdb?token=SUCCESS-1874'
                    assert request('PATCH', '/preferences', {
                        'geoip': {'custom_update_url': success_url}}, admin)[0] == 200
                    assert request('POST', '/geoip/update', token=admin)[0] == 202
                    until(lambda: '/success.mmdb?token=SUCCESS-1874' in DownloadHandler.paths)
                    until(lambda: 'Downloaded ' in '\n'.join(request(
                        'GET', '/logs/amule', token=guest)[1]['lines']))
                    until(lambda: 'Downloaded ' in events_file.read_text())
                    failure_prefix = 'HTTP download failed for ' + base + ': '
                    previous_failures = request('GET', '/logs/amule', token=guest)[1]['lines']
                    disconnect_url = base + '/disconnect.mmdb?token=CONNECTION-1874'
                    assert request('PATCH', '/preferences', {
                        'geoip': {'custom_update_url': disconnect_url}}, admin)[0] == 200
                    assert request('POST', '/geoip/update', token=admin)[0] == 202
                    until(lambda: '/disconnect.mmdb?token=CONNECTION-1874' in DownloadHandler.paths)
                    until(lambda: request('GET', '/preferences', token=guest)[1]['geoip'][
                        'last_update_status'].endswith(base))
                    failure_line = until(lambda: next((line for line in request(
                        'GET', '/logs/amule', token=guest)[1]['lines']
                        if failure_prefix in line and line not in previous_failures), None))
                    assert failure_line.split(failure_prefix, 1)[1].strip(), failure_line
                    print('PASS: backend download failure reason is preserved')
                    oversize_url = base + '/oversize.mmdb?token=OVERSIZE-1874'
                    assert request('PATCH', '/preferences', {
                        'geoip': {'custom_update_url': oversize_url}}, admin)[0] == 200
                    assert request('POST', '/geoip/update', token=admin)[0] == 202
                    until(lambda: '/oversize.mmdb?token=OVERSIZE-1874' in DownloadHandler.paths)
                    until(lambda: 'larger than 256 MiB' in events_file.read_text())
                    until(lambda: request('GET', '/preferences', token=guest)[1]['geoip'][
                        'last_update_status'].endswith(base))
                    subscriber.terminate()
                    subscriber.wait(timeout=5)
                    processes.remove(subscriber)
                sensitive = ['USER-1874', 'PASS-1874', 'FRAGMENT-1874', 'LICENSE-KEY-1874', 'TOKEN-1874', 'MAXMIND-1874', 'PATH-1874',
                             'PROXY-USER-1874', 'SERVER-1874', 'BACKUP-1874', 'FILTER-1874',
                             'KAD-1874', 'SUCCESS-1874', 'CONNECTION-1874', 'OVERSIZE-1874']
                surfaces = [failure_status, json.dumps(request('GET', '/preferences', token=guest)[1]),
                            json.dumps(request('GET', '/logs/amule', token=guest)[1]),
                            events_file.read_text(), (core / 'logfile').read_text()]
                for text in surfaces:
                    for secret in sensitive:
                        assert secret not in text, ('secret leaked', secret, text)
                assert 'log_appended' in events_file.read_text()
                print('PASS: failure status, success/failure logs and guest SSE contain no URL secrets')
                stop_daemon(server, timeout=15)
            except BaseException:
                log.flush()
                print((root / 'fixture.log').read_text(), file=sys.stderr)
                if (core / 'console.log').exists():
                    print((core / 'console.log').read_text(errors='replace'), file=sys.stderr)
                if (core / 'logfile').exists():
                    print((core / 'logfile').read_text()[-12000:], file=sys.stderr)
                raise
            finally:
                for proc in reversed(processes):
                    stop_daemon(proc, check=False)
                downloads.shutdown()


if __name__ == '__main__':
    if sys.argv[1] == '--events':
        stream_events(*sys.argv[2:])
    else:
        retry_port_collisions(run, *map(os.path.abspath, sys.argv[1:]))
