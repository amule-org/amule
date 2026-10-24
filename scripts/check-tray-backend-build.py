#!/usr/bin/env python3
"""Compile the wxTaskBarIcon consumers using an existing Linux GUI build database.

CI installs libayatana, so its normal GUI builds exclude this backend. Override
only the generated config in a temporary include directory: keep the configured
compiler, definitions and include paths for both amule and amulegui.
"""

import argparse
import json
import shlex
import subprocess
import tempfile
from pathlib import Path


SOURCES = {"MuleTrayIcon.cpp", "amuleDlg.cpp"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("--require-remote-gui", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    selected = []
    for entry in entries:
        if Path(entry["file"]).name in SOURCES:
            command = entry.get("arguments") or shlex.split(entry["command"])
            # Only probe real GUI consumers here.
            if not any("CMakeFiles/amule.dir/" in c or "CMakeFiles/amulegui.dir/" in c
                       for c in command):
                continue
            gui = "amulegui" if any(c.startswith("-DCLIENT_GUI") for c in command) else "amule"
            selected.append((entry, command, gui))
    guis = ("amule", "amulegui") if args.require_remote_gui else ("amule",)
    required = {(gui, source) for gui in guis for source in SOURCES}
    found = {(gui, Path(entry["file"]).name) for entry, _, gui in selected}
    if not required <= found:
        parser.error("configure the required GUI targets first; missing: " +
                     ", ".join(f"{gui}/{source}" for gui, source in sorted(required - found)))

    with tempfile.TemporaryDirectory(prefix="amule-tray-backend-") as directory:
        config_dir = Path(directory)
        config = (build / "config.h").read_text()
        config = "\n".join(line for line in config.splitlines()
                           if not line.startswith("#define WITH_LIBAYATANA_APPINDICATOR")) + "\n"
        (config_dir / "config.h").write_text(config)
        for index, (entry, command, gui) in enumerate(selected):
            # Put the override ahead of the existing build-directory include,
            # after any compiler launcher and compiler executable.
            first_flag = next(i for i, value in enumerate(command) if value.startswith("-"))
            command[first_flag:first_flag] = ["-I" + str(config_dir)]
            # Never write objects or dependency files into the configured build.
            for flag in ("-o", "-MF", "-MT", "-MQ"):
                if flag in command:
                    command[command.index(flag) + 1] = str(config_dir / f"check-{index}.o")
            command = [value for value in command if value not in ("-MD", "-MMD", "-MP")]
            command.append("-fsyntax-only")
            print(f"Checking wxTaskBarIcon without libayatana: {gui}/{Path(entry['file']).name}",
                  flush=True)
            subprocess.run(command, cwd=entry["directory"], check=True, timeout=180)
    print(f"PASS: {len(selected)} wxTaskBarIcon translation units")


if __name__ == "__main__":
    main()
