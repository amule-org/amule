#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Issue #2438: Apply must preserve the edit while a remote rename is pending.

Run under an isolated X display (for example xvfb-run). The fixture creates its own
session bus and profiles, and pauses only its own daemon to delay rename replies.
"""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def descendants(node):
    yield node
    for index in range(node.get_child_count()):
        yield from descendants(node.get_child_at_index(index))


def find(node, name):
    return next((child for child in descendants(node) if child.get_name() == name), None)


def eventually(check, description, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        result = check()
        if result:
            return result
        assert time.monotonic() < deadline, description
        time.sleep(0.05)


def run(gui_binary, daemon_binary, root, Atspi):
    from AllSearchIntegrationTest import C, free_ports, running_daemon, string
    from DaemonConfig import daemon_env, write_amule_conf, write_remote_conf

    core, local = root / 'core', root / 'local'
    core.mkdir()
    local.mkdir()
    ports = free_ports(2, peer_index=1)
    write_amule_conf(core, ports[0], Port=ports[1], UDPEnable=0, AddNewFilesPaused=1)
    write_remote_conf(local, ports[0], ConfirmExit=0)
    console_path = root / 'gui-console.log'
    with running_daemon(daemon_binary, core, ports) as (daemon, ec):
        link = 'ed2k://|file|original.bin|4096|0123456789abcdef0123456789abcdef|/'
        assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']

        def core_name():
            opcode, files = ec.call_list(C['EC_OP_GET_DLOAD_QUEUE'])
            assert opcode == C['EC_OP_DLOAD_QUEUE']
            return [value.rstrip(b'\0').decode() for name, _, children in files
                    if name == C['EC_TAG_PARTFILE'] for key, value, _ in children
                    if key == C['EC_TAG_PARTFILE_NAME']]

        with console_path.open('w') as console:
            gui = subprocess.Popen([gui_binary, '-c', str(local), '-s', '-o'],
                                   env=daemon_env(root, NO_AT_BRIDGE='0', GTK_USE_PORTAL='0',
                                                  GIO_USE_VFS='local'),
                                   stdout=console, stderr=subprocess.STDOUT)
            try:
                def ready():
                    assert gui.poll() is None, console_path.read_text()
                    return 'Ready' in console_path.read_text()

                eventually(ready, 'Remote GUI did not connect')
                desktop = Atspi.get_desktop(0)
                app = eventually(lambda: next((child for child in descendants(desktop)
                                 if child.get_process_id() == gui.pid and
                                 child.get_role_name() == 'application'), None),
                                 'Remote GUI was not exposed through accessibility')
                find(app, 'Downloads').get_action_iface().do_action(0)
                cell = eventually(lambda: next((child for child in descendants(app)
                                  if child.get_role_name() == 'table cell'), None),
                                  'Download row did not arrive')
                # Activating through AT-SPI's synchronous action blocks in ShowModal;
                # a native double-click lets the accessibility connection keep serving.
                rect = cell.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
                Atspi.generate_mouse_event(rect.x + 20, rect.y + 10, 'b1d')
                dialog = eventually(lambda: find(app, 'File Details'), 'File Details did not open')
                edit = next(child for child in descendants(dialog)
                            if child.get_role_name() == 'text' and
                            Atspi.Text.get_text(child, 0, -1) == 'original.bin')
                apply_button = find(dialog, 'Apply')
                assert not apply_button.get_state_set().contains(Atspi.StateType.ENABLED)

                for name, close in [('applied.bin', False), ('apply-then-ok.bin', True)]:
                    edit.get_editable_text_iface().set_text_contents(name)
                    assert apply_button.get_state_set().contains(Atspi.StateType.ENABLED)
                    daemon.send_signal(signal.SIGSTOP)
                    try:
                        _, status = os.waitpid(daemon.pid, os.WUNTRACED)
                        assert os.WIFSTOPPED(status), 'Test daemon exited before the delayed rename'
                        apply_button.get_action_iface().do_action(0)
                        actual = Atspi.Text.get_text(edit, 0, -1)
                        assert actual == name, f'Apply reverted {name!r} to {actual!r}'
                        if close:
                            find(dialog, 'Ok').get_action_iface().do_action(0)
                    finally:
                        daemon.send_signal(signal.SIGCONT)
                    eventually(lambda: core_name() == [name], f'Daemon did not retain {name!r}')
                    if not close:
                        # The five-second dialog refresh observes the confirmed filename.
                        eventually(lambda: not apply_button.get_state_set().contains(
                                   Atspi.StateType.ENABLED), 'Apply stayed enabled after confirmation')
                        assert Atspi.Text.get_text(edit, 0, -1) == name
            except BaseException:
                print(console_path.read_text(errors='replace'), file=sys.stderr)
                raise
            finally:
                if gui.poll() is None:
                    gui.terminate()
                    try:
                        gui.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        gui.kill()
                        gui.wait()
    print('Remote Apply preserves pending edits; confirmation disables Apply; Apply then OK retains the rename')


def main():
    if not sys.platform.startswith('linux') or not os.environ.get('DISPLAY'):
        print('SKIP: remote rename GUI regression needs Linux and an X display (use xvfb-run)')
        return 77
    try:
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi, Gio
    except (ImportError, ValueError):
        print('SKIP: remote rename GUI regression needs Python GI and Atspi introspection')
        return 77
    from AllSearchIntegrationTest import retry_port_collisions
    # Importing ECWire above also checks the cryptography prerequisite before any launch.
    session_runner = shutil.which('dbus-run-session')
    registry_binary = shutil.which('at-spi2-registryd') or next(
        (str(path) for path in (Path('/usr/libexec/at-spi2-registryd'),
                               Path('/usr/lib/at-spi2-core/at-spi2-registryd')) if path.is_file()), None)
    if not session_runner or not registry_binary:
        print('SKIP: remote rename GUI regression needs dbus-run-session and at-spi2-registryd')
        return 77
    gui, daemon = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    if sys.argv[-1] != '--session':
        from DaemonConfig import daemon_env
        with tempfile.TemporaryDirectory(prefix='amule-rename-session-') as temporary:
            return subprocess.call([session_runner, '--', sys.executable, str(Path(__file__).resolve()),
                                    gui, daemon, '--session'],
                                   env=daemon_env(temporary, AT_SPI_BUS_ADDRESS=None))
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    address = bus.call_sync('org.a11y.Bus', '/org/a11y/bus', 'org.a11y.Bus', 'GetAddress',
                           None, None, Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
    # Explicit launch also works on hosts whose accessibility bus uses systemd activation
    # but has no systemd user manager inside this private session.
    registry = subprocess.Popen([registry_binary], env=dict(os.environ, AT_SPI_BUS_ADDRESS=address),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        def attempt():
            with tempfile.TemporaryDirectory(prefix='amule-file-details-') as temporary:
                run(gui, daemon, Path(temporary), Atspi)
        retry_port_collisions(attempt)
    finally:
        registry.terminate()
        registry.wait(timeout=10)
    return 0


if __name__ == '__main__':
    sys.exit(main())
