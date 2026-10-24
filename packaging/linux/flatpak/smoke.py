#!/usr/bin/env python3
"""Exercise the installed bundle on an isolated CI display and session bus.

GNOME uses its headless backend; KWin nests in Xvfb. Only the test compositor
uses software-rendering overrides. The app retains its shipped permissions.
Screenshots are review evidence, not a pixel-perfect rendering assertion.
"""

import configparser
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time



APP = "org.amule.aMule"
PROCESSES = []
LOGS = []


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, timeout=15, **kwargs).strip()


def start(name, args, env):
    log = (OUT / f"{name}.log").open("w")
    LOGS.append(log)
    process = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env)
    PROCESSES.append(process)
    return process


def wait_for(description, predicate, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if any(p.poll() is not None for p in PROCESSES):
            raise RuntimeError(f"A process exited while waiting for {description}")
        try:
            value = predicate()
            if value:
                return value
        except (subprocess.SubprocessError, GLib.Error):
            pass
        time.sleep(0.5)
    raise RuntimeError(f"Timed out waiting for {description}")


def shell_eval(expression):
    value = run("gdbus", "call", "--session", "--dest", "org.gnome.Shell",
                "--object-path", "/org/gnome/Shell", "--method",
                "org.gnome.Shell.Eval", expression)
    ok, result = GLib.Variant.parse(None, value, None, None).unpack()
    if not ok:
        raise RuntimeError(f"GNOME evaluation failed: {result}")
    return result


def gnome_windows():
    return json.loads(shell_eval("global.get_window_actors().map(a => "
                                 "({title:a.meta_window.get_title() || '', "
                                 "wayland:a.meta_window.get_client_type() === "
                                 "imports.gi.Meta.WindowClientType.WAYLAND}))"))


def focus_gnome(title):
    shell_eval("Main.overview.hide(); global.get_window_actors().find(a => "
               f"(a.meta_window.get_title() || '').includes({json.dumps(title)}))"
               ".meta_window.activate(global.get_current_time()); true")
    time.sleep(1)
    # With no physical input, the startup overview can retain Clutter focus
    # even after it is hidden. Release it so keys reach the Wayland client.
    shell_eval("global.stage.set_key_focus(null); true")


def title_seen(title):
    if SESSION == "gnome-wayland":
        return any(title in w["title"] and w["wayland"] for w in gnome_windows())
    if SESSION == "kde-wayland":
        # Native xdg_toplevel traffic proves the app did not fall back to Xwayland.
        titles = {}
        for match in re.finditer(r'xdg_toplevel[#@](\d+)\.(set_title\("([^"\n]*)"\)|destroy\(\))',
                                 (OUT / "amule.log").read_text(errors="replace")):
            if match[3] is not None:
                titles[match[1]] = match[3]
            else:
                titles.pop(match[1], None)
        return any(title in value for value in titles.values())
    return subprocess.run(["xdotool", "search", "--onlyvisible", "--name", title],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          text=True, timeout=5).stdout.strip()


def key(chord):
    if SESSION != "gnome-wayland":
        run("xdotool", "key", "--clearmodifiers", chord)
        time.sleep(0.3)
        return
    # Mutter's virtual keyboard feeds native Wayland windows. XTest cannot.
    keysyms = {"alt": 0xffe9, "ctrl": 0xffe3, "Escape": 0xff1b,
               "Page_Down": 0xff56, "Tab": 0xff09, "Return": 0xff0d, "F4": 0xffc1}
    values = [keysyms.get(k, ord(k) if len(k) == 1 else 0) for k in chord.split("+")]
    for state, sequence in [("PRESSED", values), ("RELEASED", list(reversed(values)))]:
        for value in sequence:
            shell_eval("global.smokeKeyboard.notify_keyval(imports.gi.GLib.get_monotonic_time(), "
                       f"{value}, imports.gi.Clutter.KeyState.{state}); true")
    time.sleep(0.3)


def capture(name):
    # Let map/resize animations settle before recording review evidence.
    time.sleep(1)
    path = OUT / f"{name}.png"
    if SESSION == "gnome-wayland":
        value = run("gdbus", "call", "--session", "--dest", "org.gnome.Shell.Screenshot",
                    "--object-path", "/org/gnome/Shell/Screenshot", "--method",
                    "org.gnome.Shell.Screenshot.Screenshot", "false", "false", str(path))
        assert GLib.Variant.parse(None, value, None, None).unpack()[0], value
    else:
        run("import", "-window", "root", str(path))
    with Image.open(path) as image:
        assert min(image.size) >= 600, f"Unexpected screenshot dimensions: {image.size}"
        assert max(ImageStat.Stat(image.convert("RGB")).stddev) > 5, "Blank display"
    print(f"Captured {path.name}", flush=True)


def main():
    global SESSION, OUT, GLib, Image, ImageStat
    from gi.repository import GLib
    from PIL import Image, ImageStat
    SESSION, output_dir = sys.argv[1:]
    OUT = Path(output_dir).resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    assert SESSION in {"x11", "gnome-wayland", "kde-wayland"}
    host = os.environ.copy()
    runtime = OUT / "runtime"
    runtime.mkdir(mode=0o700)
    host["XDG_RUNTIME_DIR"] = str(runtime)
    host.pop("WAYLAND_DISPLAY", None)
    host.pop("DISPLAY", None)
    host.update(LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe",
                GSETTINGS_BACKEND="memory", XDG_CONFIG_HOME=str(OUT / "host-config"),
                XDG_SESSION_TYPE="x11" if SESSION == "x11" else "wayland",
                XDG_CURRENT_DESKTOP={"x11": "OPENBOX", "gnome-wayland": "GNOME",
                                     "kde-wayland": "KDE"}[SESSION])
    os.environ["XDG_RUNTIME_DIR"] = str(runtime)
    # Helpers activated on this private bus must use the test session's runtime,
    # rather than the environment inherited when dbus-run-session started.
    run("dbus-update-activation-environment", *(f"{key}={host[key]}" for key in
        ["XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "GSETTINGS_BACKEND"]))

    # Refuse production or symlinked profiles before launching any sandbox.
    config = prepare_profile(Path.home(), SESSION)
    permissions = run("flatpak", "info", "--user", "--show-permissions", APP)
    (OUT / "permissions.ini").write_text(permissions + "\n")
    metadata = configparser.ConfigParser()
    metadata.read_string(permissions)
    devices = metadata.get("Context", "devices", fallback="").split(";")
    assert "dri" not in devices and "all" not in devices, permissions
    filesystems = set(filter(None, metadata.get("Context", "filesystems").split(";")))
    assert filesystems == {"~/.aMule:create"}, permissions
    # No --nodevice override: check the actual installed manifest and sandbox.
    run("flatpak", "run", "--command=sh", APP, "-c", "test ! -e /dev/dri")

    if SESSION == "gnome-wayland":
        start("compositor", ["gnome-shell", "--wayland", "--headless", "--no-x11",
                             "--wayland-display=smoke-0", "--virtual-monitor=1280x960",
                             "--unsafe-mode"], host)
        # Unsafe mode is confined to this disposable session bus; it enables the
        # compositor's test input and screenshot interfaces, not app permissions.
        wait_for("GNOME test API", lambda: shell_eval("true") == "true")
        shell_eval("global.smokeKeyboard = global.stage.context.get_backend().get_default_seat()"
                   ".create_virtual_device(imports.gi.Clutter.InputDeviceType.KEYBOARD_DEVICE); "
                   "Main.overview.hide(); true")
    else:
        host["DISPLAY"] = ":99"
        os.environ["DISPLAY"] = host["DISPLAY"]
        start("xvfb", ["Xvfb", ":99", "-screen", "0", "1280x960x24", "-nolisten", "tcp"], host)
        wait_for("Xvfb", lambda: run("xdpyinfo", env=host))
        if SESSION == "kde-wayland":
            host["QT_QPA_PLATFORM"] = "xcb"
            start("compositor", ["kwin_wayland", "--x11-display=:99", "--width=1280",
                                 "--height=960", "--socket=smoke-0", "--no-lockscreen"], host)
        else:
            start("compositor", ["openbox"], host)
            wait_for("X11 window manager", lambda: "_NET_SUPPORTING_WM_CHECK(WINDOW)" in
                     run("xprop", "-root", "_NET_SUPPORTING_WM_CHECK"))

    app_env = os.environ.copy()
    for variable in ["LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER", "WAYLAND_DISPLAY", "DISPLAY"]:
        app_env.pop(variable, None)
    options = []
    if SESSION != "x11":
        wait_for("Wayland socket", lambda: (runtime / "smoke-0").is_socket())
        app_env["WAYLAND_DISPLAY"] = "smoke-0"
        options = ["--env=GDK_BACKEND=wayland", "--env=WAYLAND_DEBUG=1"]
    else:
        app_env["DISPLAY"] = ":99"
        options = ["--env=GDK_BACKEND=x11"]
    app_env.update(LANG="C.UTF-8", LC_ALL="C.UTF-8", XDG_SESSION_TYPE=host["XDG_SESSION_TYPE"],
                   XDG_CURRENT_DESKTOP=host["XDG_CURRENT_DESKTOP"])

    app = start("amule", ["flatpak", "run", *options, APP], app_env)
    if SESSION == "kde-wayland":
        window = wait_for("KWin nested window", lambda: run(
            "xdotool", "search", "--onlyvisible", "--pid", str(PROCESSES[1].pid))).splitlines()[-1]
        run("xdotool", "windowfocus", window)
    # A fresh configuration also shows a one-time version notice before the
    # main frame exists. Dismiss only that expected dialog, not arbitrary errors.
    first_window = wait_for("aMule startup", lambda: "info" if title_seen("^Info$" if SESSION == "x11" else "Info")
                            else "main" if title_seen("aMule") else None)
    if first_window == "info":
        if SESSION == "gnome-wayland":
            focus_gnome("Info")
        capture("00-version-notice")
        key("Return")
    wait_for("native aMule window", lambda: title_seen("aMule"))
    time.sleep(3)
    if SESSION == "gnome-wayland":
        focus_gnome("aMule")
    elif SESSION == "x11":
        window = run("xdotool", "search", "--onlyvisible", "--class", "amule").splitlines()[-1]
        run("xdotool", "windowactivate", "--sync", window)
    capture("01-startup")
    key("alt+s")
    capture("02-search")
    key("alt+n")
    key("Tab")
    key("Page_Down")
    capture("03-networks-scroll")
    key("alt+p")
    wait_for("Preferences dialog", lambda: title_seen("Preferences"))
    if SESSION == "gnome-wayland":
        focus_gnome("Preferences")
    capture("04-preferences")
    if SESSION == "gnome-wayland":
        # Bare Shell sessions do not necessarily have the distribution's
        # Alt+F4 binding. Use Mutter's normal window-close request instead.
        shell_eval("global.get_window_actors().find(a => "
                   "a.meta_window.get_title() === 'Preferences').meta_window"
                   ".delete(global.get_current_time()); true")
    else:
        key("alt+F4")
    wait_for("Preferences to close", lambda: not title_seen("Preferences"))
    if SESSION == "gnome-wayland":
        focus_gnome("aMule")
        shell_eval("{ const w = global.get_window_actors().find(a => "
                   "(a.meta_window.get_title() || '').includes('aMule')).meta_window; "
                   "w.unmaximize(imports.gi.Meta.MaximizeFlags.BOTH); "
                   "w.move_resize_frame(false, 20, 20, 1024, 740); } true")
    else:
        run("xdotool", "windowsize", window, "1024", "740")
        wait_for("resized X11 window/display", lambda:
                 {"WIDTH=1024", "HEIGHT=740"}.issubset(set(
                     run("xdotool", "getwindowgeometry", "--shell", window).splitlines())))
    if SESSION == "gnome-wayland":
        wait_for("resized GNOME window", lambda: json.loads(shell_eval(
            "{ const w = global.get_window_actors().find(a => "
            "(a.meta_window.get_title() || '').includes('aMule')).meta_window; "
            "const r = w.get_frame_rect(); [r.width, r.height]; }")) == [1024, 740])
    time.sleep(1)
    capture("05-resized")
    if SESSION == "gnome-wayland":
        for width, height in [(640, 400), (1024, 740), (800, 480), (1024, 740)]:
            shell_eval("{ const w = global.get_window_actors().find(a => "
                       "(a.meta_window.get_title() || '').includes('aMule')).meta_window; "
                       f"w.move_resize_frame(false, 20, 20, {width}, {height}); }} true")
            wait_for("resize cycle", lambda: json.loads(shell_eval(
                "{ const w = global.get_window_actors().find(a => "
                "(a.meta_window.get_title() || '').includes('aMule')).meta_window; "
                "const r = w.get_frame_rect(); [r.width, r.height]; }")) == [width, height])
            time.sleep(0.5)
        capture("06-restored")
    key("ctrl+q")
    assert app.wait(timeout=30) == 0, "aMule did not shut down cleanly"
    saved = configparser.ConfigParser(interpolation=None)
    saved.read(config / "amule.conf")
    assert saved.get("eMule", "Nick") == "Flatpak smoke profile"
    assert saved.has_option("eMule", "IncomingDir")
    assert (config / "preferences.dat").stat().st_size > 0
    persisted = run("flatpak", "run", "--command=sh", APP, "-ec",
                    'test -s "$HOME/.aMule/preferences.dat"; '
                    'test -d "$HOME/.aMule/Incoming"; test -d "$HOME/.aMule/Temp"; '
                    'cat "$HOME/.aMule/amule.conf"')
    assert persisted == (config / "amule.conf").read_text().strip()
    check_drawing_failures(OUT / "amule.log")
    (OUT / "result.txt").write_text(f"PASS: {SESSION}; no DRI; native window; keyboard navigation; "
                                   "Preferences; resize; screenshots; clean shutdown; no drawing failures.\n"
                                   "Visual correctness and hardware performance require review.\n")
    print((OUT / "result.txt").read_text(), flush=True)


def prepare_profile(home, session):
    # Match the application's default path; never overwrite a user's profile.
    config = home / ".aMule"
    if config.is_symlink():
        raise RuntimeError("Smoke test refuses a symlinked aMule profile")
    config.mkdir(parents=True, exist_ok=True)
    if any(config.iterdir()):
        raise RuntimeError("Smoke test requires an empty aMule profile")
    with (config / "amule.conf").open("x") as file:
        file.write(
            "[eMule]\nNick=Flatpak smoke profile\nFirstRunWizardDone=1\nAutoconnect=0\nNewVersionCheck=0\n"
            "ConfirmExit=0\nStartupMinimized=0\nEnableTrayIcon=0\nUPnPEnabled=0\n"
            "ConnectToED2K=0\nConnectToKad=0\nGeoIPEnabled=0\nGeoIPAutoUpdate=0\n"
            "IPFilterAutoLoad=0\n")
        if session == "x11":
            # Openbox preserves maximized windows' size on a resize request.
            file.write("[Razor_Preferences]\nMaximized=0\nMAIN_X_SIZE=1200\n"
                       "MAIN_Y_SIZE=860\nMAIN_X_POS=20\nMAIN_Y_POS=30\n")
    return config


def check_drawing_failures(log_path):
    failures = [line for line in log_path.read_text(errors="replace").splitlines()
                if "drawing failure" in line]
    if failures:
        raise RuntimeError("aMule drawing failure:\n" + "\n".join(failures))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        try:
            capture("failure")
        except Exception as error:
            print(f"Could not capture failure screenshot: {error}", flush=True)
        raise
    finally:
        for process in reversed(PROCESSES):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for log in LOGS:
            log.close()
