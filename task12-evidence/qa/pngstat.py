#!/usr/bin/env python3
"""Minimal PNG decoder: luminance stats over a rect.
usage: pngstat.py file.png [x y w h]"""
import sys, zlib, struct

def decode(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h, bd, ct = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos+8])
        chunk = data[pos+8:pos+8+ln]
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    assert bd == 8
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        filt = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if filt == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i-ch]) & 255
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif filt == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif filt == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]
                c = prev[i-ch] if i >= ch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, ch, bytes(out)

def stats(path, x=0, y=0, w=None, h=None):
    W, H, ch, px = decode(path)
    w = w or W; h = h or H
    vals = []
    for yy in range(y, min(y+h, H)):
        row = yy * W * ch
        for xx in range(x, min(x+w, W)):
            o = row + xx * ch
            if ch >= 3:
                lum = 0.299*px[o] + 0.587*px[o+1] + 0.114*px[o+2]
            else:
                lum = px[o]
            vals.append(lum)
    n = len(vals)
    mean = sum(vals)/n
    var = sum((v-mean)**2 for v in vals)/n
    return {"n": n, "mean": round(mean, 2),
            "var": round(var, 2),
            "min": min(vals), "max": max(vals)}

if __name__ == "__main__":
    a = sys.argv[1:]
    print(stats(a[0], *[int(v) for v in a[1:]]))
