#!/usr/bin/env python3
"""workflow_extra.py — Task-12 Fix-Round-2 QA additions on the
PACKAGED stack (port 5587, pipe \\.\pipe\openrgb2-pkg-engine):

  A. focused-text save — real `type` op into wsCopyName, then
     wsSaveCopy commits a NEW workspace (save_copy base_rev=0).
  B. conflict-bundle inspect -> conflict choice -> commit through
     the WorkspaceImportDialog (path typed, no native dialog).
  C. external-change notice capture -> reload -> healed.

Transcript: transcript-extra.json. Shots land in shots/."""

import json, os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__),
                                "..", "..", "tools", "qa"))
import qa as q  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(HERE, "shots")
PORT = 5587
LOG = []

sys.path.insert(0, HERE)
from wsctl import Client  # noqa: E402

BSL = chr(92)
PIPE = BSL + BSL + "." + BSL + "pipe" + BSL + "openrgb2-pkg-engine"


def req(**kw):
    r = q.send(kw, PORT)
    LOG.append({"req": kw, "reply": r})
    if not r.get("ok"):
        raise RuntimeError(f"{kw} -> {r}")
    return r.get("result")


def ev(code):
    return req(op="eval", code=code)


def evj(code):
    return json.loads(ev("JSON.stringify(%s)" % code))


def shot(name):
    return req(op="shot", path=os.path.join(SHOTS, name))


def soft(name, fn):
    try:
        out = fn()
        LOG.append({"step": name, "status": "pass",
                    "detail": str(out)[:400]})
        print(f"  PASS {name}: {str(out)[:110]}")
        return out
    except Exception as e:  # noqa: BLE001
        LOG.append({"step": name, "status": "fail",
                    "detail": str(e)[:400]})
        print(f"  FAIL {name}: {str(e)[:150]}")
        return None


def wait_expr(code, timeout_s=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if ev(code):
            return True
        time.sleep(0.4)
    return False


def wait_get(sel, prop, timeout_s=15.0):
    """Poll a property through the resolver (eval's `this` window
    cannot reach Overlay-parented popups via `children`)."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            r = q.send({"op": "get", "sel": sel, "prop": prop},
                       PORT)
            LOG.append({"req": {"op": "get", "sel": sel,
                                "prop": prop}, "reply": r})
            if r.get("ok") and r.get("result", {}).get("value"):
                return r["result"]["value"]
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.4)
    return None


def engine_ws(body):
    return Client(PIPE).ws(body)


def scroll_to(name, scroller, band=(60, 780)):
    """Drive a page Flickable until `name` is inside the band."""
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


def main():
    req(op="ping")

    # The packaged engine only reopens the last workspace if its
    # root still exists — make qa-desk the managed id first.
    cur = soft("current-ws", lambda: evj(
        "{id:workspace.workspaceId,ready:workspace.ready}"))
    if not cur or cur.get("id") != "qa-desk":
        soft("open-qa-desk",
             lambda: ev("workspace.openWorkspace('qa-desk');1"))
        soft("qa-desk-ready-0",
             lambda: wait_expr(
                 "workspace.workspaceId==='qa-desk'"
                 "&&workspace.ready", 15))

    # The workspace toolbar lives on the Layout page, inside
    # its Flickable — navigate + scroll the buttons into view.
    soft("nav-layout", lambda: req(op="click", sel="navLayout"))
    time.sleep(0.8)
    soft("scroll-to-actions",
         lambda: scroll_to("wsActionsButton", "layoutScroll"))

    # ---- A. focused-text save (save-a-copy) ----------------------
    print("-- focused-text save --")
    soft("open-actions",
         lambda: req(op="click", sel="wsActionsButton"))
    time.sleep(1.0)
    soft("copyname-focus",
         lambda: req(op="click", sel="wsCopyName"))
    time.sleep(0.3)
    soft("copyname-type",
         lambda: req(op="type", sel="wsCopyName",
                     text="qa-copy"))
    time.sleep(0.3)
    soft("shot-savecopy-typed",
         lambda: shot("30-savecopy-typed.png"))
    soft("savecopy-click",
         lambda: req(op="click", sel="wsSaveCopy"))
    soft("savecopy-settled",
         lambda: wait_expr("workspace.saving===false", 15))
    soft("savecopy-list", lambda: engine_ws({"verb": "list"})
         ["workspaces"])
    soft("copy-present",
         lambda: "qa-copy" in
                 (engine_ws({"verb": "list"})["workspaces"]) or
                 (_ for _ in ()).throw(
                     AssertionError("qa-copy not committed")))
    soft("shot-savecopy-done",
         lambda: shot("31-savecopy-done.png"))
    req(op="key", key="Escape")
    time.sleep(0.5)

    # ---- B. conflict-bundle inspect -> choice -> commit ---------
    print("-- conflict-bundle import --")
    soft("scroll-to-bundle",
         lambda: scroll_to("wsBundleButton", "layoutScroll"))
    bundle = os.path.join(HERE, "conflict-bundle.or2studio")
    soft("open-bundle-dialog",
         lambda: req(op="click", sel="wsBundleButton"))
    time.sleep(0.8)
    soft("importpath-type",
         lambda: req(op="type", sel="importPathField",
                     text=bundle))
    time.sleep(0.3)
    soft("inspect-click",
         lambda: req(op="click", sel="inspectBtn"))
    soft("report-ready",
         lambda: wait_get("importSummary", "text", 15) is
                 not None)
    time.sleep(0.5)
    # Conflict rows: the engine names each colliding bundled id;
    # the dialog renders rename_<id> fields + the summary text.
    soft("summary-text",
         lambda: req(op="get", sel="importSummary",
                     prop="text").get("value"))
    rep = soft("conflict-row",
               lambda: req(op="get", sel="rename_qa-strip",
                           prop="text").get("value"))
    soft("shot-conflict-report",
         lambda: shot("32-conflict-report.png"))
    # Resolve: the bundled qa-strip collides with the installed
    # library type — type an explicit rename into its field.
    soft("rename-type",
         lambda: req(op="type", sel="rename_qa-strip",
                     text="qa-strip-imported-2", clear=True))
    soft("confirm-import",
         lambda: req(op="click", sel="confirmImportBtn"))
    soft("import-finished",
         lambda: wait_get("importStatus", "text", 20))
    time.sleep(1.0)
    soft("shot-import-done",
         lambda: shot("33-conflict-committed.png"))
    soft("imported-ws-list",
         lambda: engine_ws({"verb": "list"})["workspaces"])
    req(op="key", key="Escape")
    time.sleep(0.5)

    # A successful bundle import adopts the NEW workspace — the
    # ext-change phase below needs qa-desk (two generations ->
    # a real LKG recovery) as the managed id again.
    soft("reopen-qa-desk",
         lambda: ev("workspace.openWorkspace('qa-desk');1"))
    soft("qa-desk-ready",
         lambda: wait_expr(
             "workspace.workspaceId==='qa-desk'&&workspace.ready", 15))

    # ---- C. external-change notice + reload ----------------------
    print("-- external change capture --")
    # The open job is async past workspace.ready — tampering while
    # the engine is still reading the active generation makes the
    # OPEN itself recover (external_sig re-baselines onto the
    # tampered bytes and the 'modified' window never reaches the
    # UI). Wait for the engine-side settle first.
    def engine_settled():
        for _ in range(30):
            g0 = engine_ws({"verb": "get"})
            if(g0.get("external_change") == "none"
               and g0.get("id") == "qa-desk"
               and not g0.get("marker_error")
               and g0.get("error") in (None, "")):
                return g0
            time.sleep(0.5)
        return None
    g = soft("engine-settled", engine_settled)
    time.sleep(1.0)
    g = engine_ws({"verb": "get"})
    gen = g.get("generation", 0)
    doc = os.path.join(g["workspace_dir"], "generations",
                       "gen-%06d" % gen, "document.json")
    with open(doc, "w", encoding="utf-8") as f:
        f.write("{ tampered-extra }")
    LOG.append({"step": "tamper-target", "detail": doc})
    soft("ext-change-visible",
         lambda: wait_expr(
             "workspace.externalChange==='modified'"
             "||workspace.externalChange==='deleted'", 25))
    soft("shot-ext-change-2",
         lambda: shot("22-external-change.png"))
    soft("ui-reload-disk",
         lambda: req(op="click", sel="wsReloadDisk"))
    soft("reload-settled-2",
         lambda: wait_expr(
             "workspace.externalChange===''"
             "||workspace.externalChange===undefined", 25))
    soft("shot-recovered-2",
         lambda: shot("21-recovered.png"))

    tp = os.path.join(HERE, "transcript-extra.json")
    prior = []
    if os.path.exists(tp):
        try:
            prior = json.load(open(tp, encoding="utf-8"))
        except Exception:  # noqa: BLE001
            prior = []
    with open(tp, "w", encoding="utf-8") as f:
        json.dump(prior + LOG, f, indent=2, ensure_ascii=False)
    print(f"transcript-extra: {len(prior) + len(LOG)} entries")


if __name__ == "__main__":
    if "--port" in sys.argv:
        PORT = int(sys.argv[sys.argv.index("--port") + 1])
    main()
