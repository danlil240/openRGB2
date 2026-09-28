#!/usr/bin/env python3
"""Minimal OpenRGB SDK v4 client — enumerate controllers, resize zones.

Wire format mirrors src/backends/openrgb/SdkWire.h:
  header = 'ORGB' + u32 dev_id + u32 pkt_id + u32 body_size (little-endian)

Usage:
  sdk_probe.py list                          # controllers + zones
  sdk_probe.py resize <dev-name> <zone-name> <count>
"""
import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 6744
PKT_COUNT, PKT_DATA, PKT_PROTO, PKT_NAME = 0, 1, 40, 50
PKT_RESIZE = 1000
PROTO = 4


def send(sock, dev, pkt, body=b""):
    sock.sendall(b"ORGB" + struct.pack("<III", dev, pkt, len(body)) + body)


def recv_pkt(sock):
    hdr = b""
    while len(hdr) < 16:
        chunk = sock.recv(16 - len(hdr))
        if not chunk:
            raise IOError("closed")
        hdr += chunk
    magic, dev, pkt, size = struct.unpack("<4sIII", hdr)
    body = b""
    while len(body) < size:
        chunk = sock.recv(size - len(body))
        if not chunk:
            raise IOError("closed")
        body += chunk
    return dev, pkt, body


class Cur:
    def __init__(self, b):
        self.b, self.i = b, 0

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.i)[0]
        self.i += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.i)[0]
        self.i += 4
        return v

    def skip(self, n):
        self.i += n

    def s(self):
        n = self.u16()
        v = self.b[self.i:self.i + n - 1].decode("utf-8", "replace")
        self.i += n
        return v


def parse_controller(body):
    c = Cur(body)
    c.u32()  # data_size
    out = {"type": c.u32(), "name": c.s(), "vendor": c.s(),
           "desc": c.s(), "ver": c.s(), "serial": c.s(), "location": c.s()}
    nm = c.u16()
    out["active_mode"] = c.u32()
    for _ in range(nm):  # mode
        c.s()
        for _ in range(3):
            c.u32()
        for _ in range(2):
            c.u32()  # brightness_min/max (v>=3)
        for _ in range(4):
            c.u32()  # colors_min/max, speed
        c.u32()      # brightness (v>=3)
        for _ in range(2):
            c.u32()  # direction, color_mode
        nc = c.u16()
        c.skip(4 * nc)
    nz = c.u16()
    zones = []
    for _ in range(nz):
        z = {"name": c.s(), "type": c.u32(), "leds_min": c.u32(),
             "leds_max": c.u32(), "leds_count": c.u32()}
        mm = c.u16()
        c.skip(mm)
        ns = c.u16()  # v4 segments
        for _ in range(ns):
            c.s(); c.u32(); c.u32(); c.u32()
        zones.append(z)
    out["zones"] = zones
    out["num_leds"] = c.u16()
    return out


def connect():
    s = socket.create_connection((HOST, PORT), 5)
    send(s, 0, PKT_PROTO, struct.pack("<I", PROTO))
    recv_pkt(s)
    send(s, 0, PKT_NAME, b"qa-probe\0")
    return s


def main():
    s = connect()
    send(s, 0, PKT_COUNT)
    _, _, body = recv_pkt(s)
    n = struct.unpack_from("<I", body)[0]
    ctrls = []
    for i in range(n):
        send(s, i, PKT_DATA, struct.pack("<I", PROTO))
        _, _, body = recv_pkt(s)
        ctrls.append(parse_controller(body))

    if len(sys.argv) > 1 and sys.argv[1] == "resize":
        dev, zone, count = sys.argv[2], sys.argv[3], int(sys.argv[4])
        for di, c in enumerate(ctrls):
            if dev.lower() in c["name"].lower():
                for zi, z in enumerate(c["zones"]):
                    if z["name"] == zone:
                        send(s, di, PKT_RESIZE,
                             struct.pack("<ii", zi, count))
                        print(f"resized {c['name']}/{zone} idx={zi} -> {count}")
                        return
                sys.exit(f"zone {zone} not found")
        sys.exit(f"device {dev} not found")

    for di, c in enumerate(ctrls):
        print(f"[{di}] {c['vendor']}:{c['name']} leds={c['num_leds']} "
              f"active_mode={c['active_mode']}")
        for zi, z in enumerate(c["zones"]):
            print(f"    zone[{zi}] {z['name']}: "
                  f"count={z['leds_count']} min={z['leds_min']} "
                  f"max={z['leds_max']}")


if __name__ == "__main__":
    main()
