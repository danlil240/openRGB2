#!/usr/bin/env python3
"""Launch the PACKAGED engine + UI for Task-12 re-verification.

Pipe: \\.\pipe\openrgb2-pkg-engine (built with chr(92) so no shell
quoting layer can mangle it). UI QA socket: 127.0.0.1:5587.

The launch environment is deliberately CLEAN — PATH is only the
package dir + System32 + Windows (the claim backed by
package-run/launch-env.json). Qt/QML resolve from the staged tree.
"""
import json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
PKG  = os.path.join(ROOT, "task12-evidence", "package",
                    "OpenRGB2-0.1.0-win64")
RUN  = os.path.join(ROOT, "task12-evidence", "package-run")
BSL  = chr(92)
PIPE = BSL + BSL + "." + BSL + "pipe" + BSL + "openrgb2-pkg-engine"
ENGINE = os.path.join(PKG, "openrgb2-engine.exe")
UI = os.path.join(PKG, "openrgb2-ui.exe")
STATE = os.path.join(PKG, "profiles", "local")

os.makedirs(RUN, exist_ok=True)
os.makedirs(STATE, exist_ok=True)


def clean_env(scale=None):
    """PATH = <pkg>;System32;Windows — no toolchain, no dev PATH."""
    e = {"PATH": os.pathsep.join([
            PKG, r"C:\Windows\System32", r"C:\Windows"]),
         "SYSTEMROOT": os.environ.get("SYSTEMROOT", r"C:\Windows"),
         "QML2_IMPORT_PATH": os.path.join(PKG, "qml"),
         "OPENRGB2_QA": "1",
         "OPENRGB2_QA_PORT": "5587",
         "QSG_INFO": "1"}
    if scale:
        e["QT_SCALE_FACTOR"] = scale
    return e


def launch_engine():
    return subprocess.Popen([
        ENGINE,
        "--ipc", "--no-scan", "--no-instance-guard",
        "--no-power-monitor",
        "--pipe", PIPE,
        "--workspace-root", os.path.join(STATE, "workspaces"),
        "--devices-dir", os.path.join(PKG, "presets", "devices"),
        "--effects-dir", os.path.join(PKG, "presets", "effects"),
        "--state", os.path.join(STATE, "engine-state.json"),
    ], cwd=PKG, env=clean_env(),
       stdout=open(os.path.join(RUN, "pkg-engine.log"), "ab"),
       stderr=subprocess.STDOUT)


def launch_ui(scale=None, log="pkg-ui.log"):
    return subprocess.Popen([UI, "--pipe", PIPE, "--qa", "5587"],
                            cwd=PKG, env=clean_env(scale),
                            stdout=open(os.path.join(RUN, log), "ab"),
                            stderr=subprocess.STDOUT)


def record_env():
    ev = {"engine": [ENGINE,
            "--ipc", "--no-scan", "--no-instance-guard",
            "--no-power-monitor", "--pipe", PIPE,
            "--workspace-root",
            os.path.join(STATE, "workspaces"),
            "--devices-dir", os.path.join(PKG, "presets",
                                          "devices"),
            "--effects-dir", os.path.join(PKG, "presets",
                                          "effects"),
            "--state", os.path.join(STATE, "engine-state.json")],
          "ui": [UI, "--pipe", PIPE, "--qa", "5587"],
          "env": clean_env(),
          "cwd": PKG,
          "note": "PATH=<pkg>;C:\\Windows\\System32;C:\\Windows "
                  "— clean-PATH claim evidence"}
    with open(os.path.join(RUN, "launch-env.json"), "w",
              encoding="utf-8") as f:
        json.dump(ev, f, indent=2)


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "both"
    record_env()
    procs = []
    if which in ("engine", "both"):
        procs.append(("engine", launch_engine()))
        time.sleep(1.5)
    if which in ("ui", "both"):
        scale = sys.argv[2] if len(sys.argv) > 2 else None
        procs.append(("ui", launch_ui(scale)))
    for name, p in procs:
        print(f"{name} pid={p.pid}")
    with open(os.path.join(RUN, "pkg-pids.txt"), "w") as f:
        for name, p in procs:
            f.write(f"{name} {p.pid}\n")
