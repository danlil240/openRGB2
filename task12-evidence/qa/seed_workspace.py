#!/usr/bin/env python3
"""Seed the QA engine: upload desk-bundle.or2studio as 'bundle',
import_inspect + import_commit in ONE session (staged tokens are
session-bound), then reload to make it the active workspace."""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsctl import Client

HERE = os.path.dirname(os.path.abspath(__file__))

def main(bundle="desk-bundle.or2studio", mode=""):
    c = Client()
    path = os.path.join(HERE, bundle)
    data = open(path, "rb").read()
    tok = c.upload("bundle", data)
    print("staged:", tok[:24], "...")

    r = c.ws({"verb": "import_inspect", "staged": tok})
    assert r.get("accepted"), f"inspect enqueue failed: {r}"
    ins = c.wait_job(r["job_id"])
    print("inspect:", json.dumps(
        {k: ins.get(k) for k in ("state", "valid", "has_workspace",
                                 "errors")}))

    args = {"verb": "import_commit", "staged": tok,
            "id": "qa-desk"}
    if mode:
        args["mode"] = mode
    r = c.ws(args)
    assert r.get("accepted"), f"commit enqueue failed: {r}"
    ic = c.wait_job(r["job_id"])
    print("commit:", json.dumps({k: ic.get(k) for k in
        ("state", "committed", "applied", "workspace_id",
         "workspace_revision", "error")}))

    r = c.ws({"verb": "reload"})
    if r.get("accepted"):
        rl = c.wait_job(r["job_id"])
        print("reload:", rl.get("state"), rl.get("id"))
    else:
        print("reload:", str(r)[:200])

    g = c.ws({"verb": "get"})
    print("get:", json.dumps({k: g.get(k) for k in
        ("managed", "id", "workspace_revision",
         "resource_revision")}))

if __name__ == "__main__":
    main(*sys.argv[1:])
