#!/usr/bin/env python3
"""Police proportionnelle de Plume a partir d'unscii-8 (domaine public, viznut.fi/unscii).

mkfont.py unscii-8.hex sortie.s [--preview]
Codes internes : ASCII 32-126, puis les lettres accentuees de CHARSET a partir de 128.
"""
import sys

ACCENTS = "àâäçéèêëîïôöùûüÀÂÇÉÈÊËÎÏÔÙÛÜœŒ«»°€æøåÆØÅáÁíÍóÓúÚőŐűŰÖ"
# 128-167 : francais et danois (æøå : 2.1) ; 168-179 : hongrois ; 180 : Ö.
SPACE_WIDTH = 4
glyphs = {}
for line in open(sys.argv[1]):
    code, data = line.strip().split(":")
    if len(data) == 16:
        glyphs[int(code, 16)] = bytes.fromhex(data)

# Accents redessines : unscii les trace a plat. Deux lignes au-dessus de la
# lettre de base (lignes 2 a 7 de la minuscule), trait de 2 pixels incline.
DRAWN = {
    "aigu": ["....##..", "...##..."],
    "grave": ["..##....", "...##..."],
    "circ": ["...##...", "..#..#.."],
    "umlaut": [".##.##..", "........"],     # deux points, un pixel d'ecart
}
REDRAW = {"é": ("e", "aigu"), "è": ("e", "grave"), "ê": ("e", "circ"), "à": ("a", "grave"),
          "â": ("a", "circ"), "ù": ("u", "grave"), "û": ("u", "circ"), "ô": ("o", "circ"),
          "î": ("i", "circ"),
          "á": ("a", "aigu"), "í": ("i", "aigu"), "ó": ("o", "aigu"), "ú": ("u", "aigu"),
          # trema : unscii ecarte les points (colonnes 1-2 et 5-6), on les
          # rapproche d'une colonne au-dessus de la lettre.
          "ü": ("u", "umlaut"), "ö": ("o", "umlaut"),
          # majuscules : unscii les comprime deja sur les lignes 2 a 7 avec
          # l'accent en ligne 0, on repart donc du glyphe accentue lui-meme.
          "Á": ("Á", "aigu"), "É": ("É", "aigu"), "Í": ("Í", "aigu"),
          "Ó": ("Ó", "aigu"), "Ú": ("Ú", "aigu"),
          "Ü": ("Ü", "umlaut"), "Ö": ("Ö", "umlaut")}
for ch, (base, accent) in REDRAW.items():
    rows = bytearray(glyphs[ord(base)])
    if base == "i":
        rows[0] = 0                                      # pas de point sous l'accent
    for r, line in enumerate(DRAWN[accent]):
        rows[r] = int(line.replace("#", "1").replace(".", "0"), 2)
    glyphs[ord(ch)] = bytes(rows)

# Code 127 inutilise mais present : la largeur de tout code c est alors
# font_width[c - 32], sans calcul (accents a partir de 128).
chars = [chr(c) for c in range(32, 128)] + list(ACCENTS)
widths, columns = [], []
for ch in chars:
    rows = glyphs.get(ord(ch), bytes(8)) if ch != "\x7f" else bytes(8)
    used = [x for x in range(8) if any(r >> (7 - x) & 1 for r in rows)]
    if not used:
        left, width = 0, SPACE_WIDTH
    else:
        left, width = used[0], used[-1] - used[0] + 2      # + 1 colonne d'espace
    widths.append(width)
    columns.append(bytes(((r << left) & 0xFF) for r in rows))

with open(sys.argv[2], "w") as f:
    f.write("; Genere par tools/mkfont.py depuis unscii-8 (domaine public).\n")
    f.write("\t.export _font_width, _font_bits, FONT_FIRST, FONT_COUNT\n")
    f.write(f"FONT_FIRST = 32\nFONT_COUNT = {len(chars)}\n\t.rodata\n_font_width:\n")
    for i in range(0, len(widths), 16):
        f.write("\t.byte " + ",".join(str(w) for w in widths[i:i + 16]) + "\n")
    f.write("_font_bits:\t\t\t; 8 octets par caractere, aligne a gauche\n")
    for ch, col in zip(chars, columns):
        f.write("\t.byte " + ",".join(f"${b:02X}" for b in col) + f"\t; {ch if ch.isprintable() else ''}\n")
with open(sys.argv[2].replace(".s", "_map.txt"), "w", encoding="utf-8") as f:
    for i, ch in enumerate(chars):
        f.write(f"{32 + i} {ch if ch.isprintable() else ''}\n")
if "--preview" in sys.argv:
    sample = "été très forêt à où âme sûr côté île Noël ça"
    lines = ["" for _ in range(8)]
    for ch in sample:
        i = chars.index(ch)
        for r in range(8):
            lines[r] += "".join("#" if columns[i][r] >> (7 - x) & 1 else "." for x in range(widths[i]))
    print("\n".join(l[:200] for l in lines))
    print("largeur moyenne a-z :", sum(widths[chars.index(c)] for c in "abcdefghijklmnopqrstuvwxyz") / 26)
