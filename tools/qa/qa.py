#!/usr/bin/env python3
"""qa.py — command-line driver for the openrgb2-ui QA socket.

openrgb2-ui must be started with --qa (or OPENRGB2_QA=1). The app
then listens on 127.0.0.1:5583 (override: --qa <port>,
OPENRGB2_QA_PORT, or the --port flag here).

Every op sends one JSON line and prints the reply line (pretty).
Selectors: bare string = objectName ("navLighting"), or "text:Save",
"type:AppSlider", "path:0.2.1", or a JSON object
'{"name":"x","index":0}' / '{"text":"Save","type":"AppButton"}'.

usage:
    qa.py ping
    qa.py state
    qa.py tree [--all] [--under SEL]
    qa.py find SEL
    qa.py click SEL [--double] [--button right]
    qa.py clickxy X Y [--double]
    qa.py set SEL PROP VALUE
    qa.py get SEL [PROP]
    qa.py geom SEL
    qa.py type SEL TEXT [--clear]
    qa.py key "Ctrl+2" | "Tab" | "a"
    qa.py scroll SEL DY [DX]      (DY in wheel steps, -3 = down 3)
    qa.py shot [PATH.png]
    qa.py wait SEL [PROP VALUE] [--timeout MS]
    qa.py eval "this.currentView = 2"
    qa.py raw '{"op":"tree","all":true}'
"""

import json
import os
import socket
import sys

DEFAULT_PORT = 5583
HOST = "127.0.0.1"


def die(msg, code=2):
    print(f"qa: {msg}", file=sys.stderr)
    sys.exit(code)


def parse_value(s):
    """CLI value -> JSON value (numbers/bools/null decoded)."""
    try:
        return json.loads(s)
    except (json.JSONDecodeError, ValueError):
        return s


def parse_sel(s):
    """Selector string -> JSON selector (object if given as JSON)."""
    s = s.strip()
    if s.startswith("{"):
        try:
            return json.loads(s)
        except json.JSONDecodeError:
            die(f"bad JSON selector: {s}")
    return s


def send(req, port):
    line = json.dumps(req, separators=(",", ":")) + "\n"
    try:
        with socket.create_connection((HOST, port), timeout=35) as sock:
            sock.sendall(line.encode("utf-8"))
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = sock.recv(1 << 16)
                if not chunk:
                    die("connection closed before reply", 1)
                buf += chunk
    except OSError as e:
        die(f"cannot reach QA socket {HOST}:{port} — "
            f"is openrgb2-ui running with --qa? ({e})", 1)
    try:
        return json.loads(buf.decode("utf-8"))
    except json.JSONDecodeError:
        die(f"unparseable reply: {buf!r}", 1)


def build(op, args):
    """Map CLI args to a request dict."""
    if op == "ping":
        return {"op": "ping"}
    if op == "state":
        return {"op": "state"}
    if op == "tree":
        req = {"op": "tree"}
        rest = []
        for a in args:
            if a == "--all":
                req["all"] = True
            elif a == "--under":
                pass
            else:
                rest.append(a)
        if "--under" in args:
            i = args.index("--under")
            req["under"] = parse_sel(args[i + 1])
        return req
    if op == "find":
        return {"op": "find", "sel": parse_sel(args[0])}
    if op in ("click", "clickxy"):
        req = {"op": "click"}
        pos = 0
        if op == "clickxy" or (args and args[0].lstrip("-").replace(
                ".", "", 1).isdigit() and len(args) >= 2
                and args[1].lstrip("-").replace(".", "", 1).isdigit()):
            req["x"] = float(args[0])
            req["y"] = float(args[1])
            pos = 2
        else:
            req["sel"] = parse_sel(args[0])
            pos = 1
        for a in args[pos:]:
            if a == "--double":
                req["double"] = True
            elif a.startswith("--button="):
                req["button"] = a.split("=", 1)[1]
        return req
    if op == "set":
        return {"op": "set", "sel": parse_sel(args[0]),
                "prop": args[1], "value": parse_value(args[2])}
    if op == "get":
        req = {"op": "get", "sel": parse_sel(args[0])}
        if len(args) > 1:
            req["prop"] = args[1]
        return req
    if op == "geom":
        return {"op": "geom", "sel": parse_sel(args[0])}
    if op == "type":
        req = {"op": "type", "sel": parse_sel(args[0]),
               "text": args[1]}
        if "--clear" in args[2:]:
            req["clear"] = True
        return req
    if op == "key":
        return {"op": "key", "key": args[0]}
    if op == "scroll":
        req = {"op": "scroll", "dy": int(args[1])}
        a0 = args[0]
        if a0.lstrip("-").replace(".", "", 1).isdigit():
            req["x"] = float(args[0])
            req["y"] = float(args[1])
            req["dy"] = int(args[2])
            if len(args) > 3:
                req["dx"] = int(args[3])
        else:
            req["sel"] = parse_sel(a0)
            if len(args) > 2:
                req["dx"] = int(args[2])
        return req
    if op == "shot":
        req = {"op": "shot"}
        if args:
            req["path"] = args[0]
        return req
    if op == "wait":
        req = {"op": "wait", "sel": parse_sel(args[0])}
        rest = list(args[1:])
        if "--timeout" in rest:
            i = rest.index("--timeout")
            req["timeout_ms"] = int(rest[i + 1])
            del rest[i:i + 2]
        if rest:
            req["prop"] = rest[0]
            req["value"] = parse_value(rest[1]) if len(rest) > 1 else True
        return req
    if op == "eval":
        return {"op": "eval", "code": " ".join(args)}
    if op == "raw":
        try:
            return json.loads(" ".join(args))
        except json.JSONDecodeError as e:
            die(f"bad raw JSON: {e}")
    die(f"unknown op '{op}' — see docstring for usage")


def main():
    args = list(sys.argv[1:])
    port = int(os.environ.get("OPENRGB2_QA_PORT", DEFAULT_PORT))
    while args and args[0].startswith("--port"):
        if args[0].startswith("--port="):
            port = int(args[0].split("=", 1)[1])
            args.pop(0)
        else:
            args.pop(0)
            port = int(args.pop(0))
    if not args:
        print(__doc__)
        sys.exit(2)
    req = build(args[0], args[1:])
    reply = send(req, port)
    # Windows consoles default to cp1252 — QML text carries
    # typographic chars, so never let encoding kill the reply.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print(json.dumps(reply, indent=2, ensure_ascii=False))
    sys.exit(0 if reply.get("ok") else 1)


if __name__ == "__main__":
    main()
