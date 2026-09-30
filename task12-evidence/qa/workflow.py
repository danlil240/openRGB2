#!/usr/bin/env python3
"""workflow.py — Task-12 QA workflow driver for openrgb2-ui.

Drives the real, windowed UI through the QA socket (port 5584)
against the isolated fake-backend engine (custom pipe). Each step
is recorded into transcript.json; screenshots land in shots/.

usage: python workflow.py [--phase N] [--port 5584]
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__),
                                "..", "..", "tools", "qa"))
import qa as q  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(HERE, "shots")
PORT = 5584
LOG = []

os.makedirs(SHOTS, exist_ok=True)


def req(**kw):
    r = q.send(kw, PORT)
    LOG.append({"req": kw, "reply": r})
    if not r.get("ok"):
        raise RuntimeError(f"{kw} -> {r}")
    return r.get("result")


def soft(name, fn):
    """Run a step; record pass/fail without aborting."""
    try:
        out = fn()
        LOG.append({"step": name, "status": "pass",
                    "detail": str(out)[:400]})
        print(f"  PASS {name}: {str(out)[:120]}")
        return out
    except Exception as e:  # noqa: BLE001
        LOG.append({"step": name, "status": "fail",
                    "detail": str(e)[:400]})
        print(f"  FAIL {name}: {str(e)[:160]}")
        return None


def ev(code):
    return req(op="eval", code=code)


def evj(code):
    return json.loads(ev(f"JSON.stringify({code})"))


def shot(name):
    p = os.path.join(SHOTS, name)
    req(op="shot", path=p)
    return name


def click(sel, **kw):
    return req(op="click", sel=sel, **kw)


def wait_expr(code, timeout_s=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if ev(code):
            return True
        time.sleep(0.4)
    return False


OBJIDS = (
    "(function(){var m=objectCtl.objectModel,ids=[];"
    "for(var i=0;i<256;i++){var r=m.rowAt(i);"
    "if(!r||r.id===undefined)break;ids.push(r.id);}"
    "return ids})()"
)

FIND = (
    "(function(){var w=this.contentItem?this:null;"
    "function walk(it,name){if(!it||!it.childItems)return null;"
    "var cs=it.childItems();for(var i=0;i<cs.length;i++){"
    "if(cs[i].objectName===name)return cs[i];"
    "var r=walk(cs[i],name);if(r)return r;}return null;}"
    "return walk(w,%s)})()"
)


def find(name):
    """Return truthy when an item with objectName exists."""
    return ev(FIND % json.dumps(name) + "!==null") is not False and \
        evj(FIND % json.dumps(name) + "?{x:1}:null") is not None


def scroll_to(name, scroller, band=(60, 780)):
    """Drive a page Flickable until `name` sits inside the
       window band; returns the item's scene rect or None."""
    for _ in range(24):
        g = req(op="geom", sel=name)
        r = g.get("rect")
        if r and band[0] < r[1] < band[1]:
            return r
        g2 = req(op="geom", sel=scroller)
        top = (g2.get("rect") or [0, 0, 0, 0])[1]
        cy = req(op="get", sel=scroller,
                 prop="contentY").get("value") or 0
        if r:
            req(op="set", sel=scroller, prop="contentY",
                value=cy + (r[1] - top - 300))
        else:
            req(op="set", sel=scroller, prop="contentY",
                value=cy + 700)
        time.sleep(0.35)
    return None


def geom_of(name):
    return evj(FIND % json.dumps(name) +
               ".mapToGlobal(Qt.point(0,0))") or {}


def main():
    phase = None
    if "--phase" in sys.argv:
        phase = int(sys.argv[sys.argv.index("--phase") + 1])
    if "--port" in sys.argv:
        global PORT
        PORT = int(sys.argv[sys.argv.index("--port") + 1])

    print(f"== workflow driver, qa port {PORT} ==")
    req(op="ping")

    # ------------------------------------------------ phase 1
    # Layout view + live 3D draft preview with real objects.
    if phase in (None, 1):
        print("-- phase 1: layout + live 3D preview --")
        click("navLayout")
        time.sleep(0.6)
        # The Quick3D loader is user-preference gated — the QA
        # run asks for it explicitly (quick3dUp stays the hard
        # gate; a build without Quick3D honestly stays 2D).
        ev("if(typeof uiPrefs!=='undefined')"
           "uiPrefs.layoutShow3d=true")
        time.sleep(1.0)
        soft("viewport-active",
             lambda: wait_expr(
                 "viewport.previewActive===true", 10))
        st = soft("preview-has-objects",
                  lambda: evj(
                      "{objs:(viewport.objects||[]).length,"
                      "paused:viewport.paused,"
                      "reason:viewport.pausedReason,"
                      "label:viewport.label}"))
        soft("shot-layout-3d", lambda: shot("01-layout-3d.png"))
        if st and st.get("objs"):
            # Real 3D pick: probe a coarse grid of window points
            # over the viewport rect — Quick3D picking selects the
            # object under the pixel. (objNodes test seams are not
            # populated until delegates report, so blind probing.)
            g = req(op="geom", sel="studioViewport")
            r = g.get("rect") or [0, 0, 0, 0]
            x0, y0, w, h = r
            picked = None
            probes = []
            for fy in (0.5, 0.35, 0.65, 0.25, 0.75):
                for fx in (0.5, 0.35, 0.65, 0.2, 0.8):
                    probes.append((x0 + w * fx, y0 + h * fy))
            for px, py in probes:
                q.send({"op": "click", "x": px, "y": py}, PORT)
                time.sleep(0.35)
                sel = evj("{sel:objectCtl.selectedId}") or {}
                if sel.get("sel"):
                    picked = (px, py, sel["sel"])
                    break
            LOG.append({"step": "3d-pick",
                        "status": "pass" if picked else "fail",
                        "detail": str(picked)})
            print(f"  3d-pick -> {picked}")
            soft("shot-3d-picked",
                 lambda: shot("02-3d-picked.png"))

    # ------------------------------------------------ phase 2
    # 2D canvas + selection + transform edit.
    if phase in (None, 2):
        print("-- phase 2: 2D canvas + edits --")
        soft("view-2d", lambda: click("view2dButton"))
        time.sleep(0.5)
        soft("shot-2d", lambda: shot("03-2d-canvas.png"))
        objs = evj(OBJIDS) or []
        if objs:
            sel = "canvasObj_" + objs[0]
            soft(f"2d-select-{sel}", lambda: click(sel))
            time.sleep(0.4)
            soft("2d-selected",
                 lambda: evj("{sel:objectCtl.selectedId}"))
            soft("shot-2d-selected",
                 lambda: shot("04-2d-selected.png"))
        # device library: real '+' click adds an instance.
        # The library lives below the fold — scroll the page
        # Flickable until a row's '+' is inside the window.
        soft("scroll-to-lib",
             lambda: scroll_to(
                 {"text": "+", "type": "IconBtn", "index": 0},
                 "layoutScroll") is not None)
        before = len(evj(OBJIDS) or [])
        soft("lib-add-click",
             lambda: click({"text": "+", "type": "IconBtn",
                            "index": 0}))
        time.sleep(0.8)
        after = len(evj(OBJIDS) or [])
        LOG.append({"step": "lib-add-count",
                    "status": "pass" if after > before else "fail",
                    "detail": f"{before} -> {after}"})
        print(f"  objects {before} -> {after}")
        soft("shot-lib-added", lambda: shot("05-lib-added.png"))

    # ------------------------------------------------ phase 3
    # Layer + palette edits (Lighting page hosts the stack? —
    # layout inspector carries palette; check both).
    if phase in (None, 3):
        print("-- phase 3: layers + palette --")
        soft("lighting-nav", lambda: click("navLighting"))
        time.sleep(0.8)
        soft("shot-lighting", lambda: shot("06-lighting.png"))
        soft("layers-present",
             lambda: evj("{layers:(typeof layerStack!=="
                         "'undefined'),ls:(typeof paletteEditor"
                         "!=='undefined')}"))

        soft("scroll-to-layers",
             lambda: scroll_to("addLayer", "lightingScroll")
                     is not None)
        soft("add-layer", lambda: click("addLayer"))
        time.sleep(0.6)
        soft("shot-layer-added",
             lambda: shot("07-layer-added.png"))
        # The palette editor lives in the layer inspector — it
        # only instantiates once a layer row is selected.
        soft("select-layer-0", lambda: click("layerName_0"))
        time.sleep(0.5)
        soft("scroll-to-palette",
             lambda: scroll_to("stopAdd", "lightingScroll")
                     is not None)
        soft("palette-add-stop", lambda: click("stopAdd"))
        time.sleep(0.4)
        soft("shot-palette", lambda: shot("08-palette.png"))
        soft("back-to-layout", lambda: click("navLayout"))
        time.sleep(0.6)

    # ------------------------------------------------ phase 4
    # Save via keyboard focus path + Ctrl+S. The Ctrl+S
    # Shortcut is enabled on the Lighting view only
    # (currentView === 1) — navigate there first.
    if phase in (None, 4):
        print("-- phase 4: save --")
        click("navLighting")
        time.sleep(0.8)
        soft("dirty-before-save",
             lambda: evj("{dirty:workspace.dirty}"))
        soft("ctrl-s", lambda: req(op="key", key="Ctrl+S"))
        soft("save-settled",
             lambda: wait_expr("workspace.saving===false", 10))
        st = soft("after-save",
                  lambda: evj("{dirty:workspace.dirty,"
                              "rev:workspace.workspaceRevision}"))
        soft("shot-saved", lambda: shot("09-saved.png"))

    # ------------------------------------------------ phase 5
    # Workspace tree/list + actions dialog.
    if phase in (None, 5):
        print("-- phase 5: workspace list + actions --")
        click("navLayout")
        time.sleep(0.8)
        req(op="set", sel="layoutScroll", prop="contentY",
            value=0)
        time.sleep(0.4)
        soft("scroll-to-wsbtn",
             lambda: scroll_to("wsActionsButton",
                               "layoutScroll") is not None)
        soft("open-ws-actions", lambda: click("wsActionsButton"))
        time.sleep(1.0)
        soft("ws-list",
             lambda: evj("{list:workspace.workspaceList,"
                         "id:workspace.workspaceId}"))
        soft("shot-ws-actions",
             lambda: shot("10-ws-actions.png"))
        soft("close-dialog",
             lambda: req(op="key", key="Escape"))
        time.sleep(0.5)

    # ------------------------------------------------ phase 6
    # External change -> honest notice -> reload adopts the
    # recovered generation -> the draft stays writable (the
    # recovered-revision wedge fix, end-to-end through the UI).
    if phase in (None, 6):
        print("-- phase 6: external change + recovery --")
        sys.path.insert(0, HERE)
        from wsctl import Client  # noqa: E402
        c = Client()
        g = c.ws({"verb": "get"})
        gen = g.get("generation", 0)
        wsdir = g.get("workspace_dir", "")
        doc = os.path.join(wsdir, "generations",
                           f"gen-{gen:06d}", "document.json")
        LOG.append({"step": "tamper-target",
                    "detail": doc})
        with open(doc, "w", encoding="utf-8") as f:
            f.write("{ tampered }")
        # The engine's external-scan reports it on the next
        # snapshot — the UI shows the reload affordance.
        soft("ext-change-surfaced",
             lambda: wait_expr(
                 "workspace.externalChange==='modified'"
                 "||workspace.externalChange==='deleted'", 15))
        soft("shot-ext-change",
             lambda: shot("11-external-change.png"))
        soft("ui-reload-disk",
             lambda: click("wsReloadDisk"))
        soft("reload-settled",
             lambda: wait_expr(
                 "workspace.externalChange===''"
                 "||workspace.externalChange===undefined", 15))
        st2 = soft("after-recovery",
                   lambda: evj("{rev:workspace.workspaceRevision,"
                               "dirty:workspace.dirty,"
                               "err:workspace.error}"))
        soft("shot-recovered",
             lambda: shot("12-recovered.png"))
        # THE WEDGE CHECK: a draft edit + save must land on the
        # healed marker — before the fix this was an endless
        # revision_conflict.
        ev("effectEdit.addLayer('static')")
        time.sleep(0.5)
        ev("workspace.save()")
        soft("save-after-recovery",
             lambda: wait_expr("workspace.dirty===false", 15))
        soft("post-recovery-state",
             lambda: evj("{rev:workspace.workspaceRevision,"
                         "err:workspace.error,"
                         "dirty:workspace.dirty}"))
        soft("shot-healed", lambda: shot("13-healed.png"))

    # ------------------------------------------------ phase 7
    # Calibration surface — honest state with no devices, and
    # the IPC verbs refuse cleanly rather than fabricating.
    if phase in (None, 7):
        print("-- phase 7: calibration + devices --")
        sys.path.insert(0, HERE)
        from wsctl import Client  # noqa: E402
        c = Client()
        st = c.ws({"verb": "calibration_status"})
        LOG.append({"step": "calibration-status",
                    "status": "pass" if st.get("active") is False
                    else "fail",
                    "detail": json.dumps(st)[:300]})
        print(f"  calibration_status -> {json.dumps(st)[:120]}")
        b = c.ws({"verb": "calibration_begin",
                  "controller": {"device": "none", "serial": "",
                                 "location": "", "zone": ""},
                  "zone": "z"})
        LOG.append({"step": "calibration-begin-refused",
                    "status": "pass" if b.get("_error") else "fail",
                    "detail": json.dumps(b)[:300]})
        print(f"  calibration_begin -> {json.dumps(b)[:160]}")
        soft("devices-nav", lambda: click("navDevices"))
        time.sleep(1.0)
        soft("shot-devices",
             lambda: shot("14-devices-empty.png"))

    tp = os.path.join(HERE, "transcript.json")
    prior = []
    if os.path.exists(tp):
        try:
            prior = json.load(open(tp, encoding="utf-8"))
        except Exception:  # noqa: BLE001
            prior = []
    with open(tp, "w", encoding="utf-8") as f:
        json.dump(prior + LOG, f, indent=2, ensure_ascii=False)
    print(f"transcript: {len(prior) + len(LOG)} entries"
          f" -> transcript.json")


if __name__ == "__main__":
    main()
