; Plume : routines graphiques (ecran bitmap et tampon d'impression).
;
; Ecran : banque VIC 3, bitmap en $E000 (sous la ROM KERNAL), couleurs en $CC00.
; Les routines qui lisent le bitmap coupent la ROM KERNAL le temps de l'acces.
        .setcpu "6502"
        .importzp ptr1, ptr2, tmp1, tmp2, tmp3, tmp4
        .import _font_width, _font_bits
        .export _gfx_init, _draw_glyph, _glyph_width
        .export _bm_fill, _bm_or
        .export _gx, _gch, _gstyle, _gbase

STYLE_BOLD      = $01
STYLE_ITALIC    = $02
STYLE_UNDERLINE = $04

        .bss
_gx:     .res 2          ; x en pixels depuis _gbase
_gch:    .res 1          ; code interne du caractere
_gstyle: .res 1
_gbase:  .res 2          ; adresse de la rangee de cartes (ecran : $E000 + ligne * 320)
shift:   .res 1
shvec:   .res 2
width:   .res 1
umask:   .res 1
row:     .res 1

        .code
; Bitmap, couleurs noir sur gris clair, banque VIC 3.
_gfx_init:
        lda $DD02
        ora #$03
        sta $DD02
        lda $DD00
        and #$FC                ; banque 3 : $C000-$FFFF
        sta $DD00
        lda #$38                ; ecran $CC00, bitmap $E000
        sta $D018
        lda #$3B                ; mode bitmap, 25 lignes
        sta $D011
        lda #$C8
        sta $D016
        lda #$0F
        sta $D020
        ldx #0
@c:     lda #$0F                ; noir (0) sur gris clair (15)
        sta $CC00,x
        sta $CD00,x
        sta $CE00,x
        sta $CF00,x
        inx
        bne @c
        lda #<$E000
        ldx #>$E000
        jsr setptr
        lda #<8000
        ldx #>8000
        ldy #0
        jmp fill

; Index de police dans A -> ptr2 = glyphe, renvoie la largeur dans A.
glyph_index:
        sec
        sbc #32                 ; table continue : code 32 = index 0
        tax
        lda #0
        sta ptr2+1
        txa
        asl
        rol ptr2+1
        asl
        rol ptr2+1
        asl
        rol ptr2+1
        clc
        adc #<_font_bits
        sta ptr2
        lda ptr2+1
        adc #>_font_bits
        sta ptr2+1
        lda _font_width,x
        rts

; unsigned char glyph_width(unsigned char code) : largeur avec le style courant.
_glyph_width:
        jsr glyph_index
        ldx _gstyle
        cpx #0
        beq @done
        sta tmp1
        txa
        and #STYLE_BOLD
        beq @nb
        inc tmp1
@nb:    lda tmp1
@done:  ldx #0
        rts

; Dessine _gch en (_gx, _gbase) avec _gstyle, en OU sur le contenu.
; Texte sans style : chemin direct. Decalage : saut dans une suite deroulee
; de "lsr a / ror tmp3". Le second octet n'est ecrit que s'il est non nul.
_draw_glyph:
        lda _gch
        jsr glyph_index
        sta width
        lda _gstyle
        beq dg_nostyle
        and #STYLE_BOLD
        beq dg_w
        inc width
dg_w:     lda #0                  ; masque du soulignement : 'width' bits a gauche
        ldx width
dg_m:     sec
        ror
        dex
        bne dg_m
        sta umask
dg_nostyle:
        lda _gx
        and #7
        sta shift
        eor #7                  ; entree = shtab + (7 - decalage) * 3
        sta tmp1
        asl
        adc tmp1
        adc #<shtab
        sta shvec
        lda #>shtab
        adc #0
        sta shvec+1
        lda _gx                 ; ptr1 = _gbase + (_gx / 8) * 8
        and #$F8
        clc
        adc _gbase
        sta ptr1
        lda _gx+1
        adc _gbase+1
        sta ptr1+1
        php
        sei
        lda $01
        pha
        lda #$35
        sta $01
        ldy #0
dg_row:   lda (ptr2),y
        ldx _gstyle
        beq dg_styled
        tax                     ; styles : italique, gras, souligne
        lda _gstyle
        and #STYLE_ITALIC
        beq dg_ni
        cpy #4
        bcs dg_ni
        txa
        lsr
        tax
dg_ni:    lda _gstyle
        and #STYLE_BOLD
        beq dg_nb
        stx tmp1
        txa
        lsr
        ora tmp1
        tax
dg_nb:    lda _gstyle
        and #STYLE_UNDERLINE
        beq dg_nu
        cpy #7
        bne dg_nu
        txa
        ora umask
        tax
dg_nu:    txa
dg_styled:
        ldx shift
        beq dg_one
        ldx #0
        stx tmp3
        jmp (shvec)
shdone: ora (ptr1),y
        sta (ptr1),y
        lda tmp3
        beq dg_next
        sty tmp4
        tya
        ora #8
        tay
        lda tmp3
        ora (ptr1),y
        sta (ptr1),y
        ldy tmp4
dg_next:  iny
        cpy #8
        bne dg_row
        pla
        sta $01
        plp
        rts
dg_one:   ora (ptr1),y
        sta (ptr1),y
        jmp dg_next

; 7 decalages d'un bit vers la droite ; le debordement passe dans tmp3.
shtab:  lsr a
        ror tmp3
        lsr a
        ror tmp3
        lsr a
        ror tmp3
        lsr a
        ror tmp3
        lsr a
        ror tmp3
        lsr a
        ror tmp3
        lsr a
        ror tmp3
        jmp shdone

; Parametres communs : adresse (A/X), longueur sur la pile C via les appels ci-dessous.
setptr: sta ptr1
        stx ptr1+1
        rts

; Remplit ptr1 sur (A/X) octets avec Y : pages entieres, puis le reste.
fill:   sta tmp1
        stx tmp2
        tya
        php
        sei
        ldx $01
        stx tmp4
        ldx #$35
        stx $01
        ldx tmp2
        ldy #0
        cpx #0
        beq @rest
@page:  sta (ptr1),y
        iny
        bne @page
        inc ptr1+1
        dex
        bne @page
@rest:  ldy tmp1
        beq @done
@r:     dey
        sta (ptr1),y
        cpy #0
        bne @r
@done:  ldx tmp4
        stx $01
        plp
        rts

        .import popax, popa
; void bm_fill(unsigned addr, unsigned len, unsigned char value)
_bm_fill:
        pha                     ; valeur
        jsr popax               ; longueur
        sta tmp4
        stx ptr2
        jsr popax               ; adresse
        jsr setptr
        pla
        tay
        lda tmp4
        ldx ptr2
        jmp fill

; void bm_xormask(unsigned addr, unsigned char mask) : inverse les points de
; 'mask' sur les 8 octets d'une carte (selection au pixel pres).
        .export _bm_xormask
_bm_xormask:
        sta tmp1
        jsr popax
        jsr setptr
        php
        sei
        lda $01
        pha
        lda #$35
        sta $01
        ldy #7
@x:     lda (ptr1),y
        eor tmp1
        sta (ptr1),y
        dey
        bpl @x
        pla
        sta $01
        plp
        rts

; void bm_or(unsigned addr, unsigned char value) : ajoute des points a un octet.
_bm_or:
        sta tmp1
        jsr popax
        jsr setptr
        php
        sei
        lda $01
        pha
        lda #$35
        sta $01
        ldy #0
        lda (ptr1),y
        ora tmp1
        sta (ptr1),y
        pla
        sta $01
        plp
        rts

; ------------------------------------------------------------------
; unsigned next_line_asm(void) : debut de la ligne suivante.
; Entree : _nl_pos (position), _nl_style (style). Utilise _text, _doclen,
; _wrap_width et la table _font_width. Sortie : position dans A/X,
; style en vigueur a cette position dans _nl_style.
        .import _text, _doclen, _wrap_width
        .importzp ptr3, ptr4, sreg
        .export _next_line_asm, _nl_pos, _nl_style

        .bss
_nl_pos:   .res 2
_nl_style: .res 1
nl_end:    .res 2           ; fin du texte
nl_space:  .res 2           ; dernier espace (adresse), $FFFF = aucun
nl_spst:   .res 1           ; style au dernier espace
nl_start:  .res 2

        .code
_next_line_asm:
        clc                     ; ptr1 = text + pos
        lda #<_text
        adc _nl_pos
        sta ptr1
        sta nl_start
        lda #>_text
        adc _nl_pos+1
        sta ptr1+1
        sta nl_start+1
        clc                     ; nl_end = text + doclen
        lda #<_text
        adc _doclen
        sta nl_end
        lda #>_text
        adc _doclen+1
        sta nl_end+1
        lda #0
        sta ptr4                ; x = 0
        sta ptr4+1
        lda #$FF
        sta nl_space+1
        ldy #0
@loop:  lda ptr1                ; fin du texte ?
        cmp nl_end
        lda ptr1+1
        sbc nl_end+1
        bcc @more
        jmp @eof
@more:  lda (ptr1),y
        tax
        and #$F8
        cmp #$10
        bne @char
        txa                     ; marqueur de style
        and #7
        sta _nl_style
        jmp @next
@char:  cpx #13
        bne @width
        jsr @inc                ; retour a la ligne : la ligne suivante commence apres
        jmp @retptr
@width: lda _font_width-32,x    ; largeur + gras
        sta tmp1
        lda _nl_style
        and #1
        clc
        adc tmp1
        clc                     ; sreg = x + largeur
        adc ptr4
        sta sreg
        lda ptr4+1
        adc #0
        sta sreg+1
        lda _wrap_width         ; trop large si wrap_width < x + largeur
        cmp sreg
        lda _wrap_width+1
        sbc sreg+1
        bcs @fits
        lda nl_space+1          ; coupure au dernier espace s'il y en a un
        cmp #$FF
        beq @nospace
        lda nl_space
        sta ptr1
        lda nl_space+1
        sta ptr1+1
        lda nl_spst
        sta _nl_style
        jsr @inc
        jmp @retptr
@nospace:
        lda ptr1                ; mot plus long que la ligne : coupe ici
        cmp nl_start
        bne @retptr
        lda ptr1+1
        cmp nl_start+1
        bne @retptr
        jsr @inc                ; au moins un caractere par ligne
        jmp @retptr
@fits:  cpx #' '
        bne @adv
        lda ptr1
        sta nl_space
        lda ptr1+1
        sta nl_space+1
        lda _nl_style
        sta nl_spst
@adv:   lda sreg
        sta ptr4
        lda sreg+1
        sta ptr4+1
@next:  jsr @inc
        jmp @loop
@eof:   lda _doclen
        ldx _doclen+1
        rts
@retptr:
        sec                     ; position = ptr1 - text
        lda ptr1
        sbc #<_text
        pha
        lda ptr1+1
        sbc #>_text
        tax
        pla
        rts
@inc:   inc ptr1
        bne :+
        inc ptr1+1
:       rts

; void bm_move(unsigned dst, unsigned src, unsigned len) : deplace des octets
; du bitmap (ROM masquee), vers le bas ou vers le haut selon les adresses.
        .export _bm_move

_bm_move:
        sta tmp1                ; longueur
        stx tmp2
        jsr popax               ; source
        sta ptr2
        stx ptr2+1
        jsr popax               ; destination
        sta ptr1
        stx ptr1+1
        php
        sei
        lda $01
        pha
        lda #$35
        sta $01
        lda ptr1+1              ; destination avant la source : copie montante
        cmp ptr2+1
        bcc @up
        bne @down
        lda ptr1
        cmp ptr2
        bcc @up
@down:  clc                     ; copie descendante : on part de la fin
        lda ptr1+1
        adc tmp2
        sta ptr1+1
        clc
        lda ptr2+1
        adc tmp2
        sta ptr2+1
        ldy tmp1
        beq @dpages
@dtail: dey
        lda (ptr2),y
        sta (ptr1),y
        cpy #0
        bne @dtail
@dpages:
        ldx tmp2
        beq @done
@dpage: dec ptr1+1
        dec ptr2+1
        ldy #0
@dbyte: dey
        lda (ptr2),y
        sta (ptr1),y
        cpy #0
        bne @dbyte
        dex
        bne @dpage
        beq @done
@up:    ldy #0
        ldx tmp2
        beq @utail
@upage: lda (ptr2),y
        sta (ptr1),y
        iny
        bne @upage
        inc ptr1+1
        inc ptr2+1
        dex
        bne @upage
@utail: ldx tmp1
        beq @done
@ubyte: lda (ptr2),y
        sta (ptr1),y
        iny
        dex
        bne @ubyte
@done:  pla
        sta $01
        plp
        rts
