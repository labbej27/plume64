#!/usr/bin/env python3
"""Pilote Plume sur un C64 Ultimate par son API REST.

c64plume.py shot fichier.png       ecran bitmap ($E000) en PNG
c64plume.py keys "texte"           touches PETSCII (\\xNN accepte) dans le tampon clavier
c64plume.py click X Y              clic simule via la boite aux lettres $02A7
c64plume.py dump                   texte du document, styles notes {G}{I}{S}
c64plume.py busy                   secondes avant que Plume reprenne la main
"""
import os, struct, sys, time, urllib.request, zlib

IP = os.environ.get("C64_IP", "192.168.1.128")


def api(path, method="GET"):
    req = urllib.request.Request(f"http://{IP}{path}", method=method)
    return urllib.request.urlopen(req, timeout=15).read()


def read(addr, n):
    return api(f"/v1/machine:readmem?address={addr:04X}&length={n}")


def write(addr, data):
    api(f"/v1/machine:writemem?address={addr:04X}&data={bytes(data).hex()}", "PUT")


def shot(path):
    write(0x02A7, [2])                 # Plume se fige, ROM masquee
    for _ in range(50):
        if read(0x02A7, 1)[0] == 3:
            break
        time.sleep(0.05)
    try:
        bitmap = read(0xE000, 8000)
    finally:
        write(0x02A7, [0])
    rows = bytearray()
    for y in range(200):
        rows.append(0)
        for col in range(40):
            rows.append(0xFF ^ bitmap[col * 8 + (y // 8) * 320 + y % 8])
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 200, 1, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))


def keys(text):
    raw = text.encode("latin-1").decode("unicode_escape").encode("latin-1")
    # ASCII -> codes du clavier C64 : minuscules $41-$5A, majuscules $C1-$DA
    data = bytes(c - 0x20 if 0x61 <= c <= 0x7A else c + 0x80 if 0x41 <= c <= 0x5A else c for c in raw)
    for i in range(0, len(data), 8):
        while read(0xC6, 1)[0]:
            time.sleep(0.05)
        part = data[i:i + 8]
        write(0x0277, part)
        write(0xC6, [len(part)])
        time.sleep(0.25)


def click(x, y):
    write(0x02A8, [x & 255, x >> 8, y])
    write(0x02A7, [1])
    time.sleep(0.3)


CODES = "àâäçéèêëîïôöùûüÀÂÇÉÈÊËÎÏÔÙÛÜœŒ«»°€æøåÆØÅáÁíÍóÓúÚőŐűŰÖ"


def dump():
    info = read(0x02AB, 4)
    text_addr, len_addr = info[0] | info[1] << 8, info[2] | info[3] << 8
    n = int.from_bytes(read(len_addr, 2), "little")
    data = read(text_addr, n) if n else b""
    out = []
    for c in data:
        if c & 0xF8 == 0x10:
            s = c & 7
            out.append("{" + ("G" if s & 1 else "") + ("I" if s & 2 else "") + ("S" if s & 4 else "") + "}" if s else "{}")
        elif c == 13:
            out.append("\\n")
        elif c >= 128:
            out.append(CODES[c - 128] if c - 128 < len(CODES) else "?")
        else:
            out.append(chr(c))
    print(f"{n} octets : " + "".join(out))


def busy():
    start = time.time()
    write(0x02A7, [2])
    while read(0x02A7, 1)[0] != 3:
        time.sleep(0.02)
    write(0x02A7, [0])
    print(f"{time.time() - start:.1f} s")


cmd = sys.argv[1]
if cmd == "dump":
    dump()
elif cmd == "busy":
    busy()
if cmd == "shot":
    shot(sys.argv[2])
elif cmd == "keys":
    keys(sys.argv[2])
elif cmd == "click":
    click(int(sys.argv[2]), int(sys.argv[3]))
