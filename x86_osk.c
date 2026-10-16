/*
 * On-screen keyboard, and the mouse UI shell behind it. Interface and the
 * port's provenance: x86_osk.h. Design record: docs/input/03-onscreen-keyboard.md.
 *
 * THREE TOP-LEVEL MODES, ONE BUTTON. GAME cycles
 *
 *     OFF --GAME--> KBD --GAME--> MOUSE --GAME--> OFF
 *
 * and osk_set_mode() is the ONLY place a transition happens, so everything that
 * must be true on a change -- held modifiers dropped, the stale button image
 * discarded, the mouse's held click released -- lives in one function.
 *
 * GAME AND NOT THE ZELDA "START" BUTTON, deliberately. docs/input/02 records the
 * decision: PAUSE/SET belongs to retro-go's overlay, so GAME is the only spare
 * button, and it is spare on EVERY unit. Putting the only door to the keyboard
 * on ODROID_INPUT_X would make the whole feature unreachable on a mario unit --
 * which is precisely the unit with no buttons to spare. src/main.c's
 * pad_scancode[] therefore leaves the GAME slot at 0.
 *
 * ---------------------------------------------------------------------------
 * TWO THINGS THE PREVIOUS PORT NEEDED THAT WE DO NOT, and why leaving them out
 * is a fact about our renderer rather than a corner cut:
 *
 *  1. NO REPAINT COUNTER. Theirs painted on a counter seeded to 2, because the
 *     LCD is double buffered by SWAP and their video layer never rewrote the
 *     letterbox bars -- so a bar drawn once flickered at the swap rate.
 *     x86_video_render() memsets both 20-row bands and repaints all 200 picture
 *     rows on EVERY frame (x86_video.c), into whichever buffer is active. So
 *     the OSK must be redrawn every frame too, and both buffers converge by
 *     construction. A dirty-count here would draw the keyboard into one buffer
 *     and let the other come back blank.
 *
 *  2. NO BDA VIDEO-MODE WATCH. Theirs re-armed its paint when 0040:0049 moved,
 *     because a guest mode change wiped both framebuffers behind its back. Ours
 *     cannot be surprised that way for the same reason as (1).
 *
 * THE GUEST'S FRAMEBUFFER IS NEVER TOUCHED. This draws into the LUT8 panel
 * buffer after the blit, never into CGA VRAM, so nothing the guest can read
 * changes -- in either text or graphics mode.
 *
 * COLOURS: see ui_map_refresh(). CGA colour n is a FIXED entry in every
 * 32-entry-CLUT mode (x86_video_ui_colour) and the nearest palette entry in a
 * 256-colour one -- never one the guest can repaint under us.
 */

#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_osk.h"
#include "x86_mouse.h"

/* ---------------------------------------------------------------------------
 * Layout
 *
 * FOUR QWERTY ROWS SPLIT ROUND THE PICTURE: rows 0-1 in the 20-row band ABOVE
 * the game, rows 2-3 in the band BELOW, so the game stays where it always is
 * (rows 20..219) and nothing is scaled or moved. The look -- blue key caps,
 * yellow cursor, red latched modifiers -- is the gw-dos (XTulator) frontend's,
 * which the owner asked for by name. It replaces the earlier three-layer,
 * two-row alphabetical grid: with four rows every key is on screen at once,
 * so there are no layers to page through and no hidden '`'.
 *
 * A key cap is (label chars x 8) + 4 px wide -- 2 px of cap either side of the
 * glyphs -- with the last column of each cap left black as the gap. A row must
 * total <= 320 px; draw_glyph() CLIPS SILENTLY, so x86_osk_reset() measures
 * every row and publishes x86_osk_row_overflow rather than trusting the sums
 * in the comments below.
 * ------------------------------------------------------------------------- */

enum {
    A_KEY = 0,      /* send the scancode */
    A_SHIFT,        /* latch/unlatch sticky Shift */
    A_CTRL,
    A_ALT,
    A_CLOSE,        /* straight to OFF, short-cutting mouse mode */
    A_KEY_SHIFT,    /* the scancode with Shift held for this one key (':') */
};

typedef struct {
    char    label[4];   /* 1..3 chars, NUL terminated */
    uint8_t sc;         /* XT scancode; 0 for action cells */
    uint8_t flags;      /* action */
} osk_key_t;

#define KC(c, sc)      { { (c), 0, 0, 0 }, (sc), A_KEY }
#define KS(lbl, sc)    { lbl, (sc), A_KEY }
#define KA(lbl, act)   { lbl, 0, (act) }

/* The SAME LAYOUT AS THE XTulator CORE'S (dos.bin) on-screen keyboard, so a
 * player moving between the two cores finds every key where it was: mixed-case
 * labels, capital letters, the arrows as glyphs after Spc, and a ':' cap of its
 * own (Shift+';' in one press -- typing "a:" without hunting for the sticky
 * Shift). That core's 80c / CPU / A: caps have no counterpart here; PgU / PgD and
 * the green Off cap take their place.
 * The arrows are the EXTENDED scancodes a real BIOS delivers with ASCII zero
 * (0x48/0x50/0x4B/0x4D), which is what a menu-driven program reads. Shifted
 * symbols come from the sticky Shf cap (or TIME), exactly as on a keyboard.
 * Labels are drawn through display_char(), which only ever upper-cases a-z, so
 * capital letters and the CP437 arrow glyphs (0x18/0x19/0x1B/0x1A, all in
 * font8x8.h) draw as written. */
static const osk_key_t osk_r0[] = {            /* 28+144+20+120 = 312 px */
    KS("Esc", 0x01),
    KC('1', 0x02), KC('2', 0x03), KC('3', 0x04), KC('4', 0x05), KC('5', 0x06),
    KC('6', 0x07), KC('7', 0x08), KC('8', 0x09), KC('9', 0x0A), KC('0', 0x0B),
    KC('-', 0x0C), KC('=', 0x0D), KS("\x1B-", 0x0E),
    KS("F1", 0x3B), KS("F2", 0x3C), KS("F3", 0x3D), KS("F4", 0x3E),
    KS("F5", 0x3F), KS("F6", 0x40),
};
static const osk_key_t osk_r1[] = {            /* 28+144+28+60+28+28 = 316 */
    KS("Tab", 0x0F),
    KC('Q', 0x10), KC('W', 0x11), KC('E', 0x12), KC('R', 0x13), KC('T', 0x14),
    KC('Y', 0x15), KC('U', 0x16), KC('I', 0x17), KC('O', 0x18), KC('P', 0x19),
    KC('[', 0x1A), KC(']', 0x1B), KS("Ent", 0x1C),
    KS("F7", 0x41), KS("F8", 0x42), KS("F9", 0x43), KS("F10", 0x44),
    KS("Del", 0x53),
};
static const osk_key_t osk_r2[] = {            /* 28+168+28+28+28+28 = 308 */
    KA("Ctl", A_CTRL),
    KC('A', 0x1E), KC('S', 0x1F), KC('D', 0x20), KC('F', 0x21), KC('G', 0x22),
    KC('H', 0x23), KC('J', 0x24), KC('K', 0x25), KC('L', 0x26),
    KC(';', 0x27), { { ':', 0, 0, 0 }, 0x27, A_KEY_SHIFT },
    KC('\'', 0x28), KC('\\', 0x2B), KC('`', 0x29),
    KS("Ins", 0x52), KS("PgU", 0x49), KS("PgD", 0x51),
    KA("Off", A_CLOSE),
};
static const osk_key_t osk_r3[] = {            /* 28+120+28+28+48+28+28 = 308 */
    KA("Shf", A_SHIFT),
    KC('Z', 0x2C), KC('X', 0x2D), KC('C', 0x2E), KC('V', 0x2F), KC('B', 0x30),
    KC('N', 0x31), KC('M', 0x32), KC(',', 0x33), KC('.', 0x34), KC('/', 0x35),
    KA("Alt", A_ALT), KS("Spc", 0x39),
    KC('\x18', 0x48), KC('\x19', 0x50), KC('\x1B', 0x4B), KC('\x1A', 0x4D),
    KS("Hom", 0x47), KS("End", 0x4F),
};

typedef struct {
    const osk_key_t *keys;
    uint8_t          n;
} osk_row_t;

#define ROW(a) { (a), (uint8_t)(sizeof(a) / sizeof((a)[0])) }

#define OSK_ROWS   4

static const osk_row_t osk_layout[OSK_ROWS] = {
    ROW(osk_r0), ROW(osk_r1), ROW(osk_r2), ROW(osk_r3),
};

/* Cap width in px, gap column included. */
static int key_w(const osk_key_t *k) { return (int)strlen(k->label) * 8 + 4; }

static int key_x(uint8_t r, uint8_t c)
{
    int x = 0;
    for (uint8_t i = 0; i < c; i++)
        x += key_w(&osk_layout[r].keys[i]);
    return x;
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

enum {
    MODE_OFF = 0,   /* buttons belong to the guest (src/main.c's table) */
    MODE_KBD,       /* the key grid owns the d-pad */
    MODE_MOUSE,     /* the pointer owns the d-pad */
    MODE_COUNT,
};

#define OSK_ECHO_MAX 38

volatile uint32_t x86_osk_mode = MODE_OFF;
volatile uint32_t x86_osk_presses = 0;
volatile uint32_t x86_osk_codes = 0;
volatile uint32_t x86_osk_last_key = 0;
/* Widest row measured at reset, in PIXELS, and 1 if any row exceeds 320. A row
   that overflows loses its rightmost keys to draw_glyph()'s silent clip, so the
   check is published rather than trusted -- tests/test_osk.py asserts it. */
volatile uint32_t x86_osk_row_px_max = 0;
volatile uint32_t x86_osk_row_overflow = 0;

static uint8_t  osk_row;
static uint8_t  osk_col;
static uint8_t  osk_sticky;          /* STICKY_* below */
static uint32_t osk_prev;            /* edge detection for navigation */

/* Typematic repeat, all four directions (UP/DOWN step rows).
 *
 * The grid is edge-driven on purpose -- a key must not fire twice for one
 * press. Holding a direction to scan along a row is a different gesture, so it
 * gets its own level-driven path: the edge does the first step, then nothing
 * for OSK_REPEAT_DELAY_MS, then a step every OSK_REPEAT_RATE_MS.
 *
 * Time comes from the host via x86_osk_set_now_ms(). If a caller never sets it
 * -- the bare-metal harness does not -- now stays 0, the delay never elapses,
 * and the behaviour is exactly the old edge-only one. That is deliberate: the
 * existing tests keep passing unchanged and still assert no repeat. */
#define OSK_REPEAT_DELAY_MS 400u
#define OSK_REPEAT_RATE_MS   90u
static uint32_t osk_now_ms;          /* host clock, 0 if the caller has none */
static uint32_t osk_hold_dir;        /* the direction bits currently held */
static uint32_t osk_repeat_at;       /* when the next repeat step is due */
static char     osk_echo[OSK_ECHO_MAX + 1];
static uint8_t  osk_echo_len;

#define STICKY_SHIFT 1u
#define STICKY_CTRL  2u
#define STICKY_ALT   4u

#define OSK_TOAST_FRAMES 90
#define OSK_TOAST_MAX    40
static char     osk_toast[OSK_TOAST_MAX + 1];
static uint16_t osk_toast_frames;

/* Panel geometry. 320x200 inside a 320x240 panel leaves a 20-row bar top and
 * bottom (x86_video.c), and an 8x8 glyph row plus padding fits twice in each. */
#define OSK_CELL      8
#define OSK_BAR       20
#define OSK_PIC_ROWS  200
#define OSK_TOP_Y0    2                          /* rows   2..9   */
#define OSK_TOP_Y1    11                         /* rows  11..18  */
#define OSK_BOT_Y0    (OSK_BAR + OSK_PIC_ROWS + 1)   /* 221..228 */
#define OSK_BOT_Y1    (OSK_BAR + OSK_PIC_ROWS + 10)  /* 230..237 */

/* UI COLOURS ARE CGA PALETTE NUMBERS, mapped to a panel index per frame by
 * ui_map_refresh(). In every mode with a 32-entry CLUT -- text, CGA, and EGA
 * x86_video_ui_colour() says where the fixed CGA colours are (0-15 in text
 * and CGA, 16-31 in EGA), whatever the guest has done to its own palette. A 256-colour mode owns the whole table, so there the nearest
 * entry of the guest's palette is used instead: a blue key stays the bluest
 * thing the game's palette has. */
enum { U_BLACK = 0, U_BLUE = 1, U_GREEN = 2, U_RED = 4, U_GREY = 8,
       U_YELLOW = 14, U_WHITE = 15 };
static const uint32_t ui_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
static uint8_t ui_map[16];

static void ui_map_refresh(void)
{
    const uint32_t *pal = (x86_video_ui_colour(0) < 0) ? x86_video_clut256() : 0;
    for (unsigned n = 0; n < 16; n++) {
        if (!pal) {
            int idx = x86_video_ui_colour(n);
            ui_map[n] = (uint8_t)(idx < 0 ? 0 : idx);
            continue;
        }
        if (n != U_BLACK && n != U_BLUE && n != U_GREEN && n != U_RED
            && n != U_GREY && n != U_YELLOW && n != U_WHITE)
            continue;                   /* 7 x 256 compares, not 16 x 256 */
        uint32_t best = 0xFFFFFFFFu;
        int r = (int)(ui_rgb[n] >> 16), g = (int)((ui_rgb[n] >> 8) & 0xFF), b = (int)(ui_rgb[n] & 0xFF);
        for (unsigned i = 0; i < 256; i++) {
            int dr = (int)((pal[i] >> 16) & 0xFF) - r;
            int dg = (int)((pal[i] >> 8) & 0xFF) - g;
            int db = (int)(pal[i] & 0xFF) - b;
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < best) { best = d; ui_map[n] = (uint8_t)i; }
        }
    }
}

#define C_BLACK  ui_map[U_BLACK]
#define C_DIM    ui_map[U_GREY]
#define C_WHITE  ui_map[U_WHITE]

/* Mouse UI state. See the block above x86_mouse_input() for what is and is not
   real about it. */
#define MOUSE_VW  640
#define MOUSE_VH  200
#define MOUSE_FINE_MICKEYS 1
#define MOUSE_FINE_FRAMES  10
#define MOUSE_RAMP_DIV     2
#define MOUSE_MAX_MICKEYS  24
static int16_t  mouse_x, mouse_y;
static uint8_t  mouse_render = 1;
static uint8_t  mouse_buttons;
static uint16_t mouse_held[4];
volatile uint32_t x86_mouse_clicks = 0;   /* button edges the UI produced */

/* ---------------------------------------------------------------------------
 * Mode transitions -- the only place the mode changes
 * ------------------------------------------------------------------------- */

static void osk_set_mode(uint32_t m)
{
    if (m == x86_osk_mode)
        return;
    x86_osk_mode = m;

    if (m != MODE_MOUSE && mouse_buttons) {
        mouse_buttons = 0;      /* a click let go by a mode switch, not by the
                                   user, would otherwise stay down forever */
        x86_mouse_buttons(0);
    }
    memset(mouse_held, 0, sizeof mouse_held);

    if (m != MODE_OFF) {
        /* The GAME press that caused this is still physically held on the frame
           the new mode first runs. ~0 means "everything was already down", so
           the first frame in a mode sees no edges at all. */
        osk_prev = ~0u;
        /* A latched modifier must not survive: it is invisible from outside the
           grid, so it would fire on whatever key came next. */
        osk_sticky = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */

static void draw_glyph(uint8_t *fb, uint32_t stride, int x, int y,
                       uint8_t ch, uint8_t fg, uint8_t bg)
{
    const unsigned char *g = x86_font8x8()[ch];

    if (x < 0 || (uint32_t)(x + OSK_CELL) > stride)
        return;

    /* BIT 0 IS THE LEFTMOST PIXEL. This tree's fonts are Hepper's bit order,
       not the hardware's -- scripts/make_font.py says so and text_render()'s
       text_mask4[] depends on it. Drawing with 0x80 >> c instead compiles,
       renders, and produces a keyboard whose every glyph is mirrored: at 8 px
       that reads as a slightly odd typeface rather than as a bug, which is
       exactly how it survived the first capture. */
    for (int r = 0; r < 8; r++) {
        uint8_t  bits = g[r];
        uint8_t *p    = fb + (uint32_t)(y + r) * stride + x;
        for (int c = 0; c < 8; c++)
            p[c] = (bits & (1u << c)) ? fg : bg;
    }
}

static int draw_text(uint8_t *fb, uint32_t stride, int x, int y,
                     const char *s, uint8_t fg, uint8_t bg)
{
    for (; *s; s++, x += OSK_CELL)
        draw_glyph(fb, stride, x, y, (uint8_t)*s, fg, bg);
    return x;
}

/* A latched Shift uppercases the letter caps, which is the only place the
   latch is visible where the user is actually looking (the cap goes red too). */
static char display_char(char c)
{
    if ((osk_sticky & STICKY_SHIFT) && c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

static int key_latched(const osk_key_t *k)
{
    switch (k->flags) {
    case A_SHIFT: return (osk_sticky & STICKY_SHIFT) != 0;
    case A_CTRL:  return (osk_sticky & STICKY_CTRL) != 0;
    case A_ALT:   return (osk_sticky & STICKY_ALT) != 0;
    default:      return 0;
    }
}

static void draw_row(uint8_t *fb, uint32_t stride, int y, uint8_t r)
{
    const osk_row_t *row = &osk_layout[r];
    int x = 0;

    for (int rr = 0; rr < 8; rr++)
        memset(fb + (uint32_t)(y + rr) * stride, C_BLACK, stride);

    for (uint8_t i = 0; i < row->n; i++) {
        const osk_key_t *k = &row->keys[i];
        int w = key_w(k);
        uint8_t bg = U_BLUE, fg = U_WHITE;
        if (key_latched(k))
            bg = U_RED;
        else if (k->flags == A_CLOSE)
            bg = U_GREEN;
        if (r == osk_row && i == osk_col) {
            bg = U_YELLOW;
            fg = U_BLACK;
        }
        if ((uint32_t)(x + w - 1) <= stride)
            for (int rr = 0; rr < 8; rr++)
                memset(fb + (uint32_t)(y + rr) * stride + x, ui_map[bg], (size_t)(w - 1));
        int gx = x + 2;
        for (const char *c = k->label; *c; c++, gx += OSK_CELL)
            draw_glyph(fb, stride, gx, y, (uint8_t)display_char(*c), ui_map[fg], ui_map[bg]);
        x += w;
    }
}

/* Three digits, right-aligned, without pulling in snprintf: the core's RAM_EMU
   headroom is measured in hundreds of bytes (TOOLING.md) and stdio's formatter
   is kilobytes. */
static int put_num(char *out, int v)
{
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    out[0] = (char)('0' + v / 100);
    out[1] = (char)('0' + (v / 10) % 10);
    out[2] = (char)('0' + v % 10);
    return 3;
}

static void draw_status(uint8_t *fb, uint32_t stride)
{
    int x;

    memset(fb, C_BLACK, OSK_BAR * stride);

    if (x86_osk_mode == MODE_MOUSE) {
        /* Mouse mode replaces the whole top bar rather than sharing it: the
           position readout changes every frame and interleaving it with the
           modifier indicators made both unreadable. */
        char st[20];
        int n = 0;
        memcpy(st + n, "MSE ", 4); n += 4;
        n += put_num(st + n, mouse_x);
        st[n++] = ',';
        n += put_num(st + n, mouse_y);
        memcpy(st + n, " CUR", 4); n += 4;
        st[n++] = mouse_render ? '+' : '-';
        st[n] = 0;
        draw_text(fb, stride, 0, OSK_TOP_Y0, st, C_WHITE, C_BLACK);
        /* GAME must be named here: it is the only control that exists on every
           unit and the only way back to the game. */
        draw_text(fb, stride, 0, OSK_TOP_Y1,
                  "dpad move B/A click TIME kbd GAME off", C_DIM, C_BLACK);
        return;
    }

    /* Keyboard mode: the top band is the first two key rows (x86_osk_draw). */
    (void)x;
}

/* An 8x8 arrow. 1 = body; the outline is drawn by testing the neighbours, so
 * the pointer stays visible over both black and white guest content without a
 * second bitmap. */
static const uint8_t mouse_arrow[8] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0x90, 0x10,
};

static void mouse_draw(uint8_t *fb, uint32_t stride)
{
    /* NOTHING TO UNDRAW, AND THAT IS A DEPENDENCY, NOT LUCK: x86_video_render()
       repaints all 200 picture rows every frame, so the pointer of frame N-1 is
       gone before this runs. If anyone adds dirty-rectangle tracking to the
       renderer, this leaves a trail of arrows and the fix is an 8x8 save-under,
       not a repaint here. */
    int px, py;
    if (x86_mouse_in_use()) {
        /* A guest found the driver: ITS position is the pointer, and the arrow
           is painted only while the guest has the driver's cursor shown --
           a game drawing its own cursor (every SCUMM title) hides it, and two
           arrows would be one too many. */
        if (!x86_mouse_cursor(&px, &py))
            return;
        py = py * OSK_PIC_ROWS / 200 + OSK_BAR;
    } else {
        if (!mouse_render || x86_osk_mode != MODE_MOUSE)
            return;
        px = mouse_x * 320 / MOUSE_VW;
        py = mouse_y * OSK_PIC_ROWS / MOUSE_VH + OSK_BAR;
    }

    for (int r = 0; r < 8; r++) {
        int y = py + r;
        if (y < OSK_BAR || y >= OSK_BAR + OSK_PIC_ROWS)
            continue;                       /* never write the OSK's bars */
        for (int c = 0; c < 8; c++) {
            int x = px + c;
            uint8_t bit = (uint8_t)(0x80u >> c);
            if (x < 0 || (uint32_t)x >= stride)
                continue;
            if (mouse_arrow[r] & bit) {
                fb[(uint32_t)y * stride + x] = C_WHITE;
            } else {
                uint8_t up   = (r > 0) ? mouse_arrow[r - 1] : 0;
                uint8_t down = (r < 7) ? mouse_arrow[r + 1] : 0;
                if ((mouse_arrow[r] & (uint8_t)(bit << 1)) ||
                    (mouse_arrow[r] & (uint8_t)(bit >> 1)) ||
                    (up & bit) || (down & bit))
                    fb[(uint32_t)y * stride + x] = C_BLACK;
            }
        }
    }
}

void x86_osk_toast(const char *s)
{
    unsigned i;
    for (i = 0; i < OSK_TOAST_MAX && s[i]; i++)
        osk_toast[i] = s[i];
    osk_toast[i] = 0;
    osk_toast_frames = OSK_TOAST_FRAMES;
}

void x86_osk_draw(uint8_t *fb, uint32_t stride)
{
    if (!fb)
        return;

    if (x86_osk_mode == MODE_OFF) {
        /* the 256-entry search only when something will actually be drawn */
        if (osk_toast_frames || x86_mouse_in_use())
            ui_map_refresh();
        if (osk_toast_frames) {
            /* One 8-row band, cleared first so the text is legible against
               whatever the guest drew underneath. */
            memset(fb + OSK_TOP_Y0 * stride, C_BLACK, 8 * stride);
            draw_text(fb, stride, 0, OSK_TOP_Y0, osk_toast, C_WHITE, C_BLACK);
            osk_toast_frames--;
        }
        /* `mouse = on` with the keyboard closed: the driver's cursor, if a
           guest has it shown (mouse_draw decides; nothing otherwise) */
        mouse_draw(fb, stride);
        return;                 /* the bars are the renderer's again */
    }

    ui_map_refresh();

    if (x86_osk_mode == MODE_MOUSE) {
        mouse_draw(fb, stride);
        draw_status(fb, stride);
        /* The grid is not reachable in mouse mode, so drawing it would offer
           keys the buttons cannot press. */
        memset(fb + (OSK_BAR + OSK_PIC_ROWS) * stride, C_BLACK, OSK_BAR * stride);
        return;
    }

    /* Two key rows in each band, the picture untouched between them. */
    memset(fb, C_BLACK, OSK_BAR * stride);
    memset(fb + (OSK_BAR + OSK_PIC_ROWS) * stride, C_BLACK, OSK_BAR * stride);
    draw_row(fb, stride, OSK_TOP_Y0, 0);
    draw_row(fb, stride, OSK_TOP_Y1, 1);
    draw_row(fb, stride, OSK_BOT_Y0, 2);
    draw_row(fb, stride, OSK_BOT_Y1, 3);
}

/* ---------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------- */

static void osk_echo_push(char c)
{
    if (c < 0x20 || c > 0x7E)
        return;
    if (osk_echo_len >= OSK_ECHO_MAX) {
        memmove(osk_echo, osk_echo + 1, OSK_ECHO_MAX - 1);
        osk_echo_len = OSK_ECHO_MAX - 1;
    }
    osk_echo[osk_echo_len++] = c;
    osk_echo[osk_echo_len] = '\0';
}

static void osk_code(uint8_t sc)
{
    x86_kbd_scancode(sc);
    x86_osk_codes++;
}

/*
 * One keypress, as the 8042 would report it.
 *
 * A SHIFTED KEY MUST CARRY A REAL SHIFT. There is no "shifted keysym" here:
 * the guest reads scancodes, so '!' IS Shift + the '1' key and nothing else
 * produces it. The Left-Shift make/break brackets the key, which is what a real
 * keyboard sends and what x86_bios_kbd_irq()'s modifier tracking decodes -- a
 * game reading port 0x60 itself sees the same four bytes.
 *
 * A sticky Ctrl or Alt is expressed the same way, and the ORDER matters: the
 * modifier's MAKE has to be dequeued by the guest before the key's, or the
 * character arrives unmodified. The controller queue is FIFO, so pushing in
 * this order is the whole of it.
 */
static void osk_send(const osk_key_t *k)
{
    uint8_t shift = (osk_sticky & STICKY_SHIFT) != 0;

    if (osk_sticky & STICKY_CTRL) osk_code(0x1D);
    if (osk_sticky & STICKY_ALT)  osk_code(0x38);
    if (shift)                    osk_code(0x2A);

    osk_code(k->sc);
    osk_code((uint8_t)(k->sc | 0x80));

    if (shift)                    osk_code(0xAA);
    if (osk_sticky & STICKY_ALT)  osk_code(0xB8);
    if (osk_sticky & STICKY_CTRL) osk_code(0x9D);

    {
        uint8_t a = x86_kbd_ascii(k->sc, shift);
        x86_osk_last_key = ((uint32_t)k->sc << 8) | a;
        osk_echo_push((char)a);
    }
    x86_osk_presses++;
    /* Sticky, not locking: one keypress consumes the latch. */
    osk_sticky = 0;
}

/* ---------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

/* UP/DOWN: to the cap in the next row (wrapping) whose centre is nearest the
   current cap's centre -- rows have different cap widths, so keeping the
   column index would wander sideways. */
static void osk_move_row(int dr)
{
    int cx = key_x(osk_row, osk_col) + key_w(&osk_layout[osk_row].keys[osk_col]) / 2;
    uint8_t best = 0;
    int bestd = 1 << 30;

    osk_row = (uint8_t)((osk_row + OSK_ROWS + dr) % OSK_ROWS);
    for (uint8_t c = 0; c < osk_layout[osk_row].n; c++) {
        int m = key_x(osk_row, c) + key_w(&osk_layout[osk_row].keys[c]) / 2;
        int d = m > cx ? m - cx : cx - m;
        if (d < bestd) { bestd = d; best = c; }
    }
    osk_col = best;
}

static void osk_press(void)
{
    const osk_row_t *row = &osk_layout[osk_row];
    const osk_key_t *k   = &row->keys[osk_col];

    switch (k->flags & 0x0Fu) {
    case A_KEY:   osk_send(k); break;
    case A_KEY_SHIFT: osk_sticky |= STICKY_SHIFT; osk_send(k); break;
    case A_SHIFT: osk_sticky ^= STICKY_SHIFT; break;
    case A_CTRL:  osk_sticky ^= STICKY_CTRL; break;
    case A_ALT:   osk_sticky ^= STICKY_ALT; break;
    case A_CLOSE: osk_set_mode(MODE_OFF); break;
    default: break;
    }
}

/*
 * MOUSE MODE FEEDS THE INT 33h DRIVER (x86_mouse.c) -- which answers only for a
 * title whose .cfg says `mouse = on`. Without that the pointer still moves and
 * B/A still count as clicks, but no guest can ask where it is, exactly as
 * before the driver existed. The driver's scope (handlers accepted and never
 * called) is stated in x86_mouse.h rather than hidden: INT 33h has no error
 * channel.
 *
 * The acceleration ramp is ported unchanged, because a d-pad gives DIRECTION
 * ONLY: without a ramp the choice is between a pointer too slow to cross the
 * screen and one too coarse to hit a menu item. A single tap moves exactly one
 * unit, and a hold coarsens after ~10 frames.
 */
static int mouse_step(unsigned frames)
{
    unsigned s;
    if (frames < MOUSE_FINE_FRAMES)
        return MOUSE_FINE_MICKEYS;
    s = MOUSE_FINE_MICKEYS + (frames - MOUSE_FINE_FRAMES) / MOUSE_RAMP_DIV;
    if (s > MOUSE_MAX_MICKEYS)
        s = MOUSE_MAX_MICKEYS;
    return (int)s;
}

/* Directions only: ramp, move our pointer and the driver's. Shared by mouse
   mode and by `mouse = on` with the OSK closed (x86_osk_mouse_drive). */
static void mouse_steer(uint32_t now)
{
    static const uint32_t dir[4] = {
        X86_OSK_UP, X86_OSK_DOWN, X86_OSK_LEFT, X86_OSK_RIGHT
    };
    int dx = 0, dy = 0;

    for (int i = 0; i < 4; i++)
        mouse_held[i] = (now & dir[i]) ? (uint16_t)(mouse_held[i] + 1) : 0;

    /* Opposite directions cancel, which the sum does naturally. Do not
       special-case it: on a real d-pad it is a rocking motion during a
       direction change and cancelling is right for the frame it lasts. */
    if (mouse_held[0]) dy -= mouse_step(mouse_held[0] - 1u);
    if (mouse_held[1]) dy += mouse_step(mouse_held[1] - 1u);
    if (mouse_held[2]) dx -= mouse_step(mouse_held[2] - 1u);
    if (mouse_held[3]) dx += mouse_step(mouse_held[3] - 1u);

    {
        int nx = mouse_x + dx, ny = mouse_y + dy;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > MOUSE_VW - 1) nx = MOUSE_VW - 1;
        if (ny > MOUSE_VH - 1) ny = MOUSE_VH - 1;
        mouse_x = (int16_t)nx;
        mouse_y = (int16_t)ny;
    }
    if (dx || dy)
        x86_mouse_move(dx, dy);
}

void x86_osk_mouse_drive(uint32_t buttons)
{
    if (x86_osk_mode == MODE_OFF)
        mouse_steer(buttons);
}

static void mouse_input(uint32_t now)
{
    uint8_t btn = 0;

    mouse_steer(now);
    if (now & X86_OSK_B) btn |= 1u;    /* left  */
    if (now & X86_OSK_A) btn |= 2u;    /* right */
    if (btn != mouse_buttons) {
        mouse_buttons = btn;
        x86_mouse_clicks++;
        x86_mouse_buttons(btn);
    }
}

void x86_osk_input(uint32_t buttons)
{
    uint32_t went = buttons & ~osk_prev;
    osk_prev = buttons;

    /* Mouse mode is taken BEFORE the edge early-out, and that is load-bearing:
       the grid is edge-driven because a key must not repeat, but a pointer is
       LEVEL-driven -- a held direction has to produce motion every frame or
       there is nothing for the ramp to ramp. */
    if (x86_osk_mode == MODE_MOUSE) {
        if (went & X86_OSK_TIME) {
            /* TIME steps BACK to the key grid. GAME only goes forwards, so
               without this the way from mouse mode to the keyboard is two
               presses through OFF, which hands the buttons to the guest in
               between. */
            osk_set_mode(MODE_KBD);
            return;
        }
        if (went & X86_OSK_START)       /* zelda-only START: pointer on/off */
            mouse_render = !mouse_render;
        mouse_input(buttons);
        return;
    }

    /* Level-driven repeat, decided BEFORE the edge early-out because a held
       button produces no edges at all. A change of direction (including
       release) restarts the delay, so tapping never repeats. */
    {
        uint32_t dir = buttons & (X86_OSK_LEFT | X86_OSK_RIGHT | X86_OSK_UP | X86_OSK_DOWN);
        if (dir != osk_hold_dir) {
            osk_hold_dir = dir;
            osk_repeat_at = osk_now_ms + OSK_REPEAT_DELAY_MS;
        } else if (dir && osk_now_ms && osk_now_ms >= osk_repeat_at) {
            uint8_t n = osk_layout[osk_row].n;
            if (dir & X86_OSK_LEFT)
                osk_col = (uint8_t)((osk_col + n - 1) % n);
            else if (dir & X86_OSK_RIGHT)
                osk_col = (uint8_t)((osk_col + 1) % n);
            else
                osk_move_row((dir & X86_OSK_UP) ? -1 : 1);
            osk_repeat_at = osk_now_ms + OSK_REPEAT_RATE_MS;
        }
    }

    if (!went)
        return;

    if (went & X86_OSK_LEFT) {
        uint8_t n = osk_layout[osk_row].n;
        osk_col = (uint8_t)((osk_col + n - 1) % n);
    }
    if (went & X86_OSK_RIGHT) {
        uint8_t n = osk_layout[osk_row].n;
        osk_col = (uint8_t)((osk_col + 1) % n);
    }
    if (went & X86_OSK_UP)
        osk_move_row(-1);
    else if (went & X86_OSK_DOWN)
        osk_move_row(1);
    if (went & X86_OSK_A)
        osk_press();
    if (went & X86_OSK_B) {
        /* The most-used key gets a button of its own. */
        static const osk_key_t bsp = KS("BSP", 0x0E);
        osk_send(&bsp);
    }
    if (went & X86_OSK_TIME)
        osk_sticky ^= STICKY_SHIFT;     /* the second most-used: Shift, sticky */
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void x86_osk_set_now_ms(uint32_t ms)
{
    osk_now_ms = ms;
}

void x86_osk_reset(void)
{
    osk_now_ms = osk_hold_dir = osk_repeat_at = 0;
    x86_osk_mode  = MODE_OFF;
    osk_row = 2;                        /* home row, on 'a' */
    osk_col = 1;
    osk_sticky = 0;
    osk_prev = 0;
    osk_echo[0] = '\0';
    osk_echo_len = 0;
    osk_toast_frames = 0;
    mouse_x = MOUSE_VW / 2;
    mouse_y = MOUSE_VH / 2;
    mouse_buttons = 0;
    memset(mouse_held, 0, sizeof mouse_held);

    /* Measure the layout instead of trusting the arithmetic in the comments.
       draw_glyph() clips silently, so an over-long row loses its rightmost keys
       -- the OFF key, in every row that has one -- with no fault to notice. */
    x86_osk_row_px_max = 0;
    x86_osk_row_overflow = 0;
    for (unsigned r = 0; r < OSK_ROWS; r++) {
        uint32_t px = (uint32_t)key_x((uint8_t)r, osk_layout[r].n);
        if (px > x86_osk_row_px_max)
            x86_osk_row_px_max = px;
    }
    if (x86_osk_row_px_max > 320u)
        x86_osk_row_overflow = 1;
}

int x86_osk_visible(void) { return x86_osk_mode != MODE_OFF; }

void x86_osk_cycle(void)
{
    osk_set_mode((x86_osk_mode + 1u) % MODE_COUNT);
}
