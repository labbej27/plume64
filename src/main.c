/* Plume : traitement de texte graphique pour Commodore 64.
   Souris 1351 (port 1) ou joystick (port 1 ou 2), police proportionnelle,
   impression MPS-803.
   (c) 2026 LABBEJ27. */
#include <cbm.h>
#include <c64.h>
#include <mouse.h>
#include <string.h>
#include <peekpoke.h>
#include "charmap.h"
#include "strings.h"

/* --- gfx.s --- */
extern unsigned gx, gbase;
extern unsigned char gch, gstyle;
void gfx_init(void);
void draw_glyph(void);
unsigned char __fastcall__ glyph_width(unsigned char code);
void __fastcall__ bm_fill(unsigned addr, unsigned len, unsigned char value);
void __fastcall__ bm_or(unsigned addr, unsigned char value);
void __fastcall__ bm_xormask(unsigned addr, unsigned char mask);
void __fastcall__ bm_move(unsigned dst, unsigned src, unsigned len);
unsigned next_line_asm(void);
extern unsigned nl_pos;
extern unsigned char nl_style;
extern const unsigned char font_width[];

#define BOLD      0x01
#define ITALIC    0x02
#define UNDERLINE 0x04

/* Le texte contient des marqueurs de style invisibles ($10 + style) aux seuls
   endroits ou le style change : environ 1 octet par caractere. */
#define IS_MARK(c) (((c) & 0xF8) == 0x10)
/* Largeur d'un caractere lue directement dans la table de la police
   (un octet par code a partir de 32). Gras : +1 pixel. */
#define CW(c, st)  ((font_width - 32)[c] + ((st) & BOLD))
#define MARK(s)    (0x10 | (s))

#define SCREEN    0xE000u
#define COLORS    ((unsigned char *)0xCC00)
#define ROWBASE(r) (SCREEN + (unsigned)(r) * 320u)
#define TEXT_X0   8
#define TEXT_W    304
#define TEXT_ROW0 2
#define VISIBLE   22
#define FOOT_ROW  24
#define MAXDOC    20000u
#define MAXLINES  950
#define CR        13
#define NOSEL     0xFFFFu
#define PRINT_CARDS_ 60

/* Memoire hors du programme :
   $C080 : debuts de ligne ; $D000 (sous les E/S, lus et ecrits E/S masquees) :
   ancienne mise en page pendant un recalcul, puis presse-papiers. */
#define lstart    ((unsigned *)0xC080)                        /* 2 octets par ligne */
#define lstyle    ((unsigned char *)(0xC080 + 2 * (MAXLINES + 2)))  /* style au debut de chaque ligne */
#define oldstart  ((unsigned *)0xD000)
#define oldstyle  ((unsigned char *)(0xD000 + 2 * (MAXLINES + 2)))
#define CLIP      ((unsigned char *)(0xD000 + 3 * (MAXLINES + 2)))
#define CLIPMAX   760
/* Noms de la boite Ouvrir : sous les E/S, apres le presse-papiers. */
#define names     ((char (*)[17])(0xD000 + 3 * (MAXLINES + 2) + CLIPMAX))
/* $0334-$03FB, tampon cassette inutilise : petites chaines. */
#define diskmsg   ((char *)0x0334)              /* 41 octets */
#define cmdname   ((char *)0x035D)              /* 24 */
#define findtext  ((unsigned char *)0x0375)     /* 24 */
#define filename  ((char *)0x038D)              /* 17, PETSCII */
#define statusbuf ((char *)0x039E)              /* 40 : barre d'etat */
#define inputbuf  ((char *)0x03C6)              /* 40 : saisie, jusqu'a $03ED */

/* Boite aux lettres de test (VICE ou API du C64 Ultimate) :
   1 = clic en x, y ; 4 = clic avec extension de la selection ;
   2 = gele le programme ROM masquee (repond 3) pour que l'API lise l'ecran
   en $E000, jusqu'a ce que l'outil remette 0. */
#define MAILBOX   ((volatile unsigned char *)0x02A7)

/* Couleurs : fond (4 bits bas) et encre (4 bits hauts). */
#define COL_PAGE   0x01
#define COL_BAR    0x0F
#define COL_MARGIN 0x0C

unsigned char text[MAXDOC];               /* exporte pour next_line_asm */
/* Tampon d'impression : une ligne du mode Texte, ou la sortie d'une passe du
   mode Graphique. Libre autrement. */
static unsigned char outbuf[PRINT_CARDS_ * 8 + 3];
unsigned doclen;
static unsigned nlines;
static unsigned relayout_last;
static unsigned relayout_from;          /* premiere ligne dont le contenu change */
static unsigned top;
static unsigned cursor;
static unsigned anchor = NOSEL;
static unsigned char style;
static unsigned char dirty;
static unsigned cliplen;
static unsigned char key;
unsigned wrap_width = TEXT_W;
static unsigned char next_style;             /* style a la fin de next_line() */

static void io_hide(void) { asm("sei"); POKE(0x01, 0x34); }
static void io_show(void) { POKE(0x01, 0x36); asm("cli"); }

/* ------------------------------------------------------------------ */
/* Dessin de base                                                      */

static unsigned put_text(unsigned x, unsigned char row, const char *s, unsigned char st)
{
    gbase = ROWBASE(row);
    gstyle = st;
    while (*s) {
        gch = (unsigned char)*s++;
        gx = x;
        draw_glyph();
        x += glyph_width(gch);
    }
    return x;
}

static unsigned text_width(const char *s, unsigned char st)
{
    unsigned w = 0;
    gstyle = st;
    while (*s) w += glyph_width((unsigned char)*s++);
    return w;
}

static void clear_cells(unsigned char row, unsigned x, unsigned w)
{
    bm_fill(ROWBASE(row) + x, w, 0);
}

static void color_cells(unsigned char row, unsigned char col, unsigned char n, unsigned char c)
{
    memset(COLORS + row * 40 + col, c, n);
}

static void hline(unsigned char row, unsigned x, unsigned w, unsigned char y)
{
    unsigned i;
    for (i = 0; i < w; i += 8) bm_or(ROWBASE(row) + x + i + y, 0xFF);
}

static void vline(unsigned char row, unsigned x, unsigned char mask)
{
    unsigned char y;
    for (y = 0; y < 8; ++y) bm_or(ROWBASE(row) + x + y, mask);
}

static void xor_span(unsigned char row, unsigned x0, unsigned x1)
{
    unsigned card;
    unsigned char a, b, mask;
    for (card = x0 & 0xFFF8u; card < x1; card += 8) {
        a = card < x0 ? (unsigned char)(x0 - card) : 0;
        b = card + 8 > x1 ? (unsigned char)(x1 - card) : 8;
        mask = (unsigned char)((0xFF >> a) & (0xFF << (8 - b)));
        bm_xormask(ROWBASE(row) + card, mask);
    }
}

static void petscii_to_text(char *dst, const char *src)
{
    unsigned char c;
    while ((c = (unsigned char)*src++) != 0) {
        if (c >= 0x41 && c <= 0x5A) c += 0x20;
        else if (c >= 0xC1 && c <= 0xDA) c -= 0x80;
        *dst++ = c;
    }
    *dst = 0;
}

static char *utoa_simple(char *b, unsigned n)
{
    char tmp[6];
    unsigned char k = 0;
    do { tmp[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) *b++ = tmp[--k];
    *b = 0;
    return b;
}

/* ------------------------------------------------------------------ */
/* Mise en page                                                        */

/* Debut de la ligne suivante, a partir de p dans le style st (assembleur,
   gfx.s). next_style recoit le style en vigueur a cette position. */
static unsigned next_line(unsigned p, unsigned char st)
{
    unsigned n;
    nl_pos = p;
    nl_style = st;
    n = next_line_asm();
    next_style = nl_style;
    return n;
}

static void layout_all(void)
{
    unsigned s = 0, n;
    unsigned char st = 0;
    nlines = 0;
    for (;;) {
        lstyle[nlines] = st;
        lstart[nlines++] = s;
        if (nlines >= MAXLINES) break;
        n = next_line(s, st);
        st = next_style;
        if (n >= doclen) {
            if (doclen && text[doclen - 1] == CR && s != doclen) {
                lstyle[nlines] = st;
                lstart[nlines++] = doclen;
            }
            break;
        }
        s = n;
    }
    lstart[nlines] = doclen;
}

static unsigned line_of(unsigned p)
{
    unsigned lo = 0, hi = nlines - 1, mid;
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        if (lstart[mid] <= p) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Style en vigueur pour un caractere tape en p (marqueurs avant p compris). */
static unsigned char style_before(unsigned p)
{
    unsigned i = line_of(p), q;
    unsigned char st = lstyle[i];
    for (q = lstart[i]; q < p; ++q) if (IS_MARK(text[q])) st = text[q] & 7;
    return st;
}

/* Recalcule les lignes apres le remplacement de [p, old_end) par un texte qui
   finit en new_end. S'arrete des que l'ancienne mise en page est retrouvee. */
static unsigned relayout(unsigned p, unsigned old_end, unsigned new_end, unsigned char *all)
{
    unsigned first, i, j, s, n, oldn = nlines;
    int delta = (int)(new_end - p) - (int)(old_end - p);
    unsigned char st;
    io_hide();
    memcpy(oldstart, lstart, (nlines + 1) * sizeof(unsigned));
    memcpy(oldstyle, lstyle, nlines + 1);
    for (j = 0; j <= oldn; ++j) {
        if (oldstart[j] > p) {
            if (oldstart[j] <= old_end) oldstart[j] = p;
            else oldstart[j] += delta;
        }
    }
    first = line_of(p);
    if (first) --first;
    i = first;
    j = first + 1;
    s = lstart[first];
    st = lstyle[first];
    *all = 0;
    relayout_from = first;
    for (;;) {
        if (i + 1 >= MAXLINES) { nlines = i + 1; break; }
        n = next_line(s, st);
        st = next_style;
        if (n >= doclen) {
            if (doclen && text[doclen - 1] == CR && s != doclen) { lstart[++i] = doclen; lstyle[i] = st; }
            nlines = i + 1;
            break;
        }
        while (j < oldn && oldstart[j] < n) ++j;
        if (j < oldn && oldstart[j] == n && n > new_end && oldstyle[j] == st) {
            memcpy(&lstart[i + 1], &oldstart[j], (oldn - j) * sizeof(unsigned));
            memcpy(&lstyle[i + 1], &oldstyle[j], oldn - j);
            nlines = i + 1 + (oldn - j);
            break;
        }
        if (i == first && n == oldstart[first + 1]) relayout_from = first + 1;
        lstart[++i] = n;
        lstyle[i] = st;
        s = n;
    }
    io_show();
    relayout_last = i;
    lstart[nlines] = doclen;
    if (nlines != oldn) *all = 1;
    return first;
}

/* ------------------------------------------------------------------ */
/* Affichage du document                                               */

static unsigned sel_lo(void) { return anchor == NOSEL ? cursor : (anchor < cursor ? anchor : cursor); }
static unsigned sel_hi(void) { return anchor == NOSEL ? cursor : (anchor < cursor ? cursor : anchor); }

static void draw_line(unsigned i)
{
    unsigned char row, st, c;
    unsigned p, end, x = TEXT_X0, lo = sel_lo(), hi = sel_hi(), sx = 0, ex = 0;
    if (i < top || i >= top + VISIBLE) return;
    row = (unsigned char)(TEXT_ROW0 + i - top);
    clear_cells(row, 0, 320);                   /* marges comprises : efface les ombres des menus */
    if (i >= nlines) return;
    end = lstart[i + 1];
    st = lstyle[i];
    gbase = ROWBASE(row);
    for (p = lstart[i]; p < end; ++p) {
        if (p == lo) sx = x;
        if (p == hi) ex = x;
        c = text[p];
        if (IS_MARK(c)) { st = c & 7; continue; }
        if (c == CR) { if (p >= lo && p < hi) x += 4; break; }
        gch = c;
        gstyle = st;
        gx = x;
        draw_glyph();
        x += CW(c, st);
    }
    if (lo < hi && lo < end && hi > lstart[i]) {
        if (lo < lstart[i]) sx = TEXT_X0;
        if (hi >= end || (p < hi && ex == 0)) ex = x;
        if (ex > sx) xor_span(row, sx, ex);
    }
}

static void draw_lines(unsigned from, unsigned to)
{
    unsigned i;
    if (from < top) from = top;
    for (i = from; i <= to && i < top + VISIBLE; ++i) draw_line(i);
}

static void draw_text_area(void)
{
    draw_lines(top, top + VISIBLE - 1);
}

static unsigned cursor_x(void)
{
    unsigned i = line_of(cursor), p, x = 0;
    unsigned char st = lstyle[i], c;
    for (p = lstart[i]; p < cursor; ++p) {
        c = text[p];
        if (IS_MARK(c)) st = c & 7; else x += CW(c, st);
    }
    return x;
}

static unsigned char cursor_shown;

static void place_cursor(void)
{
    unsigned i = line_of(cursor), x;
    unsigned char row;
    if (i + 1 == top) {                         /* une ligne plus haut : decale l'image */
        bm_move(ROWBASE(TEXT_ROW0 + 1), ROWBASE(TEXT_ROW0), (VISIBLE - 1) * 320u);
        top = i;
        draw_line(i);
    } else if (i == top + VISIBLE) {            /* une ligne plus bas */
        bm_move(ROWBASE(TEXT_ROW0), ROWBASE(TEXT_ROW0 + 1), (VISIBLE - 1) * 320u);
        ++top;
        draw_line(i);
    } else if (i < top) { top = i; draw_text_area(); }
    else if (i >= top + VISIBLE) { top = i - VISIBLE + 1; draw_text_area(); }
    row = (unsigned char)(TEXT_ROW0 + i - top);
    x = TEXT_X0 + cursor_x() + 24;
    VIC.spr1_x = (unsigned char)x;
    if (x > 255) VIC.spr_hi_x |= 0x02; else VIC.spr_hi_x &= 0xFD;
    VIC.spr1_y = (unsigned char)(row * 8 + 50);
    cursor_shown = 1;
}

static void blink_cursor(void)
{
    unsigned char on = (anchor == NOSEL || anchor == cursor) && (PEEK(0xA2) & 0x20);
    if (cursor_shown && !on) { VIC.spr_ena &= 0xFD; cursor_shown = 0; }
    else if (!cursor_shown && on) { VIC.spr_ena |= 0x02; cursor_shown = 1; }
}

/* ------------------------------------------------------------------ */
/* Barre d'etat                                                        */

#define STYLE_X 262

/* Barre de titre : nom du document et boutons de style, redessinee seulement
   quand elle change. Barre du bas : ligne et place libre. */
static unsigned char shown_dirty = 255, shown_style = 255;

static void draw_title(void)
{
    static char letter[2];
    char *buf = statusbuf;
    unsigned x;
    unsigned char k;
    shown_dirty = dirty;
    shown_style = style;
    clear_cells(1, 0, 320);
    petscii_to_text(buf, filename);
    if (!filename[0]) strcpy(buf, S_UNTITLED);
    x = put_text(6, 1, buf, BOLD);
    if (dirty) put_text(x + 2, 1, "*", BOLD);
    for (k = 0; k < 3; ++k) {
        x = STYLE_X + k * 18;
        letter[0] = S_STYLEKEYS[k];
        put_text(x + 4, 1, letter, (unsigned char)(1 << k));
        xor_span(1, x, x + 1);
        xor_span(1, x + 15, x + 16);
        if (style & (1 << k)) xor_span(1, x + 1, x + 15);
    }
}

static void draw_status(void)
{
    char *buf = statusbuf, *b;
    if (dirty != shown_dirty || style != shown_style) draw_title();
    clear_cells(FOOT_ROW, 0, 320);
    b = buf;
    strcpy(b, S_LINE " ");
    b = utoa_simple(b + strlen(b), line_of(cursor) + 1);
    *b++ = '/';
    utoa_simple(b, nlines);
    put_text(6, FOOT_ROW, buf, 0);
    strcpy(buf, S_FREE " ");
    utoa_simple(buf + strlen(buf), MAXDOC - doclen);
    put_text(314 - text_width(buf, 0), FOOT_ROW, buf, 0);
}

/* ------------------------------------------------------------------ */
/* Menus et panneaux                                                   */

#define NMENUS 5
static const char *const menu_title[NMENUS] = { S_MENU0, S_MENU1, S_MENU2, S_MENU3, S_MENU4 };
static const char *const menu_items[NMENUS][7] = {
    { S_NEW, S_OPEN, S_SAVE, S_SAVEAS, 0 },
    { S_CUT, S_COPY, S_PASTE, S_SELALL, S_FIND, S_FINDNEXT, 0 },
    { S_PLAIN, S_BOLD, S_ITALIC, S_UNDERLINE, 0 },
    { S_PGRAPH, S_PTEXT, S_PWIDE, 0 },
    { S_ABOUT, 0 },
};
static const char *const menu_keys[NMENUS][7] = {
    { "", "", "F1", "", 0 },
    { "F6", "F2", "F4", "", "", "F8", 0 },
    { "", "F3", "F5", "F7", 0 },
    { "", "", "", 0 },
    { "", 0 },
};
static unsigned menu_x[NMENUS + 1];

static void draw_menubar(void)
{
    unsigned char m;
    unsigned x = 8;
    clear_cells(0, 0, 320);
    for (m = 0; m < NMENUS; ++m) {
        menu_x[m] = x;
        put_text(x + 4, 0, menu_title[m], 0);
        x += (text_width(menu_title[m], 0) + 16) & 0xFFF8u;
    }
    menu_x[NMENUS] = x;
    put_text(320 - 8 - text_width("Plume", BOLD), 0, "Plume", BOLD);
}

static void page_colors(void)
{
    unsigned char r;
    color_cells(0, 0, 40, COL_BAR);
    color_cells(1, 0, 40, COL_BAR);
    color_cells(FOOT_ROW, 0, 40, COL_BAR);
    for (r = TEXT_ROW0; r < FOOT_ROW; ++r) {
        color_cells(r, 0, 1, COL_MARGIN);
        color_cells(r, 1, 38, COL_PAGE);
        color_cells(r, 39, 1, COL_MARGIN);
    }
}

static void redraw_all(void)
{
    shown_dirty = 255;
    page_colors();
    draw_menubar();
    draw_status();
    draw_text_area();
    place_cursor();
}

static void panel(unsigned char row, unsigned char rows, unsigned x, unsigned w)
{
    unsigned char r;
    for (r = row; r < row + rows; ++r) {
        color_cells(r, (unsigned char)(x / 8), (unsigned char)(w / 8 + 1), COL_PAGE);
        clear_cells(r, x, w);
        vline(r, x, 0x80);
        vline(r, x + w - 8, 0x01);
        bm_fill(ROWBASE(r) + x + w, 8, r == row ? 0 : 0xC0);
    }
    hline(row, x, w, 0);
    hline((unsigned char)(row + rows - 1), x, w, 7);
    if (row + rows < 25) {
        color_cells((unsigned char)(row + rows), (unsigned char)(x / 8), (unsigned char)(w / 8 + 1), COL_PAGE);
        bm_fill(ROWBASE(row + rows) + x + 8, w, 0);
        hline((unsigned char)(row + rows), x + 8, w, 0);
        hline((unsigned char)(row + rows), x + 8, w, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Souris et clavier                                                   */

static struct mouse_info mi;
static unsigned mouse_x;
static unsigned char mouse_y, clicked, extend, held, last_buttons;
/* Joystick : le pointeur avance d'un pixel par image, puis accelere. */
unsigned char joy_read(void);
extern unsigned char joy_p1;
static unsigned char joy_speed, joy_tick, joy_p1_time;

static void poll_mouse(void)
{
    unsigned char j, s;
    clicked = 0;
    extend = 0;
    if (MAILBOX[0] == 2) {
        asm("sei");
        POKE(0x01, 0x35);
        MAILBOX[0] = 3;
        while (MAILBOX[0] == 3) { }
        POKE(0x01, 0x36);
        asm("cli");
    }
    if (MAILBOX[0] == 1 || MAILBOX[0] == 4) {
        mouse_x = MAILBOX[1] | (MAILBOX[2] << 8);
        mouse_y = MAILBOX[3];
        extend = MAILBOX[0] == 4;
        MAILBOX[0] = 0;
        clicked = 1;
        held = 0;
        return;
    }
    mouse_info(&mi);
    mouse_x = mi.pos.x;
    mouse_y = (unsigned char)mi.pos.y;
    j = joy_read();
    if (joy_p1) joy_p1_time = PEEK(0xA2);
    if (!(j & 0x0F)) joy_speed = 0;
    else if (PEEK(0xA2) != joy_tick) {
        joy_tick = PEEK(0xA2);
        if (joy_speed < 48) ++joy_speed;
        s = 1 + (joy_speed >> 4);         /* 1 a 4 pixels par image */
        if (j & 4) mouse_x = mouse_x > s ? mouse_x - s : 0;
        if (j & 8) { mouse_x += s; if (mouse_x > 319) mouse_x = 319; }
        if (j & 1) mouse_y = mouse_y > s ? mouse_y - s : 0;
        if (j & 2) { mouse_y += s; if (mouse_y > 199) mouse_y = 199; }
        mouse_move(mouse_x, mouse_y);
    }
    held = (j & 0x10) || (mi.buttons & MOUSE_BTN_LEFT);
    if (held && !last_buttons) clicked = 1;
    last_buttons = held;
}


/* Un joystick branche au port 1 se lit sur les memes fils que le clavier :
   le KERNAL y voit des touches fantomes (DEL, RETURN...). On les jette tant
   que ce joystick bouge, et un instant apres. */
#ifdef KBD_HU
static unsigned char hu_key(void);
static unsigned char hu_last;
#endif

static unsigned char read_key(void)
{
#ifdef KBD_HU
    unsigned char k;
#endif
    if ((unsigned char)(PEEK(0xA2) - joy_p1_time) < 6) {
        POKE(198, 0);
        return 0;
    }
#ifdef KBD_HU
    /* Tant que C= est enfoncee, on jette ce que le KERNAL a pu mettre dans
       son tampon : sa couche C= ne fait pas Shift, et notre lecture de la
       matrice pendant sa scrutation peut lui donner des touches fantomes. */
    k = hu_key();
    if (k || hu_last != 0xFF) {         /* C= tenue, ou touche lue */
        POKE(198, 0);
        return k;
    }
#endif
    return cbm_k_getin();
}

/* ------------------------------------------------------------------ */
/* Boites de dialogue                                                  */

#define DLG_ROW  7
#define DLG_X    40
#define DLG_W    240
#define BTN_W    72

static void dialog(unsigned char rows, const char *title)
{
    unsigned char y;
    unsigned w;
    panel(DLG_ROW, rows, DLG_X, DLG_W);
    for (y = 2; y < 7; y += 2) hline(DLG_ROW, DLG_X + 8, DLG_W - 16, y);
    if (title) {
        w = (text_width(title, BOLD) + 16) & 0xFFF8u;
        clear_cells(DLG_ROW, DLG_X + (DLG_W - w) / 2, w);
        put_text(DLG_X + (DLG_W - w) / 2 + 4, DLG_ROW, title, BOLD);
    }
}

static void button(unsigned x, unsigned char row, const char *label)
{
    hline((unsigned char)(row - 1), x + 8, BTN_W - 16, 6);
    hline((unsigned char)(row + 1), x + 8, BTN_W - 16, 1);
    vline(row, x, 0x10);
    vline(row, x + BTN_W - 8, 0x08);
    bm_or(ROWBASE(row - 1) + x + 7, 0x0F);
    bm_or(ROWBASE(row - 1) + x + BTN_W - 8 + 7, 0xF0);
    bm_or(ROWBASE(row + 1) + x, 0x0F);
    bm_or(ROWBASE(row + 1) + x + BTN_W - 8, 0xF0);
    put_text(x + (BTN_W - text_width(label, BOLD)) / 2, row, label, BOLD);
}

static unsigned char on_button(unsigned x, unsigned char row)
{
    return clicked && mouse_x >= x && mouse_x < x + BTN_W
        && mouse_y / 8 + 1 >= row && mouse_y / 8 <= row + 1;
}

#define BTN_OK_X     (DLG_X + 24)
#define BTN_CANCEL_X (DLG_X + DLG_W - 24 - BTN_W)

static void dialog_close(void)
{
    redraw_all();
}

static unsigned char message(const char *title, const char *line1, const char *line2, unsigned char cancel)
{
    dialog(8, title);
    if (line1) put_text(DLG_X + 16, DLG_ROW + 2, line1, 0);
    if (line2) put_text(DLG_X + 16, DLG_ROW + 3, line2, 0);
    button(BTN_OK_X, DLG_ROW + 5, S_OK);
    if (cancel) button(BTN_CANCEL_X, DLG_ROW + 5, S_CANCEL);
    for (;;) {
        poll_mouse();
        key = read_key();
        if (key == CR || on_button(BTN_OK_X, DLG_ROW + 5)) { dialog_close(); return 1; }
        if (cancel && (key == 3 || on_button(BTN_CANCEL_X, DLG_ROW + 5))) { dialog_close(); return 0; }
    }
}

static unsigned char translate(unsigned char k);

static unsigned char ask_text(const char *title, const char *label, char *buf, unsigned char max,
                              unsigned char petscii)
{
    char *shown = inputbuf;
    unsigned char len = (unsigned char)strlen(buf), c, first = 1;
    for (;;) {
        if (first) {
            dialog(8, title);
            put_text(DLG_X + 16, DLG_ROW + 2, label, 0);
            button(BTN_OK_X, DLG_ROW + 5, S_OK);
            button(BTN_CANCEL_X, DLG_ROW + 5, S_CANCEL);
            first = 0;
        }
        clear_cells(DLG_ROW + 3, DLG_X + 8, DLG_W - 16);
        hline(DLG_ROW + 3, DLG_X + 16, DLG_W - 32, 7);
        if (petscii) petscii_to_text(shown, buf); else strcpy(shown, buf);
        strcat(shown, "|");
        put_text(DLG_X + 16, DLG_ROW + 3, shown, BOLD);
        do {
            poll_mouse();
            key = read_key();
            if (on_button(BTN_OK_X, DLG_ROW + 5)) key = CR;
            if (on_button(BTN_CANCEL_X, DLG_ROW + 5)) key = 3;
        } while (!key);
        if (key == CR && len) { dialog_close(); return 1; }
        if (key == 3) { dialog_close(); return 0; }
        if (key == 0x14) { if (len) buf[--len] = 0; continue; }
        if (len >= max) continue;
        if (petscii) {
            if (((key >= 0x20 && key <= 0x5A) || (key >= 0xC1 && key <= 0xDA))
                && key != ',' && key != ':' && key != '*' && key != '?' && key != '"' && key != '=') {
                buf[len++] = key;
            }
        } else if ((c = translate(key)) != 0) {
            buf[len++] = c;
        }
        buf[len] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Modifications du texte                                              */

static void refresh_after(unsigned first, unsigned char all)
{
    if (all) draw_lines(first, top + VISIBLE - 1);
    else draw_lines(relayout_from, relayout_last);
    place_cursor();
    draw_status();
}

/* Remplace [a, b) par n octets de src (src sous les E/S si from_io).
   Renvoie 0 si le document deborderait. */
static unsigned char replace(unsigned a, unsigned b, const unsigned char *src, unsigned n,
                             unsigned char from_io)
{
    unsigned char all;
    unsigned first;
    if (doclen - (b - a) + n > MAXDOC) { message(S_FULL, 0, 0, 0); return 0; }
    memmove(&text[a + n], &text[b], doclen - b);
    if (from_io) io_hide();
    memcpy(&text[a], src, n);
    if (from_io) io_show();
    doclen = doclen - (b - a) + n;
    dirty = 1;
    first = relayout(a, b, a + n, &all);
    refresh_after(first, all);
    return 1;
}

/* Position canonique : avant les marqueurs qui suivent un caractere. */
static unsigned canonical(unsigned p)
{
    while (p && IS_MARK(text[p - 1])) --p;
    return p;
}

/* ------------------------------------------------------------------ */
/* Disque                                                              */


static unsigned char disk_status(void)
{
    unsigned char n;
    int r;
    diskmsg[0] = 0;
    if (cbm_open(15, 8, 15, "")) return 99;
    r = cbm_read(15, diskmsg, 40);
    cbm_close(15);
    if (r <= 0) return 99;
    n = (unsigned char)r;
    while (n && (diskmsg[n - 1] == CR || diskmsg[n - 1] == '\n')) --n;
    diskmsg[n] = 0;
    return (unsigned char)((diskmsg[0] - '0') * 10 + diskmsg[1] - '0');
}

static void show_disk_error(void)
{
    char *shown = (char *)outbuf;             /* libre hors impression */
    petscii_to_text(shown, diskmsg);
    message(S_DISKERR, shown, 0, 0);
}

static unsigned char ask_filename(void)
{
    char *buf = (char *)outbuf + 64;          /* libre hors impression */
    strcpy(buf, filename);
    if (!ask_text(S_SAVEAS, S_FILENAME, buf, 16, 1)) return 0;
    strcpy(filename, buf);
    return 1;
}

/* Format PLM2 : "PLM2", longueur (2 octets), texte avec marqueurs. */
static void save_doc(void)
{
    unsigned char head[6];
    if (!filename[0] && !ask_filename()) return;
    strcpy(cmdname, "@0:");
    strcat(cmdname, filename);
    strcat(cmdname, ",S,W");
    memcpy(head, "PLM2", 4);
    head[4] = (unsigned char)doclen;
    head[5] = (unsigned char)(doclen >> 8);
    if (cbm_open(2, 8, 2, cmdname) == 0) {
        cbm_write(2, head, 6);
        cbm_write(2, text, doclen);
        cbm_close(2);
    }
    if (disk_status() >= 20) { show_disk_error(); return; }
    dirty = 0;
    draw_status();
}

static void new_doc(void)
{
    doclen = 0;
    cursor = 0;
    anchor = NOSEL;
    top = 0;
    style = 0;
    dirty = 0;
    filename[0] = 0;
    layout_all();
}

/* PLM1 (versions 1.0 et 1.1) : texte puis un octet de style par caractere.
   Le texte est lu en fin de tampon, puis fusionne au debut avec ses marqueurs. */
static unsigned char load_plm1(unsigned n)
{
    unsigned r = MAXDOC - n, w = 0, k;
    unsigned char st = 0, *chunk = outbuf, c, i;  /* tampon d'impression, libre ici */
    cbm_read(2, &text[r], n);
    for (k = 0; k < n; k += 64) {
        c = (unsigned char)(n - k > 64 ? 64 : n - k);
        cbm_read(2, chunk, c);
        for (i = 0; i < c; ++i) {
            if ((chunk[i] & 7) != st) {
                st = chunk[i] & 7;
                if (w >= r) return 0;
                text[w++] = MARK(st);
            }
            if (w > r) return 0;
            text[w++] = text[r++];
        }
    }
    doclen = w;
    return 1;
}

static void load_named(void)
{
    unsigned char head[6], ok = 1;
    unsigned n;
    strcpy(cmdname, filename);
    strcat(cmdname, ",S,R");
    if (cbm_open(2, 8, 2, cmdname) != 0) { disk_status(); show_disk_error(); return; }
    cbm_read(2, head, 6);
    n = head[4] | (head[5] << 8);
    if (!memcmp(head, "PLM2", 4) && n <= MAXDOC) {
        cbm_read(2, text, n);
        doclen = n;
    } else if (!memcmp(head, "PLM1", 4) && n <= MAXDOC) {
        ok = load_plm1(n);
    } else {
        ok = 0;
    }
    cbm_close(2);
    if (!ok) { doclen = 0; message(S_OPEN, S_BADFILE, 0, 0); }
    else if (disk_status() >= 20) { show_disk_error(); return; }
    cursor = 0;
    anchor = NOSEL;
    top = 0;
    dirty = 0;
    layout_all();
    style = style_before(0);
}

#define MAXFILES 15

static void open_doc(void)
{
    struct cbm_dirent e;
    unsigned char count = 0, i, sel = 255, rows;
    char *shown = inputbuf;
    if (dirty && !message(S_OPEN, S_LOSE, 0, 1)) return;
    if (cbm_opendir(1, 8) == 0) {
        while (count < MAXFILES && !cbm_readdir(1, &e)) {
            if (e.type == CBM_T_SEQ) { io_hide(); strcpy(names[count++], e.name); io_show(); }
        }
        cbm_closedir(1);
    }
    if (!count) {
        if (disk_status() >= 20) show_disk_error(); else message(S_OPEN, S_NOFILES, 0, 0);
        return;
    }
    rows = (unsigned char)((count + 1) / 2);
    panel(4, (unsigned char)(rows + 6), DLG_X, DLG_W);
    put_text(DLG_X + 16, 5, S_PICK, BOLD);
    for (i = 0; i < count; ++i) {
        io_hide();
        strcpy(shown, names[i]);
        io_show();
        petscii_to_text(shown, shown);
        put_text(DLG_X + 16 + (i & 1) * 112, (unsigned char)(7 + i / 2), shown, 0);
    }
    button(BTN_CANCEL_X, (unsigned char)(8 + rows), S_CANCEL);
    while (sel == 255) {
        poll_mouse();
        key = read_key();
        if (key == 3 || on_button(BTN_CANCEL_X, (unsigned char)(8 + rows))) break;
        if (clicked && mouse_y / 8 >= 7 && mouse_y / 8 < 7 + rows) {
            i = (unsigned char)((mouse_y / 8 - 7) * 2 + (mouse_x >= DLG_X + 128));
            if (i < count) sel = i;
        }
    }
    if (sel != 255) {
        io_hide();
        strcpy(filename, names[sel]);
        io_show();
        load_named();
    }
    redraw_all();
}

/* ------------------------------------------------------------------ */
/* Impression sur MPS-803 (bus serie, peripherique 4)                  */
/* Largeur A4 avec 3 cm de marge de chaque cote : 15 cm de texte.      */

#define PRINT_CARDS 60
#define PRINT_LEFT  71
#define PRINT_W     354
#define TEXT_LEFT   12
#define TEXT_COLS   59
#define WIDE_LEFT   6
#define WIDE_COLS   29
#define TEAR_GRAPH  12
#define TEAR_TEXT   8

/* La ligne en cours de dessin (strip) n'est plus lue quand une passe part :
   elle partage donc la place de outbuf. rows, lui, vit en meme temps que
   outbuf (emit_pass), il garde la sienne. */
#define strip outbuf
static unsigned char rows[14][PRINT_CARDS];
static unsigned char pending;
static unsigned char print_ok;

static unsigned char stop_pressed(void)
{
    return PEEK(0x91) == 0x7F;
}

static void pr_send(const void *data, unsigned n)
{
    if (!print_ok || !n) return;
    cbm_write(4, data, n);
    if (cbm_k_readst() & 0x80) print_ok = 0;
}

static void pr_byte(unsigned char b)
{
    pr_send(&b, 1);
}

static void emit_pass(void)
{
    unsigned char card, mask, r, b, *o = outbuf;
    *o++ = 8;
    for (card = 0; card < PRINT_CARDS; ++card) {
        for (mask = 0x80; mask; mask >>= 1) {
            b = 0x80;
            for (r = 0; r < 7; ++r) {
                if (rows[r][card] & mask) b |= 1 << r;
            }
            *o++ = b;
        }
    }
    *o++ = CR;
    pr_send(outbuf, (unsigned)(o - outbuf));
    pending -= 7;
    memmove(rows[0], rows[7], pending * PRINT_CARDS);
}

static void print_graphics(void)
{
    unsigned p = 0, n, q, x;
    unsigned char r, card, st = 0, c;
    pending = 0;
    wrap_width = PRINT_W;
    while (print_ok && !stop_pressed()) {
        n = next_line(p, st);
        memset(strip, 0, sizeof strip);
        gbase = (unsigned)strip;
        x = PRINT_LEFT;
        for (q = p; q < n; ++q) {
            c = text[q];
            if (IS_MARK(c)) { st = c & 7; continue; }
            if (c == CR) continue;
            gch = c;
            gstyle = st;
            gx = x;
            draw_glyph();
            x += CW(c, st);
        }
        st = next_style;
        for (r = 0; r < 8; ++r) {
            for (card = 0; card < PRINT_CARDS; ++card) rows[pending + r][card] = strip[card * 8 + r];
        }
        pending += 8;
        while (pending >= 7) emit_pass();
        if (n >= doclen) break;
        p = n;
    }
    wrap_width = TEXT_W;
    if (pending) {
        memset(rows[pending], 0, (7 - pending) * PRINT_CARDS);
        pending = 7;
        emit_pass();
    }
    pr_byte(8);
    for (r = 0; r < TEAR_GRAPH; ++r) pr_byte(CR);
    pr_byte(15);
}

/* Mode texte de l'imprimante : lettre de base des codes 128 à 180
   (œ Œ -> O ; « » ° € -> espace). */
static const char accent_base[] = "aaaceeeeiioouuuAACEEEEIIOUUU      aoaAOAaAiIoOuUoOuUO";

static unsigned char to_printer(unsigned char c)
{
    if (c >= 128) {
        c -= 128;
        if (c == 28 || c == 29) return 'O';
        if (c < sizeof accent_base - 1) c = (unsigned char)accent_base[c];
        else return ' ';
    }
    if (c >= 'a' && c <= 'z') return c - 0x20;
    if (c >= 'A' && c <= 'Z') return c + 0x80;
    return c;
}

static void print_text(unsigned char wide)
{
    unsigned char limit = wide ? WIDE_COLS : TEXT_COLS, left = wide ? WIDE_LEFT : TEXT_LEFT, n, k;
    unsigned p = 0, word, w, q;
    unsigned char *line = outbuf;              /* tampon graphique, libre en mode texte */
    if (wide) pr_byte(14);
    memset(line, ' ', left);
    n = left;
    while (p < doclen && print_ok && !stop_pressed()) {
        if (IS_MARK(text[p])) { ++p; continue; }
        if (text[p] == CR) {
            pr_send(line, n); pr_byte(CR); n = left; ++p;
            continue;
        }
        /* longueur visible du mot suivant (espace comprise) */
        w = 0;
        for (word = p; word < doclen && text[word] != CR; ++word) {
            if (IS_MARK(text[word])) continue;
            ++w;
            if (text[word] == ' ') { ++word; break; }
        }
        if (n - left + w > limit && n > left) { pr_send(line, n); pr_byte(CR); n = left; }
        for (q = p; q < word; ++q) {
            if (IS_MARK(text[q])) continue;
            if (n - left == limit) { pr_send(line, n); pr_byte(CR); n = left; }
            line[n++] = to_printer(text[q]);
        }
        p = word;
    }
    if (n > left) { pr_send(line, n); pr_byte(CR); }
    for (k = 0; k < TEAR_TEXT; ++k) pr_byte(CR);
    if (wide) pr_byte(15);
}

static void print_doc(unsigned char mode)
{
    dialog(5, S_MENU3);
    put_text(DLG_X + 16, DLG_ROW + 2, S_PRINTING, 0);
    print_ok = 1;
    cbm_open(4, 4, mode ? 7 : 0, "");
    if (cbm_k_readst() & 0x80) print_ok = 0;
    if (print_ok) {
        if (mode == 0) print_graphics(); else print_text(mode == 2);
    }
    cbm_close(4);
    dialog_close();
    if (!print_ok) message(S_MENU3, S_NOPRINTER, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Edition                                                             */

static void set_selection(unsigned new_anchor, unsigned new_cursor)
{
    unsigned a = sel_lo(), b = sel_hi(), la, lb;
    anchor = new_anchor;
    cursor = new_cursor;
    if (sel_lo() < a) a = sel_lo();
    if (sel_hi() > b) b = sel_hi();
    la = line_of(a);
    lb = line_of(b);
    place_cursor();
    draw_lines(la, lb);
    draw_status();
}

static void clear_selection(void)
{
    if (anchor != NOSEL) set_selection(NOSEL, cursor);
}

static unsigned char has_selection(void)
{
    return anchor != NOSEL && anchor != cursor;
}

/* Supprime [a, b) en gardant le style du texte qui suit. */
static void delete_range(unsigned a, unsigned b)
{
    unsigned char after, mark;
    a = canonical(a);
    if (a >= b) return;
    after = style_before(b);
    mark = MARK(after);
    anchor = NOSEL;
    cursor = a;
    if (after != style_before(a) && b < doclen && !IS_MARK(text[b])) replace(a, b, &mark, 1, 0);
    else replace(a, b, 0, 0, 0);
}

static void delete_selection(void)
{
    if (has_selection()) delete_range(sel_lo(), sel_hi());
    else anchor = NOSEL;
}

static void insert_char(unsigned char c)
{
    unsigned char seq[3], n = 0, here;
    unsigned p;
    delete_selection();
    p = cursor;
    here = style_before(p);
    if (style != here) seq[n++] = MARK(style);
    seq[n++] = c;
    if (style != here && p < doclen && !IS_MARK(text[p])) seq[n++] = MARK(here);
    cursor = p + (style != here ? 2 : 1);
    if (!replace(p, p, seq, n, 0)) cursor = p;
}

static void delete_before(void)
{
    if (has_selection()) { delete_selection(); return; }
    anchor = NOSEL;
    if (cursor) delete_range(cursor - 1, cursor);
}

/* Presse-papiers : marqueur du style de depart, puis le texte et ses marqueurs. */
static void copy_selection(void)
{
    unsigned a = sel_lo(), n = sel_hi() - a;
    unsigned char st, k;
    if (!has_selection()) return;
    if (n + 1 > CLIPMAX) { message(S_COPY, S_CLIPFULL, 0, 0); return; }
    st = style_before(a);
    k = IS_MARK(text[a]) ? 0 : 1;               /* la selection commence-t-elle par un marqueur ? */
    io_hide();
    CLIP[0] = MARK(st);
    memcpy(CLIP + k, &text[a], n);
    io_show();
    cliplen = n + k;
}

static void cut_selection(void)
{
    unsigned n = sel_hi() - sel_lo();
    if (!has_selection()) return;
    copy_selection();
    if (cliplen >= n) delete_selection();
}

static void paste(void)
{
    unsigned p, k;
    unsigned char here, last = 0, mark, c;
    if (!cliplen) return;
    delete_selection();
    p = cursor;
    here = style_before(p);
    io_hide();
    for (k = 0; k < cliplen; ++k) { c = CLIP[k]; if (IS_MARK(c)) last = c & 7; }
    io_show();
    /* le marqueur de depart est inutile s'il repete le style du point d'insertion */
    k = 0;
    io_hide();
    if (CLIP[0] == MARK(here)) k = 1;
    io_show();
    if (!replace(p, p, CLIP + k, cliplen - k, 1)) return;
    cursor = p + cliplen - k;
    if (last != here && cursor < doclen && !IS_MARK(text[cursor])) {
        mark = MARK(here);
        k = cursor;
        replace(k, k, &mark, 1, 0);
        cursor = k;
    }
}

/* Style : sur la selection s'il y en a une, sinon pour la frappe. bit = 0 : normal.
   La selection et la suite du texte sont deplacees en haut du tampon, puis la
   selection est reecrite en un seul passage avec ses nouveaux marqueurs. */
static void apply_style(unsigned char bit)
{
    unsigned a, b, w, sel_end, need = 0;
    unsigned char all, every = 1, old_st, start_st, emitted, want, c, k;
    unsigned char map[8];                       /* ancien style -> nouveau style */
    unsigned char *q, *end, *rd;
    unsigned first;
    if (!has_selection()) {
        if (bit) style ^= bit; else style = 0;
        draw_status();
        return;
    }
    a = sel_lo();
    b = sel_hi();
    start_st = old_st = style_before(a);
    end = &text[b];
    for (q = &text[a]; q < end; ++q) {
        c = *q;
        if (IS_MARK(c)) old_st = c & 7; else if (!(old_st & bit)) { every = 0; break; }
    }
    for (k = 0; k < 8; ++k) map[k] = !bit ? 0 : every ? (unsigned char)(k & ~bit) : (unsigned char)(k | bit);
    /* marqueurs a ajouter, pour verifier la place avant de toucher au texte */
    emitted = old_st = start_st;
    for (q = &text[a]; q < end; ++q) {
        c = *q;
        if (IS_MARK(c)) { old_st = c & 7; continue; }
        want = map[old_st];
        if (want != emitted) { ++need; emitted = want; }
    }
    if (doclen + need + 1 > MAXDOC) { message(S_FULL, 0, 0, 0); return; }
    /* selection et suite en haut du tampon, puis reecriture en un passage */
    rd = &text[MAXDOC - (doclen - a)];
    memmove(rd, &text[a], doclen - a);
    end = rd + (b - a);
    q = &text[a];
    emitted = old_st = start_st;
    for (; rd < end; ++rd) {
        c = *rd;
        if (IS_MARK(c)) { old_st = c & 7; continue; }
        want = map[old_st];
        if (want != emitted) { *q++ = MARK(want); emitted = want; }
        *q++ = c;
    }
    sel_end = (unsigned)(q - text);
    if (emitted != old_st && end < &text[MAXDOC] && !IS_MARK(*end)) *q++ = MARK(old_st);
    w = (unsigned)(q - text);
    memmove(q, end, (unsigned)(&text[MAXDOC] - end));
    doclen = w + (unsigned)(&text[MAXDOC] - end);
    dirty = 1;
    anchor = a;
    cursor = sel_end;
    first = relayout(a, b, w, &all);
    if (!all && relayout_last < line_of(sel_end)) relayout_last = line_of(sel_end);
    refresh_after(first, all);
}

static unsigned char lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Recherche sans tenir compte des majuscules ni des marqueurs de style. */
static void find_next(void)
{
    unsigned char len = (unsigned char)strlen((char *)findtext), k;
    unsigned start = has_selection() ? sel_hi() : cursor, p, q, tries;
    if (!len) return;
    p = start;
    for (tries = 0; tries <= doclen; ++tries, ++p) {
        if (p >= doclen) p = 0;
        if (IS_MARK(text[p])) continue;
        for (k = 0, q = p; k < len && q < doclen; ++q) {
            if (IS_MARK(text[q])) continue;
            if (lower(text[q]) != lower(findtext[k])) break;
            ++k;
        }
        if (k == len) {
            set_selection(p, q);
            return;
        }
    }
    message(S_FIND, S_NOTFOUND, 0, 0);
}

static void find_dialog(void)
{
    if (ask_text(S_FIND, S_FINDWHAT, (char *)findtext, 22, 0)) find_next();
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */

static unsigned char run_menu(unsigned char m)
{
    unsigned char n = 0, i, hover = 255, row, choice = 255;
    unsigned x0 = menu_x[m], width = 0, t;
    while (menu_items[m][n]) {
        t = text_width(menu_items[m][n], 0) + text_width(menu_keys[m][n], 0) + 16;
        if (t > width) width = t;
        ++n;
    }
    width = (width + 31) & 0xFFF8u;
    if (x0 + width + 8 > 320) x0 = 312 - width;
    xor_span(0, menu_x[m], menu_x[m + 1]);
    panel(1, n, x0, width);
    for (i = 0; i < n; ++i) {
        row = 1 + i;
        put_text(x0 + 12, row, menu_items[m][i], 0);
        put_text(x0 + width - 8 - text_width(menu_keys[m][i], 0), row, menu_keys[m][i], 0);
        if (m == 2 && ((i && (style & (1 << (i - 1)))) || (!i && !style)) && !has_selection()) {
            put_text(x0 + 4, row, "*", 0);
        }
    }
    for (;;) {
        poll_mouse();
        key = read_key();
        if (key == 3) break;
        row = mouse_y / 8;
        i = (row >= 1 && row <= n && mouse_x >= x0 && mouse_x < x0 + width) ? row - 1 : 255;
        if (i != hover) {
            if (hover != 255) xor_span(hover + 1, x0 + 2, x0 + width - 2);
            if (i != 255) xor_span(i + 1, x0 + 2, x0 + width - 2);
            hover = i;
        }
        if (clicked) { choice = i; break; }
    }
    /* ne redessine que ce que le menu cachait */
    shown_dirty = 255;
    page_colors();
    draw_menubar();
    draw_status();
    draw_lines(top, top + n);
    place_cursor();
    return choice;
}

static void do_menu(unsigned char m, unsigned char item)
{
    if (item == 255) return;
    switch (m) {
    case 0:
        if (item == 0 && (!dirty || message(S_NEW, S_LOSE, 0, 1))) { new_doc(); redraw_all(); }
        if (item == 1) open_doc();
        if (item == 2) save_doc();
        if (item == 3 && ask_filename()) save_doc();
        break;
    case 1:
        if (item == 0) cut_selection();
        if (item == 1) copy_selection();
        if (item == 2) paste();
        if (item == 3) set_selection(0, doclen);
        if (item == 4) find_dialog();
        if (item == 5) find_next();
        break;
    case 2:
        apply_style(item ? (unsigned char)(1 << (item - 1)) : 0);
        break;
    case 3:
        print_doc(item);
        break;
    case 4:
        message(S_ABOUT, S_ABOUT1, S_ABOUT3, 0);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Clavier et souris dans le texte                                     */

static unsigned pos_at(unsigned i, unsigned x)
{
    unsigned p = lstart[i], end = lstart[i + 1], px = 0;
    unsigned char w, st = lstyle[i], c;
    if (i + 1 >= nlines) end = doclen;
    while (p < end) {
        c = text[p];
        if (IS_MARK(c)) { st = c & 7; ++p; continue; }
        if (c == CR) break;
        w = CW(c, st);
        if (px + w / 2 >= x) break;
        px += w;
        ++p;
    }
    if (p == end && end < doclen && p > lstart[i] && text[p - 1] == ' ') --p;
    return canonical(p);
}

static unsigned pos_at_mouse(void)
{
    unsigned i = top + (mouse_y / 8 >= TEXT_ROW0 ? mouse_y / 8 - TEXT_ROW0 : 0);
    if (i >= nlines) i = nlines - 1;
    return pos_at(i, mouse_x >= TEXT_X0 ? mouse_x - TEXT_X0 : 0);
}

static void move_vertical(int d)
{
    unsigned i = line_of(cursor), x = cursor_x();
    if (d < 0 && i == 0) return;
    if (d > 0 && i + 1 >= nlines) return;
    cursor = pos_at(i + d, x);
}

static void move_right(void)
{
    unsigned p = cursor;
    while (p < doclen && IS_MARK(text[p])) ++p;
    if (p < doclen) ++p;
    cursor = p;
}

static void move_left(void)
{
    if (cursor) cursor = canonical(cursor - 1);
}

/* Accents sous C=. Edition hongroise : les positions de AltGr dans hu_prog
   (q=ő w=ű e=é y=ü u=ú i=í o=ó p=ö a=á), sinon le clavier français. */
#ifdef KBD_HU
/* Le KERNAL ne sait pas faire Shift + C= : avec les deux touches, sa table
   ne rend rien du tout. Plume lit donc la matrice lui-meme tant que C= est
   enfoncee. Deux octets par touche : ce qu'il faut ecrire dans $DC00 pour ne garder
   que sa rangee, puis son masque de colonne. L'ordre fixe le numero rendu par
   hu_key, d'ou la table des lettres juste apres. */
static const unsigned char hu_keys[] = {
    0xFD, 0x40,     /* e */
    0xFD, 0x02,     /* w */
    0xEF, 0x02,     /* i */
    0xEF, 0x40,     /* o */
    0xDF, 0x02,     /* p */
    0x7F, 0x40,     /* q */
    0xF7, 0x40,     /* u */
    0xF7, 0x02,     /* y */
    0xFD, 0x04,     /* a */
    0xDF, 0x08      /* - (donne _) */
};
#define HU_KEYS (sizeof hu_keys / 2)

/* Numeros de hu_key (0xE0 + numero, puis + HU_KEYS si Shift), puis les lettres
   de la police. Aucun de ces codes ne tombe sur ceux que handle_key lit avant
   translate, ni sur les lettres de la police, dont 0x85-0x8C sont aussi les
   touches de fonction (0x8B = couper, 0x85 = enregistrer...). */
static const unsigned char hu_font[] = {
    132, 178, 170, 172, 139, 176, 174, 142, 168, '_',   /* e u i o o o u u a _ */
    146, 179, 171, 173, 180, 177, 175, 155, 169, '_'    /* E U I O O O U U A _ */
};

static unsigned char hu_last = 0xFF;    /* offset deja vu ; 0xFF = C= relachee */

/* Touche de la couche C= enfoncee, sous forme 0xE0 + son numero (+ HU_KEYS si Shift),
   ou 0 si C= est relachee ou si la touche a deja ete comptee. $028D dirait
   l'etat de C=, mais le KERNAL le remet a zero pendant sa propre scrutation :
   on lit la matrice (rangee 7, bit 5). */
static unsigned char hu_key(void)
{
    unsigned char i, c, d, shift;
    asm("sei");
    POKE(0xDC00, 0x7F);                 /* rangee 7 : C=, et q */
    if (PEEK(0xDC01) & 0x20) {          /* C= relachee */
        asm("cli");
        hu_last = 0xFF;
        return 0;
    }
    POKE(0xDC00, 0xFD);                 /* rangee 1 : Shift gauche, bit 7 */
    c = ~PEEK(0xDC01);
    POKE(0xDC00, 0xBF);                 /* rangee 6 : Shift droit, bit 4 */
    d = ~PEEK(0xDC01);
    shift = ((c & 0x80) | (d & 0x10)) ? HU_KEYS : 0;
    for (i = 0; i < 2 * HU_KEYS; i += 2) {
        POKE(0xDC00, hu_keys[i]);
        if (!(~PEEK(0xDC01) & hu_keys[i + 1])) continue;
        POKE(0xDC00, 0x7F);
        asm("cli");
        if (i == hu_last) return 0;
        hu_last = i;
        return 0xE0 + (i >> 1) + shift;
    }
    hu_last = 0xFE;                     /* C= tenue, aucune touche de la couche */
    asm("cli");
    return 0;
}
#else
static const unsigned char cbm_keys[]   = { 0xB1, 0xB2, 0xA3, 0xB0, 0xAB, 0xBC, 0xB8, 0xB7, 0xB9, 0xA2, 0xA1, 0xB6, 0xAF };
static const unsigned char cbm_accent[] = { 132,  133,  134,  128,  129,  131,  140,  141,  138,  136,  137,  135,  156 };
#endif
static unsigned char translate(unsigned char k)
{
#ifndef KBD_HU
    unsigned char i;
#endif
#ifdef KBD_HU
    /* Lettres rendues par hu_key (voir hu_font). */
    if (k >= 0xE0 && k < 0xE0 + 2 * HU_KEYS) return hu_font[k - 0xE0];
#endif
#ifdef KBD_DK
    /* Clavier danois : : ; @ donnent æ ø å, Shift les majuscules ;
       Shift + . et Shift + , donnent : et ; ; C= + @ donne @. */
    switch (k) {
    case 0x3A: return 162;
    case 0x3B: return 163;
    case 0x40: return 164;
    case 0x5B: return 165;
    case 0x5D: return 166;
    case 0xBA: return 167;
    case 0x3E: return ':';
    case 0x3C: return ';';
    case 0xA4: return '@';
    }
#endif
    if (k >= 0x41 && k <= 0x5A) return k + 0x20;
    if (k >= 0xC1 && k <= 0xDA) return k - 0x80;
    if (k >= 0x20 && k <= 0x40) return k;
    if (k == 0x5B || k == 0x5D) return k;
    if (k == 0x5C) return 161;
    if (k == 0xDC) return '_';              /* C= + - : tiret bas */
    if (k == 0x5E) return '^';
    if (k == 0xA0) return ' ';
#ifndef KBD_HU
    for (i = 0; i < sizeof cbm_keys; ++i) if (k == cbm_keys[i]) return cbm_accent[i];
#endif
    return 0;
}

/* Apres un deplacement, le style de frappe suit le texte. */
static void cursor_moved(void)
{
    style = style_before(cursor);
    place_cursor();
    draw_status();
}

static void handle_key(unsigned char k)
{
    unsigned char c;
    switch (k) {
    case CR:   insert_char(CR); return;
    case 0x14: delete_before(); return;
    case 0x1D: clear_selection(); move_right(); break;
    case 0x9D: clear_selection(); move_left(); break;
    case 0x11: clear_selection(); move_vertical(1); break;
    case 0x91: clear_selection(); move_vertical(-1); break;
    case 0x13: clear_selection(); cursor = lstart[line_of(cursor)]; break;
    case 3:    clear_selection(); return;
    case 0x85: save_doc(); return;
    case 0x86: apply_style(BOLD); return;
    case 0x87: apply_style(ITALIC); return;
    case 0x88: apply_style(UNDERLINE); return;
    case 0x89: copy_selection(); return;
    case 0x8A: paste(); return;
    case 0x8B: cut_selection(); return;
    case 0x8C: find_next(); return;
    default:
        c = translate(k);
        if (c) insert_char(c);
        return;
    }
    cursor_moved();
}

static void handle_click(void)
{
    unsigned char row = mouse_y / 8, m, k;
    unsigned p;
    if (row == 0) {
        for (m = 0; m < NMENUS; ++m) {
            if (mouse_x >= menu_x[m] && mouse_x < menu_x[m + 1]) { do_menu(m, run_menu(m)); return; }
        }
        return;
    }
    if (row == 1) {
        for (k = 0; k < 3; ++k) {
            if (mouse_x >= STYLE_X + k * 18 && mouse_x < STYLE_X + k * 18 + 16) apply_style((unsigned char)(1 << k));
        }
        return;
    }
    p = pos_at_mouse();
    if (extend) {
        set_selection(anchor == NOSEL ? cursor : anchor, p);
        return;
    }
    if (anchor != NOSEL) set_selection(NOSEL, p);
    else cursor = p;
    cursor_moved();
    anchor = p;
    while (held) {
        poll_mouse();
        p = pos_at_mouse();
        if (p != cursor) set_selection(anchor, p);
    }
    if (anchor == cursor) anchor = NOSEL;
}

/* ------------------------------------------------------------------ */
/* Sprites : fleche (sprite 0) et curseur texte (sprite 1)             */

static const unsigned char arrow[] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xF8, 0xD8, 0x8C, 0x0C, 0x06, 0x06
};

static void sprites_init(void)
{
    unsigned char i, *spr = (unsigned char *)0xC000;
    memset(spr, 0, 128);
    for (i = 0; i < sizeof arrow; ++i) spr[i * 3] = arrow[i];
    for (i = 0; i < 8; ++i) spr[64 + i * 3] = 0xC0;
    *(unsigned char *)0xCFF8 = 0;
    *(unsigned char *)0xCFF9 = 1;
    VIC.spr0_color = COLOR_BLACK;
    VIC.spr1_color = COLOR_BLUE;
    VIC.spr_ena = 0x03;
}

void main(void)
{
    POKE(0x0291, 0x80);
    findtext[0] = 0;
#ifdef KBD_HU
    hu_last = 0xFF;                     /* aucune touche de la couche vue */
#endif                            /* tampon cassette : pas remis a zero */
    gfx_init();
    VIC.bordercolor = COLOR_GRAY1;
    sprites_init();
    mouse_install(&mouse_def_callbacks, mouse_static_stddrv);
    mouse_show();
    new_doc();
    redraw_all();
    /* repere pour les outils de test : adresses du texte et de sa longueur */
    *(unsigned *)0x02AB = (unsigned)text;
    *(unsigned *)0x02AD = (unsigned)&doclen;
    for (;;) {
        poll_mouse();
        if (clicked) handle_click();
        key = read_key();
        if (key) handle_key(key);
        blink_cursor();
    }
}
