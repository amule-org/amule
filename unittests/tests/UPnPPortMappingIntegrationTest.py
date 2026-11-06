#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Port mapping results, late services and the autoconnect hold, checked against a fake gateway."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import free_ports, spawn_daemon, stop_daemon
from DaemonConfig import write_amule_conf

SKIP = 77
OPT_IN = 'AMULE_TEST_UPNP_NO_REAL_GATEWAY'
ON_CI = 'GITHUB_ACTIONS' in os.environ
DISCOVERY_TIMEOUT = 15
# libupnp ignores answers once its search timer fires, 2 to 3 s after the search.
AFTER_SEARCH_TIMER = 3.5
# The control point stops holding callers back this long after the search.
AFTER_DISCOVERY_DEADLINE = 4.5
PENDING_POLLS = 20
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


def multicast(sock, data):
    for index, _ in socket.if_nameindex():
        try:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, membership(index))
            sock.sendto(data, (SSDP_GROUP, SSDP_PORT))
        except OSError:
            pass


class FakeGateway:
    """Answers SSDP, description, SCPD, eventing and SOAP. Reject sets hold (service, mapping).

    A silent gateway ignores searches and appears through announce(). A held service stalls its
    SCPD until release is set, which keeps it from registering.
    """

    def __init__(self, services=('ip',), mappings=(), reject=(), answer_delay=0, silent=False,
                 held=None):
        self.services = services
        self.reject = set(reject)
        self.answer_delay = answer_delay
        self.silent = silent
        self.held = held
        self.release = threading.Event()
        self.first_registered = threading.Event()
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
        self.release.set()
        self.http.shutdown()
        self.http.server_close()
        self.ssdp.close()

    def location(self):
        return f'http://127.0.0.1:{self.http.server_address[1]}/desc.xml'

    def announce(self):
        notify = (f'NOTIFY * HTTP/1.1\r\nHOST: {SSDP_GROUP}:{SSDP_PORT}\r\nCACHE-CONTROL: max-age=1800\r\n'
                  f'LOCATION: {self.location()}\r\nNT: {IGD}\r\nNTS: ssdp:alive\r\n'
                  f'SERVER: fake/1 UPnP/1.0 fake/1\r\nUSN: uuid:fake-igd::{IGD}\r\n\r\n')
        multicast(self.ssdp, notify.encode())

    def answer_searches(self):
        reply = (f'HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n'
                 f'LOCATION: {self.location()}\r\nSERVER: fake/1 UPnP/1.0 fake/1\r\nST: {IGD}\r\n'
                 f'USN: uuid:fake-igd::{IGD}\r\n\r\n')
        while True:
            try:
                data, peer = self.ssdp.recvfrom(4096)
                if data.startswith(b'M-SEARCH'):
                    if not self.silent:
                        time.sleep(self.answer_delay)
                        self.ssdp.sendto(reply.encode(), peer)
            except OSError:
                return

    def on_subscribe(self, service):
        # The control point lists and maps a service before it subscribes, so this means it can use
        # them all and has sent the mappings it knows.
        self.subscribed.add(service)
        self.first_registered.set()
        if self.subscribed == set(self.services):
            self.registered.set()

    def description(self):
        services = ''.join(
            f'<service><serviceType>{SERVICE_TYPES[name]}</serviceType>'
            f'<serviceId>urn:upnp-org:serviceId:{name}</serviceId><SCPDURL>/scpd/{name}</SCPDURL>'
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
                if self.path == f'/scpd/{gateway.held}':
                    gateway.release.wait()
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
        multicast(probe, search)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                _, peer = probe.recvfrom(4096)
            except socket.timeout:
                continue
            return peer[0]
    return None


class Driver:
    """Runs the driver one operation at a time. Entering returns when the constructor has."""

    def __init__(self, driver, operations):
        self.command = [driver, str(TCP_PORT), str(UDP_PORT), *operations]

    def __enter__(self):
        self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        self.watchdog = threading.Timer(60, self.process.kill)
        self.watchdog.start()
        first = self.process.stdout.readline().strip()
        if first != 'constructed':
            details = self.stop()
            if self.process.returncode == SKIP:
                raise Skipped(first)
            raise ScenarioFailed(f'driver failed ({details})')
        return self

    def __exit__(self, failure, *_):
        details = self.stop()
        if failure:
            print(f'driver: {details}', file=sys.stderr)
        elif self.process.returncode != 0:
            raise ScenarioFailed(f'driver failed ({details})')

    def stop(self):
        # Closing stdin makes the driver exit.
        errors = self.process.communicate()[1]
        self.watchdog.cancel()
        return f'exit code {self.process.returncode}, stderr: {errors.strip()}'

    def run(self, count):
        """Returns the results of the next operations."""
        try:
            self.process.stdin.write('\n' * count)
            self.process.stdin.flush()
        except BrokenPipeError as error:
            raise ScenarioFailed('driver stopped') from error
        lines = [self.process.stdout.readline().strip() for _ in range(count)]
        if not all('=' in line for line in lines):
            raise ScenarioFailed(f'driver stopped after {lines}')
        return dict(line.split('=') for line in lines)


def wait_registered(gateway):
    if not gateway.registered.wait(DISCOVERY_TIMEOUT):
        raise ScenarioFailed(f'only {gateway.subscribed} registered')


def run_driver(driver, gateway, operations):
    with Driver(driver, operations) as running:
        wait_registered(gateway)
        return running.run(len(operations))


def run_late(driver, gateway, before, after):
    """Runs operations before and after the gateway, or its second service, registers."""
    with Driver(driver, before + after) as running:
        if gateway.held and not gateway.first_registered.wait(DISCOVERY_TIMEOUT):
            raise ScenarioFailed('the first service did not register')
        got = running.run(len(before))
        gateway.release.set()
        if gateway.silent:
            time.sleep(AFTER_SEARCH_TIMER)
            gateway.announce()
        wait_registered(gateway)
        got.update(running.run(len(after)))
    return got


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

# name, gateway arguments, operations before and after, results, requests, final table.
LATE_SCENARIOS = [
    # A gateway may wait before it answers.
    ('gateway answers after the mappings', {'services': BOTH, 'answer_delay': 1.5}, ['add'], [],
     {'add': 'false'}, requests(ADD, *BOTH), {TCP, UDP}),
    ('gateway announces after the search', {'services': BOTH, 'silent': True}, ['add'], [],
     {'add': 'false'}, requests(ADD, *BOTH), {TCP, UDP}),
    ('mappings deleted before the gateway appears', {'services': BOTH, 'silent': True},
     ['add', 'delete'], [], {'add': 'false', 'delete': 'false'}, [], set()),
    ('delete after the gateway appears', {'silent': True}, ['add'], ['delete'],
     {'add': 'false', 'delete': 'true'}, requests(ADD, 'ip') + requests(DELETE, 'ip'), set()),
    ('second service registers after the mappings', {'services': BOTH, 'held': 'ppp'}, ['add'], [],
     {'add': 'true'}, requests(ADD, *BOTH), {TCP, UDP}),
]


def record_failure(failures, message):
    # Print now: CTest's time limit can stop the run before main() reports.
    print(message)
    failures.append(message)


def run_scenarios(driver, failures):
    for name, arguments, operations, results, expected in SCENARIOS:
        with FakeGateway(**arguments) as gateway:
            try:
                got = run_driver(driver, gateway, operations)
            except ScenarioFailed as error:
                record_failure(failures, f'{name}: {error}')
                continue
        if got != results:
            record_failure(failures, f'{name}: returned {got}, expected {results}')
        # Exact list: also rules out state queries, GetStatusInfo and the disabled mapping.
        if sorted(gateway.requests) != sorted(expected):
            record_failure(failures, f'{name}: gateway saw {gateway.requests}, expected {expected}')


def run_late_scenarios(driver, failures):
    for name, arguments, before, after, results, expected, table in LATE_SCENARIOS:
        with FakeGateway(**arguments) as gateway:
            try:
                got = run_late(driver, gateway, before, after)
            except ScenarioFailed as error:
                record_failure(failures, f'{name}: {error}')
                continue
        if got != results:
            record_failure(failures, f'{name}: returned {got}, expected {results}')
        if sorted(gateway.requests) != sorted(expected):
            record_failure(failures, f'{name}: gateway saw {gateway.requests}, expected {expected}')
        for service, mappings in gateway.tables.items():
            if mappings != table:
                record_failure(failures, f'{name}: {service} holds {mappings}, expected {table}')


def wait_not_pending(running):
    # Under 2 s, so a pass at "gateway registered" can't come from the 4 s deadline.
    for _ in range(PENDING_POLLS):
        if running.run(1) == {'pending': 'false'}:
            return
        time.sleep(0.1)
    raise ScenarioFailed('still pending after the gateway registered')


def check_discovery_pending(driver, failures):
    def check(name, got, expected):
        if got != {'pending': expected}:
            record_failure(failures, f'discovery pending: {name}: {got}')
    try:
        # True right after the constructor also proves it did not wait out the search.
        with FakeGateway(silent=True), Driver(driver, ['pending'] * 2) as running:
            check('no gateway yet', running.run(1), 'true')
            time.sleep(AFTER_DISCOVERY_DEADLINE)
            check('search over', running.run(1), 'false')
        # The gateway sees the subscription just before the control point finishes with it.
        with FakeGateway() as gateway, Driver(driver, ['pending'] * PENDING_POLLS) as running:
            wait_registered(gateway)
            wait_not_pending(running)
        # A registration that outlasts the search keeps the hold.
        with FakeGateway(held='ip') as gateway, Driver(driver, ['pending'] * (1 + PENDING_POLLS)) as running:
            time.sleep(AFTER_DISCOVERY_DEADLINE)
            check('still registering', running.run(1), 'true')
            gateway.release.set()
            wait_registered(gateway)
            wait_not_pending(running)
        # A Netgear GS308EP answers as a gateway and lists no services.
        with FakeGateway(services=()), Driver(driver, ['pending']) as running:
            time.sleep(1)
            check('gateway without services', running.run(1), 'true')
    except ScenarioFailed as error:
        record_failure(failures, f'discovery pending: {error}')


def local_address():
    """amuled refuses a server on loopback. Connecting a UDP socket sends nothing."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect((SSDP_GROUP, SSDP_PORT))
            address = probe.getsockname()[0]
        except OSError:
            # Also the case on a network with no default route.
            address = socket.gethostbyname(socket.gethostname())
    if address.startswith('127.'):
        raise Skipped('no local address for the fake server')
    return address


def wait_login(listener):
    """Returns once the server's login packet arrives. Nothing here reads the rest."""
    with listener.accept()[0] as peer:
        peer.settimeout(30)
        peer.recv(4096)


def tcp_mapped_at_login(amuled, gateway):
    """Starts amuled with autoconnect on, and returns whether the port is mapped at its login."""
    address = local_address()
    ec_port, tcp_port, udp_port = free_ports(3, udp_indices=(2,), peer_index=1)
    with tempfile.TemporaryDirectory() as root, socket.socket() as listener:
        listener.bind((address, 0))
        listener.listen(1)
        listener.settimeout(30)
        server_port = listener.getsockname()[1]
        write_amule_conf(root, ec_port, Address=None, Port=tcp_port, UDPPort=udp_port, FilterLanIPs=0,
                         ConnectToED2K=1, Autoconnect=1, UPnPEnabled=1, UPnPTCPPort=0)
        (Path(root) / 'server.met').write_bytes(
            b'\xe0' + struct.pack('<I', 1) + socket.inet_aton(address)
            + struct.pack('<HI', server_port, 0))
        console_path = Path(root) / 'console.log'
        # Not running_daemon(): its wait for EC would read the table after the login.
        with spawn_daemon(amuled, root) as process:
            try:
                wait_login(listener)
                mapped = any((str(tcp_port), 'TCP') in table for table in gateway.tables.values())
                stop_daemon(process, timeout=20)
                return mapped
            except (OSError, AssertionError) as error:
                stop_daemon(process, timeout=20, check=False)
                print(console_path.read_text(errors='replace'))
                raise ScenarioFailed(f'amuled failed: {error}') from error


def check_amuled_autoconnect(amuled, failures):
    try:
        with FakeGateway(services=BOTH, answer_delay=1.5) as gateway:
            if not tcp_mapped_at_login(amuled, gateway):
                record_failure(failures, 'amuled logged in before a slow gateway was mapped')
        with FakeGateway(services=BOTH, held='ip') as gateway:
            # Past the end of the search, and past the login that would follow it without a hold.
            threading.Timer(7, gateway.release.set).start()
            if not tcp_mapped_at_login(amuled, gateway):
                record_failure(failures, 'amuled logged in before a registering gateway was mapped')
        # Holding back must end: with no gateway, amuled still logs in.
        with FakeGateway(silent=True) as gateway:
            tcp_mapped_at_login(amuled, gateway)
    except ScenarioFailed as error:
        record_failure(failures, f'amuled autoconnect: {error}')


def main():
    # Mappings use lease 0 and the add scenarios never delete them. A lost probe or a NOTIFY during
    # the run can bring in a real gateway, so outside CI nobody runs this by accident.
    if not ON_CI and os.environ.get(OPT_IN) != '1':
        print(f'SKIP: set {OPT_IN}=1 on a network with no real gateway to run this test')
        sys.exit(SKIP)
    gateway_address = real_gateway_answers()
    if gateway_address:
        skip_or_fail(f'a gateway answers at {gateway_address}; this test would change its mappings')
    failures = []
    skipped = None
    try:
        run_scenarios(sys.argv[1], failures)
        run_late_scenarios(sys.argv[1], failures)
        check_discovery_pending(sys.argv[1], failures)
        if len(sys.argv) > 2:
            check_amuled_autoconnect(sys.argv[2], failures)
    except Skipped as error:
        skipped = str(error)
    if failures:
        sys.exit('FAIL: see the failures above')
    if skipped:
        skip_or_fail(skipped)


if __name__ == '__main__':
    main()
