#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Port mapping results come from each action's reply, checked against a fake gateway."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

SKIP = 77
OPT_IN = 'AMULE_TEST_UPNP_NO_REAL_GATEWAY'
ON_CI = 'GITHUB_ACTIONS' in os.environ
DISCOVERY_TIMEOUT = 15
ADD, DELETE = 'AddPortMapping', 'DeletePortMapping'
SSDP_GROUP = '239.255.255.250'
SSDP_PORT = 1900
IGD = 'urn:schemas-upnp-org:device:InternetGatewayDevice:1'
SERVICE_TYPES = {
    'ip': 'urn:schemas-upnp-org:service:WANIPConnection:1',
    'ppp': 'urn:schemas-upnp-org:service:WANPPPConnection:1',
}
TCP_PORT, UDP_PORT = 59662, 59672
TCP, UDP = (str(TCP_PORT), 'TCP'), (str(UDP_PORT), 'UDP')
ADD_ARGS = {
    'NewRemoteHost': 'RemoteHost',
    'NewExternalPort': 'ExternalPort',
    'NewProtocol': 'PortMappingProtocol',
    'NewInternalPort': 'InternalPort',
    'NewInternalClient': 'InternalClient',
    'NewEnabled': 'PortMappingEnabled',
    'NewPortMappingDescription': 'PortMappingDescription',
    'NewLeaseDuration': 'PortMappingLeaseDuration',
}
DELETE_ARGS = {name: ADD_ARGS[name] for name in list(ADD_ARGS)[:3]}
ENVELOPE = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>%s</s:Body>'
            '</s:Envelope>')
FAULT = ('<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>'
         '<UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>%d</errorCode>'
         '<errorDescription>rejected</errorDescription></UPnPError></detail></s:Fault>')
CONFLICT_IN_MAPPING_ENTRY, NO_SUCH_ENTRY, INVALID_ACTION = 718, 714, 401


class Skipped(Exception):
    """This machine or LAN cannot run the test."""


class ScenarioFailed(Exception):
    pass


def skip_or_fail(reason):
    """A local LAN may not suit this test, but a skip on CI would hide a broken test."""
    if ON_CI:
        sys.exit(f'FAIL: {reason}')
    print(f'SKIP: {reason}')
    sys.exit(SKIP)


def membership(index):
    return struct.pack('4s4si', socket.inet_aton(SSDP_GROUP), socket.inet_aton('0.0.0.0'), index)


class FakeGateway:
    """Answers SSDP, description, SCPD, eventing and SOAP. Reject sets hold (service, mapping)."""

    def __init__(self, services=('ip',), mappings=(), reject=()):
        self.services = services
        self.reject = set(reject)
        self.tables = {name: set(mappings) for name in services}
        self.requests = []
        self.subscribed = set()
        self.registered = threading.Event()
        self.http = ThreadingHTTPServer(('127.0.0.1', 0), self.handler())
        self.ssdp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ssdp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.ssdp.bind(('', SSDP_PORT))
        except OSError as error:
            raise Skipped(f'cannot bind SSDP port {SSDP_PORT}: {error}') from error
        # libupnp picks its own interface, so listen on all of them.
        for index, _ in socket.if_nameindex():
            try:
                self.ssdp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership(index))
            except OSError:
                pass

    def __enter__(self):
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        threading.Thread(target=self.answer_searches, daemon=True).start()
        return self

    def __exit__(self, *_):
        self.http.shutdown()
        self.http.server_close()
        self.ssdp.close()

    def answer_searches(self):
        location = f'http://127.0.0.1:{self.http.server_address[1]}/desc.xml'
        reply = (f'HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: {location}\r\n'
                 f'SERVER: fake/1 UPnP/1.0 fake/1\r\nST: {IGD}\r\nUSN: uuid:fake-igd::{IGD}\r\n\r\n')
        while True:
            try:
                data, peer = self.ssdp.recvfrom(4096)
                if data.startswith(b'M-SEARCH'):
                    self.ssdp.sendto(reply.encode(), peer)
            except OSError:
                return

    def on_subscribe(self, service):
        # The control point lists a service before it subscribes, so this means it can use them all.
        self.subscribed.add(service)
        if self.subscribed == set(self.services):
            self.registered.set()

    def description(self):
        services = ''.join(
            f'<service><serviceType>{SERVICE_TYPES[name]}</serviceType>'
            f'<serviceId>urn:upnp-org:serviceId:{name}</serviceId><SCPDURL>/scpd.xml</SCPDURL>'
            f'<controlURL>/control/{name}</controlURL><eventSubURL>/event/{name}</eventSubURL></service>'
            for name in self.services)
        return ('<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><specVersion>'
                f'<major>1</major><minor>0</minor></specVersion><device><deviceType>{IGD}</deviceType>'
                '<friendlyName>fake</friendlyName><UDN>uuid:fake-igd</UDN><deviceList><device>'
                '<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>'
                '<friendlyName>wan</friendlyName><UDN>uuid:fake-wan</UDN><deviceList><device>'
                '<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>'
                '<friendlyName>conn</friendlyName><UDN>uuid:fake-conn</UDN>'
                f'<serviceList>{services}</serviceList></device></deviceList></device></deviceList>'
                '</device></root>')

    @staticmethod
    def scpd():
        def action(name, arguments):
            xml = ''.join(f'<argument><name>{arg}</name><direction>in</direction>'
                          f'<relatedStateVariable>{var}</relatedStateVariable></argument>'
                          for arg, var in arguments.items())
            return f'<action><name>{name}</name><argumentList>{xml}</argumentList></action>'
        variables = ''.join(f'<stateVariable sendEvents="no"><name>{var}</name>'
                            '<dataType>string</dataType></stateVariable>'
                            for var in ADD_ARGS.values())
        return ('<?xml version="1.0"?><scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion>'
                '<major>1</major><minor>0</minor></specVersion><actionList>'
                f'{action(ADD, ADD_ARGS)}{action(DELETE, DELETE_ARGS)}'
                f'{action("GetStatusInfo", {})}</actionList><serviceStateTable>{variables}'
                '</serviceStateTable></scpd>')

    def control(self, service, action, body):
        """Returns the UPnP error code, or None when the action succeeds."""
        def argument(name):
            found = re.search(f'<{name}[^>]*>([^<]*)<', body)
            return found[1] if found else ''
        mapping = (argument('NewExternalPort'), argument('NewProtocol'))
        self.requests.append((service, action, mapping))
        if action == ADD:
            if (service, mapping) in self.reject:
                return CONFLICT_IN_MAPPING_ENTRY
            self.tables[service].add(mapping)
            return None
        if action == DELETE:
            if mapping not in self.tables[service]:
                return NO_SUCH_ENTRY
            self.tables[service].discard(mapping)
            return None
        return INVALID_ACTION

    def handler(gateway):
        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def reply(self, status, body=''):
                data = body.encode()
                self.send_response(status)
                self.send_header('Content-Type', 'text/xml; charset="utf-8"')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('EXT', '')
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.reply(200, gateway.description() if self.path == '/desc.xml' else gateway.scpd())

            def do_SUBSCRIBE(self):
                service = self.path.rsplit('/', 1)[-1]
                gateway.on_subscribe(service)
                self.send_response(200)
                self.send_header('SID', f'uuid:fake-{service}')
                self.send_header('TIMEOUT', 'Second-1800')
                self.send_header('Content-Length', '0')
                self.end_headers()

            def do_UNSUBSCRIBE(self):
                self.reply(200)

            def do_POST(self):
                service = self.path.rsplit('/', 1)[-1]
                body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
                action = self.headers.get('SOAPACTION', '').strip('"').rsplit('#', 1)[-1]
                error = gateway.control(service, action, body)
                if error is not None:
                    self.reply(500, ENVELOPE % (FAULT % error))
                else:
                    self.reply(200, ENVELOPE % f'<u:{action}Response xmlns:u="{SERVICE_TYPES[service]}"/>')
        return Handler


def real_gateway_answers():
    """The control point maps on every gateway it finds, so never run next to a real one."""
    search = (f'M-SEARCH * HTTP/1.1\r\nHOST: {SSDP_GROUP}:{SSDP_PORT}\r\nMAN: "ssdp:discover"\r\n'
              f'MX: 1\r\nST: {IGD}\r\n\r\n').encode()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.settimeout(0.2)
        for index, _ in socket.if_nameindex():
            try:
                probe.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, membership(index))
                probe.sendto(search, (SSDP_GROUP, SSDP_PORT))
            except OSError:
                pass
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                _, peer = probe.recvfrom(4096)
            except socket.timeout:
                continue
            return peer[0]
    return None


def run_driver(driver, gateway, operations):
    with subprocess.Popen([driver, str(TCP_PORT), str(UDP_PORT), *operations], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) as process:
        deadline = time.monotonic() + DISCOVERY_TIMEOUT
        while (not gateway.registered.wait(0.1) and process.poll() is None
               and time.monotonic() < deadline):
            pass
        # Closing stdin without a line makes the driver exit without sending anything.
        output, _ = process.communicate('\n' if gateway.registered.is_set() else '', timeout=60)
    if process.returncode == SKIP:
        raise Skipped(output.strip())
    if not gateway.registered.is_set():
        raise ScenarioFailed('the control point did not discover the fake gateway')
    if process.returncode != 0:
        raise ScenarioFailed(f'driver exited with {process.returncode}: {output}')
    return dict(line.split('=') for line in output.splitlines() if '=' in line)


def requests(action, *services):
    return [(service, action, mapping) for mapping in (TCP, UDP) for service in services]


# name, gateway arguments, operations, expected results, expected requests.
BOTH = ('ip', 'ppp')
SCENARIOS = [
    ('new mappings', {}, ['add'], {'add': 'true'}, requests(ADD, 'ip')),
    # The router-wide entry count does not change, and the request still succeeded.
    ('mappings already exist', {'mappings': (TCP, UDP)}, ['add'], {'add': 'true'}, requests(ADD, 'ip')),
    ('all rejected', {'reject': (('ip', TCP), ('ip', UDP))}, ['add'], {'add': 'false'},
     requests(ADD, 'ip')),
    ('first rejected, second still sent', {'reject': (('ip', TCP),)}, ['add'], {'add': 'false'},
     requests(ADD, 'ip')),
    ('one service rejects', {'services': BOTH, 'reject': (('ppp', TCP), ('ppp', UDP))}, ['add'],
     {'add': 'true'}, requests(ADD, *BOTH)),
    ('every service rejects one mapping', {'services': BOTH, 'reject': (('ip', UDP), ('ppp', UDP))},
     ['add'], {'add': 'false'}, requests(ADD, *BOTH)),
    ('delete existing', {'mappings': (TCP, UDP)}, ['delete'], {'delete': 'true'},
     requests(DELETE, 'ip')),
    ('delete missing', {}, ['delete'], {'delete': 'false'}, requests(DELETE, 'ip')),
    ('one delete fails, other still sent', {'mappings': (UDP,)}, ['delete'], {'delete': 'false'},
     requests(DELETE, 'ip')),
    ('one service never held the mapping', {'services': BOTH, 'reject': (('ppp', TCP), ('ppp', UDP))},
     ['add', 'delete'], {'add': 'true', 'delete': 'true'},
     requests(ADD, *BOTH) + requests(DELETE, *BOTH)),
]


def run_scenarios(driver, failures):
    for name, arguments, operations, results, expected in SCENARIOS:
        with FakeGateway(**arguments) as gateway:
            try:
                got = run_driver(driver, gateway, operations)
            except ScenarioFailed as error:
                failures.append(f'{name}: {error}')
                continue
        if got != results:
            failures.append(f'{name}: returned {got}, expected {results}')
        # Exact list: also rules out state queries, GetStatusInfo and the disabled mapping.
        if sorted(gateway.requests) != sorted(expected):
            failures.append(f'{name}: gateway saw {gateway.requests}, expected {expected}')


def main():
    # Mappings use lease 0 and the add scenarios never delete them. A lost probe or a NOTIFY during
    # the run can bring in a real gateway, so outside CI nobody runs this by accident.
    if not ON_CI and OPT_IN not in os.environ:
        print(f'SKIP: set {OPT_IN}=1 on a network with no real gateway to run this test')
        sys.exit(SKIP)
    gateway_address = real_gateway_answers()
    if gateway_address:
        skip_or_fail(f'a gateway answers at {gateway_address}; this test would change its mappings')
    failures = []
    skipped = None
    try:
        run_scenarios(sys.argv[1], failures)
    except Skipped as error:
        skipped = str(error)
    if failures:
        sys.exit('FAIL:\n' + '\n'.join(failures))
    if skipped:
        skip_or_fail(skipped)


if __name__ == '__main__':
    main()
