#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Linux GUI regression check; run with the amule executable as the argument.

Requires Xvfb, ImageMagick import, tesseract, xwininfo and libXtst. Uses an
isolated offline profile, real EC download additions/removal, and real mouse
and keyboard input. Screenshots and console output stay in a temporary folder
printed on failure. This is an opt-in visual test, not part of headless CTest.
"""
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, EC, string, tag
from LogfilePathIntegrationTest import write_config


def run(binary):
    root = Path(tempfile.mkdtemp(prefix='amule-download-details-'))
    port, _ = write_config(root)
    for name in ('Incoming', 'Temp'):
        (root / name).mkdir()
    (root / 'lastversion').write_text('GIT\n')
    with (root / 'amule.conf').open('a') as config:
        config.write('\n[GUI]\n[GUI/TransferWnd]\nShowClientList=1\nSplitter=300\n')
    # Ask Xvfb for a free display rather than colliding with another GUI test.
    read_fd, write_fd = os.pipe()
    xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                             '1024x768x24'], pass_fds=(write_fd,), stderr=subprocess.DEVNULL)
    os.close(write_fd)
    display = ':' + os.read(read_fd, 32).decode().strip()
    os.close(read_fd)
    env = dict(os.environ, DISPLAY=display, LANG='en_US.UTF-8', LC_ALL='C.UTF-8')
    x = ctypes.CDLL('libX11.so.6')
    xt = ctypes.CDLL('libXtst.so.6')
    x.XOpenDisplay.restype = ctypes.c_void_p
    connection = x.XOpenDisplay(display.encode())
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    xt.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_ulong]
    xt.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    xt.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    console = (root / 'console.log').open('w')
    proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                            env=env, stdout=console, stderr=console)
    ec = None

    def key(code):
        xt.XTestFakeKeyEvent(connection, code, 1, 0)
        xt.XTestFakeKeyEvent(connection, code, 0, 0)
        x.XFlush(connection)

    def click(px, py):
        xt.XTestFakeMotionEvent(connection, -1, px, py, 0)
        xt.XTestFakeButtonEvent(connection, 1, 1, 0)
        xt.XTestFakeButtonEvent(connection, 1, 0, 0)
        x.XFlush(connection)
        time.sleep(0.3)

    def screen(name):
        assert proc.poll() is None, (root / 'console.log').read_text()
        filename = root / (name + '.png')
        subprocess.run(['import', '-window', 'root', str(filename)], env=env, check=True)
        return subprocess.check_output(['tesseract', str(filename), 'stdout'],
                                       stderr=subprocess.DEVNULL).decode()

    def expect(name, *texts):
        time.sleep(1.2)  # Allow the normal live-detail timer and deferred selection event.
        actual = screen(name)
        for text in texts:
            assert text in actual, (name, text, actual)
        return actual

    def tab(name, activate=True):
        # Locate tabs from OCR so resize checks do not depend on the sash position.
        filename = root / 'locate.png'
        subprocess.run(['import', '-window', 'root', str(filename)], env=env, check=True)
        rows = subprocess.check_output(['tesseract', str(filename), 'stdout', 'tsv'],
                                       stderr=subprocess.DEVNULL).decode().splitlines()
        for row in rows[1:]:
            fields = row.split('\t')
            if len(fields) == 12 and fields[11] == name:
                point = (int(fields[6]) + 10, int(fields[7]) + 5)
                if activate:
                    click(*point)
                return point
        raise AssertionError('Missing tab: ' + name)

    try:
        deadline = time.monotonic() + 25
        while ec is None:
            try:
                ec = EC(port)
            except OSError:
                assert proc.poll() is None
                assert time.monotonic() < deadline, 'GUI did not start'
                time.sleep(0.2)
        expect('empty', 'Select a download')
        hashes = [bytes.fromhex('0123456789abcdef0123456789abcdef'),
                  bytes.fromhex('1123456789abcdef0123456789abcdef')]
        for name, file_hash in zip(('pane-test.bin', 'second.bin'), hashes):
            assert ec.call(C['EC_OP_ADD_LINK'], [string(C['EC_TAG_STRING'],
                f'ed2k://|file|{name}|4096|{file_hash.hex()}|/')])[0] == C['EC_OP_NOOP']
        time.sleep(0.5)
        click(150, 170)
        key(110)  # Home selects the first visible download.
        expect('selected', 'Filename: pane-test.bin', 'Size: 4 KiB')
        tab('Transfers')
        expect('transfers', 'Waiting', 'Completed:', 'Transferred:')
        tab('Comments')
        expect('comments', 'No comments available.')
        tab('Sources')
        expect('sources', 'File sources:')
        tab('General')
        click(150, 170)
        xt.XTestFakeKeyEvent(connection, 37, 1, 0)  # Ctrl+A selects both rows.
        key(38)
        xt.XTestFakeKeyEvent(connection, 37, 0, 0)
        x.XFlush(connection)
        expect('multiple', 'Select a single download')
        click(150, 170)
        key(110)
        expect('single-again', 'Filename: pane-test.bin')
        # Filtering must clear the details of a now-hidden selection.
        filter_x, filter_y = tab('Filter:', activate=False)
        click(filter_x + 55, filter_y)
        for code in (38, 38, 38):  # 'aaa', which matches neither file
            key(code)
        expect('filtered', 'Select a download')
        xt.XTestFakeKeyEvent(connection, 37, 1, 0)  # Ctrl+A, Backspace
        key(38)
        xt.XTestFakeKeyEvent(connection, 37, 0, 0)
        key(22)
        time.sleep(0.5)  # The text-filter rebuild is deferred.
        click(150, 170)
        key(110)
        expect('restored', 'Filename: pane-test.bin')
        assert ec.call(C['EC_OP_PARTFILE_DELETE'], [tag(C['EC_TAG_PARTFILE'], hashes[0], 9)])[0] == C['EC_OP_NOOP']
        actual = expect('removed')
        assert 'Select a download' in actual or 'Filename: second.bin' in actual
        assert 'Filename: pane-test.bin' not in actual
        # Collapse must survive both frame resize and expansion afterwards.
        _, tab_y = tab('General', activate=False)
        click(18, tab_y - 35)
        assert 'General' not in expect('collapsed', 'File sources:')
        tree = subprocess.check_output(['xwininfo', '-root', '-tree'], env=env).decode()
        windows = re.findall(r'(0x[0-9a-f]+) "([^"\n]*)"', tree)
        window = next(int(wid, 16) for wid, title in windows if title.startswith('aMule'))
        x.XResizeWindow(connection, window, 950, 700)
        x.XFlush(connection)
        assert 'General' not in expect('collapsed-resized', 'File sources:')
        _, header_y = tab('sources:', activate=False)
        click(18, header_y)
        expect('expanded-resized', 'General')
        _, tab_y = tab('General', activate=False)
        click(18, tab_y - 35)
        assert 'General' not in expect('collapsed-before-restart', 'File sources:')
        ec.sock.close()
        ec = None
        proc.terminate()
        proc.wait(timeout=15)
        proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                env=env, stdout=console, stderr=console)
        time.sleep(3)
        assert 'General' not in expect('collapsed-after-restart', 'File sources:')
        _, header_y = tab('sources:', activate=False)
        click(18, header_y)
        expect('expanded-after-restart', 'General', 'Select a download')
        print('Download details selection, tabs, filtering, removal, collapsed resize and restart passed')
    except BaseException:
        print('GUI evidence:', root, file=sys.stderr)
        raise
    finally:
        if ec is not None:
            ec.sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        console.close()
        x.XCloseDisplay(connection)
        xvfb.terminate()
        xvfb.wait()


if __name__ == '__main__':
    missing = [name for name in ('Xvfb', 'import', 'tesseract', 'xwininfo')
               if not shutil.which(name)]
    if missing:
        print('Missing optional GUI-test tools:', ', '.join(missing))
        sys.exit(77)
    run(str(Path(sys.argv[1]).resolve()))
