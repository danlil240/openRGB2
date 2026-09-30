#!/usr/bin/env python3
"""Run openrgb2-ctl against the QA pipe (keeps the pipe name
out of the bash command line)."""
import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
CTL = os.path.join(ROOT, "build-studio", "tools", "engine_ctl",
                   "Release", "openrgb2-ctl.exe")
PIPE = r"\\.\pipe\openrgb2-qa-engine"

r = subprocess.run([CTL, "--pipe", PIPE] + sys.argv[1:],
                   capture_output=True, text=True, timeout=60)
print(r.stdout)
sys.stderr.write(r.stderr)
sys.exit(r.returncode)
