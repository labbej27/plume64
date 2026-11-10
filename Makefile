# Plume : traitement de texte graphique pour Commodore 64.
# make -> build/plume-fr.crt, plume-en.crt et plume-dk.crt (et les .prg)
# dk : menus anglais, clavier danois (æ ø å sur : ; @)
LANGS := fr en dk
SRC   := src/main.c src/gfx.s src/font.s
PAGES  = $(shell python3 -I -c "import os,sys;print((os.path.getsize(sys.argv[1])-2+255)//256)" $<)

.PHONY: all clean
all: $(foreach l,$(LANGS),build/plume-$(l).crt)

src/font.s: fonts/unscii-8.hex tools/mkfont.py
	python3 -I tools/mkfont.py $< $@

build/plume-fr.prg: $(SRC) src/strings.h src/charmap.h
	@mkdir -p build/fr
	cl65 -t c64 -O -Or -Cl -o $@ -m build/plume-fr.map --obj-path build/fr -Wl -D,__HIMEM__=0xC000 -Wl -D,__STACKSIZE__=0x200 $(SRC)

build/plume-en.prg: $(SRC) src/strings.h src/charmap.h
	@mkdir -p build/en
	cl65 -t c64 -O -Or -Cl -DLANG_EN -o $@ -m build/plume-en.map --obj-path build/en -Wl -D,__HIMEM__=0xC000 -Wl -D,__STACKSIZE__=0x200 $(SRC)

build/plume-dk.prg: $(SRC) src/strings.h src/charmap.h
	@mkdir -p build/dk
	cl65 -t c64 -O -Or -Cl -DLANG_EN -DKBD_DK -o $@ -m build/plume-dk.map --obj-path build/dk -Wl -D,__HIMEM__=0xC000 -Wl -D,__STACKSIZE__=0x200 $(SRC)

build/plume-%.crt: build/plume-%.prg crt.s crt.cfg tools/mkcrt.py
	ca65 -D PRG_PAGES=$(PAGES) -o build/crt-$*.o crt.s
	ld65 -C crt.cfg -o build/crt-$*.bin build/crt-$*.o
	python3 -I tools/mkcrt.py build/crt-$*.bin $< $@ "PLUME $*"

clean:
	rm -rf build
