#!/usr/bin/env python3
"""Run on a private D-Bus and X server; no real user files or portals are touched."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

import gi
from gi.repository import Gio, GLib

DRIVER = sys.argv.pop(1)
XML = '''<node><interface name="org.freedesktop.portal.FileChooser">
<method name="OpenFile"><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface></node>'''


class PortalTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.context = GLib.MainContext.new()
        cls.ready = threading.Event()
        cls.mode = "success"
        cls.calls = []

        def serve():
            cls.context.push_thread_default()
            cls.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
            result = cls.bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "RequestName",
                GLib.Variant("(su)", ("org.freedesktop.portal.Desktop", 4)),
                None, Gio.DBusCallFlags.NONE, 5000, None)
            assert result.unpack()[0] == 1
            info = Gio.DBusNodeInfo.new_for_xml(XML)
            cls.registration = cls.bus.register_object("/org/freedesktop/portal/desktop",
                info.interfaces[0], cls.open_folder, None, None)
            cls.loop = GLib.MainLoop.new(cls.context, False)
            cls.ready.set()
            cls.loop.run()
            cls.bus.unregister_object(cls.registration)
            cls.context.pop_thread_default()
        cls.thread = threading.Thread(target=serve)
        cls.thread.start()
        assert cls.ready.wait(5)

    @classmethod
    def tearDownClass(cls):
        cls.loop.quit()
        cls.thread.join(5)

    @classmethod
    def open_folder(cls, connection, sender, path, interface, method, parameters, invocation):
        parent, title, options = parameters.unpack()
        cls.calls.append(options)
        request = "/org/freedesktop/portal/desktop/request/" + sender[1:].replace(".", "_") + "/" + options["handle_token"]
        # An unrelated response must not satisfy this chooser request.
        connection.emit_signal(sender, request + "_unrelated", "org.freedesktop.portal.Request",
            "Response", GLib.Variant("(ua{sv})", (0, {"uris": GLib.Variant("as", ["file:///unapproved"])})))
        results = {} if cls.mode == "cancel" else {"uris": GLib.Variant("as", [cls.folder.as_uri()])}
        if cls.mode == "empty":
            results = {"uris": GLib.Variant("as", [])}
        # Deliberately emit before returning the method reply to exercise the race.
        connection.emit_signal(sender, request, "org.freedesktop.portal.Request", "Response",
            GLib.Variant("(ua{sv})", (1 if cls.mode == "cancel" else 0, results)))
        invocation.return_value(GLib.Variant("(o)", (request,)))

    def run_case(self, mode, writable=False, sparse=False):
        with tempfile.TemporaryDirectory(prefix="amule-portal-test-", dir=Path(DRIVER).parent) as directory:
            base = Path(directory)
            profile = base / "profile"
            profile.mkdir()
            type(self).folder = base / "approved folder #1"
            self.folder.mkdir()
            os.setxattr(self.folder, "user.document-portal.host-path", b"/media/USB/My Documents")
            type(self).mode = mode
            if writable:
                (profile / "folder-access.ini").write_text("Count=1\nFolder0=" + str(self.folder) +
                    "\nHostFolder0=/media/USB/My Documents\nWritable0=1\n")
            if sparse:
                downloads = base / "downloads"
                downloads.mkdir()
                # A missing entry must not shift its write permission onto a
                # later read-only share, or make Remember overwrite another row.
                (profile / "folder-access.ini").write_text(
                    "Count=3\nWritable0=1\nFolder1=" + str(self.folder) +
                    "\nHostFolder1=/media/USB/My Documents\nWritable1=" + ("1" if writable else "0") +
                    "\nFolder2=" + str(downloads) + "\nWritable2=1\n")
            env = dict(os.environ, FLATPAK_ID="org.amule.aMule",
                XDG_CONFIG_HOME=str(base / "config"), XDG_DATA_HOME=str(base / "data"),
                XDG_CACHE_HOME=str(base / "cache"), GIO_USE_VFS="local",
                GSETTINGS_BACKEND="memory", NO_AT_BRIDGE="1")
            proc = subprocess.run([DRIVER, str(profile), "/media/USB", "write" if writable else "read"],
                env=env, text=True, capture_output=True, timeout=15)
            if proc.returncode == 77:
                self.skipTest(proc.stderr)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertNotIn("Error:", proc.stderr)
            self.assertNotIn("CRITICAL", proc.stderr)
            self.assertTrue(self.calls[-1]["directory"])
            self.assertFalse(self.calls[-1]["multiple"])
            self.assertEqual(bytes(self.calls[-1]["current_folder"]), b"/media/USB\0")
            record = profile / "folder-access.ini"
            if mode in ("cancel", "empty"):
                self.assertIn("SELECTED=\n", proc.stdout)
                self.assertFalse(record.exists(), "Cancelled approval must not be persisted")
            else:
                self.assertIn("SELECTED=" + str(self.folder), proc.stdout)
                self.assertIn("HOST=/media/USB/My Documents", proc.stdout)
                self.assertEqual(record.stat().st_mode & 0o777, 0o600)
                contents = record.read_text()
                index = "1" if sparse else "0"
                self.assertIn("Count=" + ("3" if sparse else "1"), contents)
                self.assertIn("Writable" + index + "=" + ("1" if writable else "0"), contents)
                self.assertIn("HostFolder" + index + "=/media/USB/My Documents", contents)
                if sparse:
                    self.assertIn("Folder2=" + str(downloads), contents)
                    self.assertNotIn("Folder0=", contents)

    def test_failed_atomic_save_preserves_profile_and_can_retry(self):
        with tempfile.TemporaryDirectory(prefix="amule-profile-save-") as directory:
            env = dict(os.environ, FLATPAK_ID="org.amule.aMule",
                GIO_USE_VFS="local", GSETTINGS_BACKEND="memory", NO_AT_BRIDGE="1")
            proc = subprocess.run([DRIVER, directory, "", "atomic"], env=env,
                capture_output=True, text=True, timeout=15)
            if proc.returncode == 77:
                self.skipTest(proc.stderr)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            original = Path(directory) / "atomic.conf"
            backup = Path(directory) / "atomic.conf.bak"
            self.assertEqual(original.read_bytes(), backup.read_bytes())
            self.assertEqual(original.read_bytes(), (Path(directory) / "atomic.conf.credits.bak").read_bytes())
            self.assertIn(b"value=", original.read_bytes())
            self.assertEqual(original.stat().st_mode & 0o777, 0o600)
            self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
            self.assertFalse(list(Path(directory).glob("*.tmp.*")))

    def test_read_only_default_and_early_response(self):
        self.run_case("success")

    def test_write_requires_explicit_mode(self):
        self.run_case("success", True)

    def test_missing_registry_entry_does_not_grant_write_to_share(self):
        self.run_case("success", sparse=True)

    def test_sparse_registry_preserves_writable_destination(self):
        self.run_case("success", writable=True, sparse=True)

    def test_sparse_registry_enforces_both_permissions(self):
        with tempfile.TemporaryDirectory(prefix="amule-registry-test-", dir=Path(DRIVER).parent) as directory:
            base = Path(directory)
            profile = base / "profile"
            profile.mkdir()
            readonly = base / "readonly"
            readonly.mkdir()
            document = readonly / "budget.xlsx"
            document.write_bytes(b"existing office document")
            downloads = base / "downloads"
            downloads.mkdir()
            record = profile / "folder-access.ini"
            record.write_text("Count=3\nWritable0=1\nFolder1=" + str(readonly) +
                "\nWritable1=0\nFolder2=" + str(downloads) + "\nWritable2=1\n")
            original_registry = record.read_bytes()
            env = dict(os.environ, FLATPAK_ID="org.amule.aMule",
                XDG_CONFIG_HOME=str(base / "config"), XDG_DATA_HOME=str(base / "data"),
                XDG_CACHE_HOME=str(base / "cache"), GIO_USE_VFS="local",
                GSETTINGS_BACKEND="memory", NO_AT_BRIDGE="1")
            proc = subprocess.run([DRIVER, str(profile), str(base), "registry"],
                env=env, capture_output=True, text=True, timeout=15)
            if proc.returncode == 77:
                self.skipTest(proc.stderr)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(document.read_bytes(), b"existing office document")
            self.assertEqual(record.read_bytes(), original_registry)
            self.assertEqual((downloads / "download.part").read_bytes(), b"partial download")

    def test_cancel_does_not_add_a_folder(self):
        self.run_case("cancel")

    def test_empty_result_does_not_add_a_folder(self):
        self.run_case("empty")


if __name__ == "__main__":
    unittest.main()
