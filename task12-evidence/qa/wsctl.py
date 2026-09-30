#!/usr/bin/env python3
"""Minimal IPC client for the QA engine pipe — workspace verbs.
Keeps the pipe name inside this file so no shell quoting mangles it.
"""
import json, struct, sys, time

PIPE = r"\\.\pipe\openrgb2-qa-engine"
MAGIC = 0x5049324F
HELLO, WELCOME = 0x0001, 0x8001
OK, ERROR, EVENT = 0x8002, 0x8003, 0x8200
WORKSPACE_COMMAND = 0x010E

def _recv_exact(f, n):
    buf = b""
    while len(buf) < n:
        c = f.read(n - len(buf))
        if not c:
            raise RuntimeError("pipe closed")
        buf += c
    return buf

class Client:
    def __init__(self, pipe=PIPE):
        self.f = open(pipe, "r+b", buffering=0)
        self.seq = 0
        self._send(HELLO, 0, {"client_name": "qa-wsctl",
                              "min_version": 1, "max_version": 1})
        m = self._recv()
        assert m[0] == WELCOME, f"expected WELCOME got {m[0]:#x}: {m[2]}"
        self.welcome = m[2]

    def _send(self, typ, rid, body):
        b = json.dumps(body).encode("utf-8")
        hdr = struct.pack("<IHHI I", MAGIC, 1, typ, rid, len(b))
        self.f.write(hdr + b)

    def _recv(self):
        hdr = _recv_exact(self.f, 16)
        magic, ver, typ, rid, blen = struct.unpack("<IHHI I", hdr)
        assert magic == MAGIC, hex(magic)
        body = json.loads(_recv_exact(self.f, blen) or b"{}")
        return typ, rid, body

    def call(self, typ, body):
        self.seq += 1
        self._send(typ, self.seq, body)
        while True:
            t, rid, b = self._recv()
            if t == EVENT:
                continue
            if rid == self.seq:
                if t == ERROR:
                    b["_error"] = True
                return b

    def ws(self, body):
        return self.call(WORKSPACE_COMMAND, body)

    def wait_job(self, job_id, timeout=30):
        end = time.time() + timeout
        last = {}
        while time.time() < end:
            last = self.ws({"verb": "job_status", "job_id": job_id})
            if last.get("state") in ("succeeded", "failed", "cancelled"):
                return last
            time.sleep(0.05)
        return last

    def upload(self, kind, data: bytes):
        import hashlib, base64
        r = self.ws({"verb": "transfer_begin", "direction": "upload",
                     "kind": kind, "length": len(data),
                     "sha256": hashlib.sha256(data).hexdigest()})
        tok = r.get("token") or r.get("transfer_id")
        assert tok, f"transfer_begin failed: {r}"
        MAX = int(r.get("max_chunk_bytes", 196608))
        off = 0
        while off < len(data):
            chunk = data[off:off + MAX]
            r = self.ws({"verb": "transfer_chunk",
                         "transfer_id": tok, "offset": off,
                         "data": base64.b64encode(chunk).decode()})
            assert not r.get("_error"), f"chunk failed: {r}"
            off += len(chunk)
        r = self.ws({"verb": "transfer_finish", "transfer_id": tok})
        assert not r.get("_error"), f"finish failed: {r}"
        return tok

    def download(self, body):
        """export-style download: verb returns a token then chunks."""
        r = self.ws(body)
        return r

    def dl_read(self, token):
        import base64, hashlib
        out = b""
        off = 0
        while True:
            r = self.ws({"verb": "transfer_chunk",
                         "transfer_id": token,
                         "offset": off, "max_bytes": 196608})
            if r.get("_error"):
                raise RuntimeError(f"chunk read failed: {r}")
            chunk = base64.b64decode(r.get("data", ""))
            out += chunk
            off += len(chunk)
            if not r.get("more", False):
                break
        self.ws({"verb": "transfer_finish", "transfer_id": token})
        return out

if __name__ == "__main__":
    c = Client()
    verb = sys.argv[1] if len(sys.argv) > 1 else "get"
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    args["verb"] = verb
    r = c.ws(args)
    print(json.dumps(r, indent=2)[:8000])
