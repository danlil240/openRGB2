#!/usr/bin/env python3
"""qa.py — command-line driver for the openrgb2-ui QA socket.

openrgb2-ui must be started with --qa (or OPENRGB2_QA=1). The app
then listens on 127.0.0.1:5583 (override: --qa <port>,
OPENRGB2_QA_PORT, or the --port flag here).

Every request carries the app's per-launch token (the socket closes
on a missing/wrong one). The app prints it to stderr and writes it to
%LOCALAPPDATA%/OpenRGB2/qa/qa-<port>.token (~/.local/share/... on
Linux); this driver reads that file. Override with --token TOKEN or
OPENRGB2_QA_TOKEN.

`shot PATH` saves where asked only if PATH does not exist yet (the
app refuses to overwrite files outside its QA output dir,
$OPENRGB2_QA_OUTDIR or %TEMP%/openrgb2-qa); this driver deletes an
existing PATH first, so re-shooting the same file still works.

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
    qa.py reveal SEL              scroll until SEL's rect is inside the window
    qa.py pick X Y | pick SEL     hit-test: show the item chain under a point
    qa.py shot [PATH.png]
    qa.py wait SEL [PROP VALUE] [--timeout MS]
    qa.py eval "this.currentView = 2"
    qa.py raw '{"op":"tree","all":true}'

Extra flags: `click SEL --reveal` scrolls the target into view
first (flickables clip). `click SEL` verifies by default: an
off-window center auto-reveals, and if the resolved point is
covered by another item the click refuses instead of silently
landing on the wrong thing. Coverage is judged on input, not
painting: items stacked above the target that accept no mouse
input (plain Item/Rectangle, QQuickDropArea, disabled or
hover-only items) let the click through — only an interactive
cover refuses. --allow-covered forces the click anyway;
--no-verify skips verification (raw send). Pinned items
(footers) that can't be scrolled are detected by an unmoving
rect. `shot` creates the parent directory before asking the
app to write.

`pick` entries: {n: objectName, t: type, r: scene rect,
p: child-index path, m: accepts mouse buttons (own or an
enabled pointer handler's), h: hoverEnabled, en: enabled}.
The chain lists every item under the point, topmost first.
"""

import json
import os
import socket
import sys
import time

DEFAULT_PORT = 5583
HOST = "127.0.0.1"

# --token / OPENRGB2_QA_TOKEN override; else read per port from the
# app's token file (cached).
_TOKEN_OVERRIDE = os.environ.get("OPENRGB2_QA_TOKEN") or None
_TOKEN_CACHE = {}


def token_file(port):
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser(
            "~\\AppData\\Local")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser(
            "~/.local/share")
    return os.path.join(base, "OpenRGB2", "qa", f"qa-{port}.token")


def token_for(port):
    if _TOKEN_OVERRIDE:
        return _TOKEN_OVERRIDE
    if port not in _TOKEN_CACHE:
        path = token_file(port)
        try:
            with open(path, "r", encoding="ascii") as f:
                _TOKEN_CACHE[port] = f.read().strip()
        except OSError:
            die(f"no QA token: cannot read {path} — is openrgb2-ui "
                f"running with --qa on port {port}? (or pass --token)", 1)
    return _TOKEN_CACHE[port]


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
    req = dict(req)
    req.setdefault("token", token_for(port))
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


def geom_rect(sel, port):
    """Resolved scene rect [x,y,w,h] for SEL, or None."""
    g = send({"op": "geom", "sel": sel}, port)
    if not g.get("ok"):
        return None
    return g["result"].get("rect")


def window_size(port):
    st = send({"op": "state"}, port)
    if not st.get("ok"):
        return 0, 0
    return (st["result"].get("width", 0),
            st["result"].get("height", 0))


def reveal(sel, port, margin=40, max_steps=20):
    """Scroll the page until SEL's rect is inside the window.

    Flickables clip: an item can report visible:true while its
    scene rect is off-window (clicks then land nowhere). Pumps a
    few wheel steps toward the item, settling each time. Items
    outside the scroll area (pinned footers like saveBar) never
    move — an unchanged rect across a scroll ends the loop
    honestly instead of timing out.
    """
    still = 0
    for _ in range(max_steps):
        win_w, win_h = window_size(port)
        if not win_h:
            return {"ok": False, "error": "no_window",
                    "message": "could not read window size"}
        r = geom_rect(sel, port)
        if r is None:
            return {"ok": False, "error": "not_found",
                    "message": f"no rect for {sel}"}
        cy = r[1] + r[3] / 2
        if margin <= cy <= win_h - margin:
            # In-bounds isn't enough — a Flickable keeps moving
            # after the wheel input (momentum), and clicks resolve
            # coords once. Require two identical rects ~150ms apart
            # before calling it settled.
            time.sleep(0.15)
            r2 = geom_rect(sel, port)
            if r2 == r:
                return {"ok": True,
                        "result": {"rect": r, "height": win_h}}
            continue
        # dy<0 scrolls content up (reveals items below the fold)
        dy = -6 if cy > win_h - margin else 6
        send({"op": "scroll", "x": 960, "y": win_h / 2, "dy": dy},
             port)
        time.sleep(0.35)
        r2 = geom_rect(sel, port)
        if r2 == r:
            # One unmoved scroll is inconclusive — the page may
            # still be settling after a nav switch. Items outside
            # the scroll area (pinned footers like saveBar) never
            # move; require three consecutive stills to say so.
            still += 1
            if still < 3:
                continue
            if margin <= cy <= win_h - margin:
                return {"ok": True,
                        "result": {"rect": r, "height": win_h}}
            return {"ok": False, "error": "fixed_offscreen",
                    "message": f"{sel} does not move with scroll "
                               f"(rect {r}); it is outside the "
                               f"scrollable area"}
        still = 0
    return {"ok": False, "error": "reveal_timeout",
            "message": f"could not scroll {sel} into view"}


# Enumerate EVERY item under the point (not just the childAt
# descent), in stacking order topmost-first: children visit
# bottom->top, we iterate reversed and emit a node after its
# subtree (children render above their parent). Per item we
# report whether a click would be consumed at-or-by it: its own
# acceptedMouseButtons/acceptedButtons, or an enabled pointer
# handler (TapHandler/DragHandler...) sitting on it via `data`.
# Root choice: root.contentItem is the ApplicationWindow's
# content AREA — popups live under QQuickOverlay, a SIBLING of it
# beneath the window root item (QQuickRootItem). Walking from the
# content area alone makes every in-popup item unreachable: the
# pick used to descend into the page below and report the click
# "covered" by whatever painted there (observed: the layout
# canvas' QQuickDropArea over wsOpen_*). Start at the window root
# item (contentItem.parent) so the overlay branch is included —
# the overlay is its topmost child, matching z-order.
_PICK_JS = (
    "(function(root){var p=root.contentItem;"
    "try{if(p&&p.parent)p=p.parent}catch(e){}"
    "if(!p)p=root;var x=%r,y=%r,out=[];"
    "function macc(k){var mb=0;"
    "try{if(typeof k.acceptedMouseButtons==='function')"
    "mb=mb|k.acceptedMouseButtons()}catch(e){}"
    "try{if(k.acceptedButtons!==undefined&&k.acceptedButtons!==null)"
    "mb=mb|k.acceptedButtons}catch(e){}"
    "if(mb===0){try{var d=k.data;for(var j=0;j<d.length;j++){var q=d[j];"
    "if(q!==k&&q.acceptedButtons!==undefined&&q.acceptedButtons!==null"
    "&&q.enabled!==false)mb=mb|q.acceptedButtons}}catch(e){}}"
    "return mb!==0}"
    "function emit(k,path){var tl=k.mapToItem(null,0,0),"
    "br=k.mapToItem(null,k.width,k.height),hov=false,en=true;"
    "try{hov=!!k.hoverEnabled}catch(e){}"
    "try{en=k.enabled!==false}catch(e){}"
    "out.push({n:k.objectName||'',t:String(k).split('(')[0],"
    "p:path,r:[tl.x,tl.y,br.x-tl.x,br.y-tl.y],"
    "m:macc(k),h:hov,en:en})}"
    "function walk(item,path){var kids;try{kids=item.children}"
    "catch(e){}if(!kids)return;"
    "for(var i=kids.length-1;i>=0;i--){var k=kids[i];"
    "try{if(!k.visible||k.width<=0||k.height<=0)continue;"
    "var pt=k.mapFromItem(null,x,y);"
    "if(pt.x<0||pt.y<0||pt.x>k.width||pt.y>k.height)continue;"
    "var cp=path+'.'+i;walk(k,cp);"
    "if(out.length<800)emit(k,cp)}catch(e){}}}"
    "walk(p,'');return JSON.stringify(out)})(this)")


def pick_chain(port, x, y):
    """Items under a window point, topmost-first stacking order.

    Uses the eval escape hatch — no server change needed. Each
    entry: {n: objectName, t: type name, r: scene rect,
    p: child-index path from contentItem, m: accepts mouse
    buttons, h: hoverEnabled, en: enabled}.
    """
    r = send({"op": "eval", "code": _PICK_JS % (x, y)}, port)
    if not r.get("ok") or not r.get("result"):
        return []
    try:
        return json.loads(r["result"])
    except (json.JSONDecodeError, TypeError):
        return []


def _rect_close(a, b, eps=2.0):
    return a and b and len(a) == 4 and len(b) == 4 and \
        all(abs(x - y) <= eps for x, y in zip(a, b))


# Type-name hints for items that consume a click without an
# acceptedButtons query resolving to JS (C++ items with
# overridden mouse handlers). QQuickDropArea deliberately is
# NOT here — it handles drag/drop only and never takes a click.
_INTERACTIVE_HINTS = (
    "MouseArea", "TapHandler", "DragHandler", "PinchHandler",
    "Flickable", "ScrollBar", "Slider", "SpinBox", "Dial",
    "Button", "CheckBox", "RadioButton", "Switch", "ComboBox",
    "TextInput", "TextField", "TextArea", "RangeSlider",
    "SwipeDelegate", "ItemDelegate", "TabButton", "ToolButton",
)


def _accepts_mouse(el):
    """Would this item actually consume a mouse press?

    `m` (acceptedButtons on the item or an enabled pointer
    handler on it) is authoritative; the type-name hints cover
    input consumers whose flag query didn't resolve. Disabled
    items consume nothing.
    """
    if el.get("en") is False:
        return False
    if el.get("m"):
        return True
    t = el.get("t") or ""
    return any(hint in t for hint in _INTERACTIVE_HINTS)


def _blocks_target(cover, target):
    """True when `cover` can steal the click from `target`.

    Entries inside the target's own subtree never block (a click
    consumed by the target's inner content still lands on the
    target) and strict ancestors cannot occlude — both are told
    apart by the child-index path prefix.
    """
    tp, cp = target.get("p") or "", cover.get("p") or ""
    if not tp or not cp:
        return True
    return not (cp == tp or cp.startswith(tp + ".")
                or tp.startswith(cp + "."))


def verify_hit(port, rect, name):
    """Check that a click at rect's center lands inside the
    target's subtree. The stack lists every item containing the
    point; the target proves it is in that stack by matching
    objectName AND its own rect (rect alone when unnamed).
    Returns (target_entry, covers, chain): covers are the items
    stacked ABOVE the target — entries that paint over it but
    may or may not intercept the click (see _accepts_mouse)."""
    if not rect or rect[2] <= 0 or rect[3] <= 0:
        return None, [], []
    cx, cy = rect[0] + rect[2] / 2, rect[1] + rect[3] / 2
    chain = pick_chain(port, cx, cy)
    for i, el in enumerate(chain):
        if name:
            if el["n"] == name and _rect_close(el["r"], rect):
                return el, chain[:i], chain
        elif _rect_close(el["r"], rect):
            return el, chain[:i], chain
    return None, [], chain


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
            # The app resolves the path in ITS cwd — pin it to the
            # caller's intent with an absolute path, and pre-create
            # the directory (the app will not mkdir).
            p = os.path.abspath(args[0])
            parent = os.path.dirname(p)
            if parent:
                os.makedirs(parent, exist_ok=True)
            # The app never overwrites outside its QA output dir —
            # replacing the caller's own file is the caller's call.
            if os.path.isfile(p):
                os.remove(p)
            req["path"] = p
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
    global _TOKEN_OVERRIDE
    while args and (args[0].startswith("--port")
                    or args[0].startswith("--token")):
        flag = args.pop(0)
        if "=" in flag:
            flag, val = flag.split("=", 1)
        else:
            val = args.pop(0)
        if flag == "--port":
            port = int(val)
        elif flag == "--token":
            _TOKEN_OVERRIDE = val
        else:
            die(f"unknown flag {flag}")
    if not args:
        print(__doc__)
        sys.exit(2)
    if args[0] == "reveal":
        reply = reveal(parse_sel(args[1]), port)
    elif args[0] == "pick":
        # Hit-test: chain of items under a point (diagnostic for
        # covered/clipped clicks). `pick SEL` uses the item's
        # center; `pick X Y` takes window coords.
        if len(args) > 2 and args[1].lstrip("-").replace(
                ".", "", 1).isdigit():
            x, y = float(args[1]), float(args[2])
        else:
            r = geom_rect(parse_sel(args[1]), port)
            if r is None:
                reply = {"ok": False, "error": "not_found",
                         "message": f"no rect for {args[1]}"}
                print(json.dumps(reply, indent=2, ensure_ascii=False))
                sys.exit(1)
            x, y = r[0] + r[2] / 2, r[1] + r[3] / 2
        reply = {"ok": True,
                 "result": {"x": x, "y": y,
                            "chain": pick_chain(port, x, y)}}
    else:
        if args[0] == "click":
            no_verify = "--no-verify" in args
            if no_verify:
                args.remove("--no-verify")
            is_xy = args[1].lstrip("-").replace(".", "", 1).isdigit()
            if "--reveal" in args:
                args.remove("--reveal")
                if not is_xy:
                    r = reveal(parse_sel(args[1]), port)
                    if not r.get("ok"):
                        print(json.dumps(r, indent=2))
                        sys.exit(1)
                    time.sleep(0.15)
            elif not is_xy and not no_verify:
                # A click is only honest if it lands on the
                # target: off-window centers get a reveal; a
                # point covered by another item refuses loudly.
                # Covers that accept no mouse input (DropArea,
                # plain Item/Rectangle) don't intercept — the
                # click legitimately falls through to the target.
                # --allow-covered overrides the refusal entirely.
                allow_covered = "--allow-covered" in args
                if allow_covered:
                    args.remove("--allow-covered")
                sel = parse_sel(args[1])
                win_w, win_h = window_size(port)
                r = geom_rect(sel, port)
                if r is not None:
                    cy = r[1] + r[3] / 2
                    if not (0 <= cy <= win_h):
                        rr = reveal(sel, port)
                        if not rr.get("ok"):
                            print(json.dumps(rr, indent=2))
                            sys.exit(1)
                        r = geom_rect(sel, port)
                        time.sleep(0.15)
                if r is not None:
                    name = ""
                    if isinstance(sel, str):
                        name = sel if ":" not in sel else \
                            (sel.split(":", 1)[1]
                             if sel.startswith("name:") else "")
                    elif isinstance(sel, dict):
                        name = sel.get("name", "")
                    target, covers, chain = verify_hit(
                        port, r, name)
                    msg = None
                    leaf = chain[0] if chain else {}
                    if target is None:
                        msg = ("click point is covered by "
                               + (leaf.get("n") or leaf.get("t")
                                  or "nothing"))
                    else:
                        blockers = [c for c in covers
                                    if _blocks_target(c, target)
                                    and _accepts_mouse(c)]
                        if blockers:
                            leaf = blockers[0]
                            msg = ("click point is covered by "
                                   + (leaf.get("n")
                                      or leaf.get("t"))
                                   + " (interactive cover — "
                                     "--allow-covered to force)")
                    if msg is not None and not allow_covered:
                        reply = {"ok": False, "error": "covered",
                                 "message": msg,
                                 "rect": r, "leaf": leaf}
                        if covers:
                            reply["covers"] = covers
                        print(json.dumps(reply, indent=2,
                                         ensure_ascii=False))
                        sys.exit(1)
        req = build(args[0], args[1:])
        reply = send(req, port)
    # Windows consoles default to cp1252 — QML text carries
    # typographic chars, so never let encoding kill the reply.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print(json.dumps(reply, indent=2, ensure_ascii=False))
    sys.exit(0 if reply.get("ok") else 1)


if __name__ == "__main__":
    main()
