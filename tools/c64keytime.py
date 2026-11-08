#!/usr/bin/env python3
"""Mesure le temps de traitement de touches par Plume : c64keytime.py TOUCHES (\\xNN accepte)"""
import sys, time, urllib.request
IP = "192.168.1.128"
def rd(a, n): return urllib.request.urlopen(f"http://{IP}/v1/machine:readmem?address={a:04X}&length={n}", timeout=10).read()
def wr(a, d): urllib.request.urlopen(urllib.request.Request(f"http://{IP}/v1/machine:writemem?address={a:04X}&data={bytes(d).hex()}", method="PUT"), timeout=10).read()
def idle():
    while rd(0xC6, 1)[0]: time.sleep(0.01)
    wr(0x02A7, [2])
    while rd(0x02A7, 1)[0] != 3: time.sleep(0.01)
    wr(0x02A7, [0])
keys = sys.argv[1].encode("latin-1").decode("unicode_escape").encode("latin-1")
idle()
t = time.time()
for i in range(0, len(keys), 10):
    part = keys[i:i + 10]
    while rd(0xC6, 1)[0]: time.sleep(0.01)
    wr(0x0277, part); wr(0xC6, [len(part)])
idle()
print(f"{len(keys)} touches : {time.time() - t:.2f} s au total, {(time.time() - t) / len(keys):.3f} s par touche")
