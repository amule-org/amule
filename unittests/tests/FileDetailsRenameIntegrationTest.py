#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Issue #2438: File Details must preserve and submit user edits in both GUI variants.

Run under an isolated X display (for example xvfb-run). The fixture creates its own
session bus and profiles, and pauses only its own daemon to delay rename replies.
"""
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def descendants(node):
    if node is None:  # A closing dialog can disappear between child-count and child lookup.
        return
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


def run(gui_binary, daemon_binary, root, Atspi, local_mode=False):
    from AllSearchIntegrationTest import C, free_ports, running_daemon, string, tag, connect_daemon
    from DaemonConfig import daemon_env, write_amule_conf, write_remote_conf

    core, local = root / 'core', root / 'local'
    core.mkdir()
    local.mkdir()
    ports = free_ports(2, peer_index=1)
    write_amule_conf(core, ports[0], Port=ports[1], UDPEnable=0, AddNewFilesPaused=1, ConfirmExit=0)
    write_remote_conf(local, ports[0], ConfirmExit=0)
    console_path = root / 'gui-console.log'
    with ExitStack() as backend:
        daemon, ec = backend.enter_context(running_daemon(daemon_binary, core, ports))
        link = 'ed2k://|file|original.bin|4096|0123456789abcdef0123456789abcdef|/'
        assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'], link)])[0] == C['EC_OP_NOOP']

        def core_name():
            opcode, files = ec.call_list(C['EC_OP_GET_DLOAD_QUEUE'])
            assert opcode == C['EC_OP_DLOAD_QUEUE']
            return [value.rstrip(b'\0').decode() for name, _, children in files
                    if name == C['EC_TAG_PARTFILE'] for key, value, _ in children
                    if key == C['EC_TAG_PARTFILE_NAME']]

        if local_mode:
            backend.close()  # Seed the profile with the daemon, then open it in the local GUI.
        command = ([gui_binary, '-c', str(core), '--disable-fatal', '-o'] if local_mode else
                   [gui_binary, '-c', str(local), '-s', '-o'])
        with console_path.open('w') as console:
            gui = subprocess.Popen(command,
                                   env=daemon_env(root, NO_AT_BRIDGE='0', GTK_USE_PORTAL='0',
                                                  GIO_USE_VFS='local'),
                                   stdout=console, stderr=subprocess.STDOUT)
            try:
                if local_mode:
                    ec = connect_daemon(gui, ports[0])
                    backend.callback(ec.sock.close)
                def ready():
                    assert gui.poll() is None, console_path.read_text()
                    return local_mode or 'Ready' in console_path.read_text()

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
                    if not local_mode:
                        daemon.send_signal(signal.SIGSTOP)
                    try:
                        if not local_mode:
                            _, status = os.waitpid(daemon.pid, os.WUNTRACED)
                            assert os.WIFSTOPPED(status), 'Test daemon exited before the delayed rename'
                        apply_button.get_action_iface().do_action(0)
                        actual = Atspi.Text.get_text(edit, 0, -1)
                        assert actual == name, f'Apply reverted {name!r} to {actual!r}'
                        assert not apply_button.get_state_set().contains(Atspi.StateType.ENABLED), \
                            'Submitted edit stayed dirty'
                        if not close:
                            # The old name is still in the remote model. It is nevertheless
                            # a new edit, and must supersede the queued rename.
                            edit.get_editable_text_iface().set_text_contents('original.bin')
                            assert apply_button.get_state_set().contains(Atspi.StateType.ENABLED), \
                                'Returning to the original name ignored a pending rename'
                            apply_button.get_action_iface().do_action(0)
                            name = 'original.bin'
                        if close:
                            find(dialog, 'Ok').get_action_iface().do_action(0)
                    finally:
                        if not local_mode:
                            daemon.send_signal(signal.SIGCONT)
                    eventually(lambda: core_name() == [name], f'Daemon did not retain {name!r}')
                    if not close:
                        # Let the queued renames and a real dialog refresh complete.
                        time.sleep(5.5)
                        assert core_name() == [name]
                        eventually(lambda: not apply_button.get_state_set().contains(
                                   Atspi.StateType.ENABLED), 'Apply stayed enabled after confirmation')
                        assert Atspi.Text.get_text(edit, 0, -1) == name

                def open_details(expected):
                    rect = cell.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
                    Atspi.generate_mouse_event(rect.x + 20, rect.y + 10, 'b1d')
                    details = eventually(lambda: find(app, 'File Details'), 'Details did not reopen')
                    field = next(child for child in descendants(details)
                                 if child.get_role_name() == 'text' and
                                 Atspi.Text.get_text(child, 0, -1) == expected)
                    return details, field, find(details, 'Apply')

                time.sleep(5.5)  # Allow the remote list model to receive the rename.
                dialog, edit, apply_button = open_details('apply-then-ok.bin')
                assert not apply_button.get_state_set().contains(Atspi.StateType.ENABLED)
                edit.get_editable_text_iface().set_text_contents('draft.bin')
                # A real timer tick must not replace an unsubmitted edit.
                time.sleep(5.5)
                assert Atspi.Text.get_text(edit, 0, -1) == 'draft.bin'
                assert apply_button.get_state_set().contains(Atspi.StateType.ENABLED)
                find(dialog, 'Cancel').get_action_iface().do_action(0)
                assert core_name() == ['apply-then-ok.bin'], 'Cancel applied an unsaved edit'

                dialog, edit, apply_button = open_details('apply-then-ok.bin')
                # Invalid names are still rejected by the authoritative core validator.
                edit.get_editable_text_iface().set_text_contents('../invalid.bin')
                apply_button.get_action_iface().do_action(0)
                eventually(lambda: 'Unable to rename file' in console_path.read_text() or
                           'plain file name' in (core / 'logfile').read_text(),
                           'Core did not reject the invalid name')
                assert core_name() == ['apply-then-ok.bin']
                assert Atspi.Text.get_text(edit, 0, -1) == '../invalid.bin'
                # Programmatic Cleanup edits use the same dirty-state path as typing.
                edit.get_editable_text_iface().set_text_contents('clean_name.BIN')
                find(dialog, 'Cleanup').get_action_iface().do_action(0)
                assert Atspi.Text.get_text(edit, 0, -1) == 'Clean Name.bin'
                assert apply_button.get_state_set().contains(Atspi.StateType.ENABLED)
                apply_button.get_action_iface().do_action(0)
                eventually(lambda: core_name() == ['Clean Name.bin'], 'Correction did not rename')
                # Wait for the model update, then navigate in both directions. Even with
                # one file, both handlers must reset an unsubmitted edit and its dirty bit.
                time.sleep(5.5)
                arrows = [child for child in descendants(dialog)
                          if child.get_role_name() == 'button' and child.get_name() == '']
                assert len(arrows) == 2, 'Missing file navigation buttons'
                for arrow in arrows:
                    edit.get_editable_text_iface().set_text_contents('discard-on-navigation.bin')
                    arrow.get_action_iface().do_action(0)
                    assert Atspi.Text.get_text(edit, 0, -1) == 'Clean Name.bin'
                    assert not apply_button.get_state_set().contains(Atspi.StateType.ENABLED)
                # A background rename after an Apply must not be undone by an unchanged OK.
                request = [tag(C['EC_TAG_KNOWNFILE'], bytes.fromhex(
                           '0123456789abcdef0123456789abcdef'), 9),
                           string(C['EC_TAG_PARTFILE_NAME'], 'external.bin')]
                assert ec.call(C['EC_OP_RENAME_FILE'], request)[0] == C['EC_OP_NOOP']
                find(dialog, 'Ok').get_action_iface().do_action(0)
                time.sleep(1)
                assert core_name() == ['external.bin'], 'Unchanged OK resent a stale filename'

                # Open-file deletion must still dismiss the dialog before its timer fires.
                time.sleep(1)
                dialog, edit, apply_button = open_details('external.bin')
                assert ec.call(C['EC_OP_PARTFILE_DELETE'], [tag(C['EC_TAG_PARTFILE'],
                               bytes.fromhex('0123456789abcdef0123456789abcdef'), 9)])[0] == C['EC_OP_NOOP']
                eventually(lambda: find(app, 'File Details') is None, 'Removed file left details open')
                time.sleep(5.5)  # A stopped dialog timer must not fire against the removed file.
                assert gui.poll() is None, console_path.read_text()
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
    print(('Local' if local_mode else 'Remote') + ' rename passed: pending edits, return to original, Apply/OK, refresh, Cancel, rejection/correction, Cleanup, navigation and removal')


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
    import ECWire  # Checks the lazily loaded cryptography prerequisite before any launch.
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
                                    gui, daemon, *(['--local'] if '--local' in sys.argv else []), '--session'],
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
                run(gui, daemon, Path(temporary), Atspi, local_mode='--local' in sys.argv)
        retry_port_collisions(attempt)
    finally:
        if registry.poll() is None:
            registry.terminate()
            try:
                registry.wait(timeout=10)
            except subprocess.TimeoutExpired:
                registry.kill()
                registry.wait()
    return 0


if __name__ == '__main__':
    sys.exit(main())
