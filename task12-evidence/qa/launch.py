#!/usr/bin/env python3
"""Launch the isolated QA engine + UI for Task-12 evidence runs.

Pipe: \\.\pipe\openrgb2-qa-engine (separate from any running
production engine on \\.\pipe\openrgb2-engine).
"""
import os, subprocess, sys, time

ROOT = r"C:\Users\daniel\.codex\worktrees\studio-import\openRGB2"
RUN  = os.path.join(ROOT, "task12-evidence", "qa", "run")
PIPE = r"\\.\pipe\openrgb2-qa-engine"
ENGINE = os.path.join(ROOT, "build-studio", "src", "engine",
                      "Release", "openrgb2-engine.exe")
UI = os.path.join(ROOT, "build-studio", "src", "ui", "Release",
                  "openrgb2-ui.exe")
QT_BIN = r"C:\Qt\6.8.3\msvc2022_64\bin"
QML_ROOT = os.path.join(ROOT, "build-studio")

os.makedirs(RUN, exist_ok=True)

def launch_engine():
    return subprocess.Popen([
        ENGINE,
        "--ipc", "--no-scan", "--no-instance-guard",
        "--no-power-monitor",
        "--pipe", PIPE,
        "--workspace-root", os.path.join(RUN, "workspaces"),
        "--devices-dir", os.path.join(ROOT, "src", "core",
                                      "presets", "devices"),
        "--effects-dir", os.path.join(ROOT, "src", "core",
                                      "presets", "effects"),
        "--state", os.path.join(RUN, "engine-state.json"),
    ], cwd=RUN,
       stdout=open(os.path.join(RUN, "engine.log"), "ab"),
       stderr=subprocess.STDOUT)

def launch_ui(qa_port=5584):
    env = dict(os.environ)
    env["PATH"] = QT_BIN + os.pathsep + env.get("PATH", "")
    env["QML2_IMPORT_PATH"] = QML_ROOT
    env["OPENRGB2_QA"] = "1"
    env["OPENRGB2_QA_PORT"] = str(qa_port)
    env["DLS_PRESET_DIR"] = os.path.join(ROOT, "src", "core",
                                         "presets", "devices")
    env["DLS_EFFECT_DIR"] = os.path.join(ROOT, "src", "core",
                                         "presets", "effects")
    env["QSG_INFO"] = "1"  # log the RHI backend in ui.log
    return subprocess.Popen([UI, "--pipe", PIPE, "--qa",
                             str(qa_port)],
                            cwd=RUN, env=env,
                            stdout=open(os.path.join(RUN,
                                                     "ui.log"), "ab"),
                            stderr=subprocess.STDOUT)

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "both"
    procs = []
    if which in ("engine", "both"):
        procs.append(("engine", launch_engine()))
        time.sleep(1.5)
    if which in ("ui", "both"):
        procs.append(("ui", launch_ui()))
    for name, p in procs:
        print(f"{name} pid={p.pid}")
    # keep the launcher alive; children die with the console
    try:
        for _, p in procs:
            p.wait()
    except KeyboardInterrupt:
        for _, p in procs:
            p.terminate()
