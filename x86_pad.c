/*
 * x86_pad.c -- per-title gamepad mapping and typematic repeat. Contract and
 * format: x86_pad.h. Ported from the gw-dos (XTulator) frontend's gamecfg.c.
 *
 * COLD PATH except x86_pad_button() / x86_pad_repeat(), which run once per
 * frame from poll_input() and only touch a 30-byte table.
 */

#include <string.h>
#include "x86_pad.h"

/* NEGATIVE CONTROL -- 0 in every real build. tests/test_pad.py --negative
   builds with it and REQUIRES the "game is refused" check to fail. */
#ifndef X86_PAD_CTL_ALLOWGAME
#define X86_PAD_CTL_ALLOWGAME 0
#endif

uint8_t x86_pad_keys[X86_PAD_BUTTONS][X86_PAD_KEYS];
uint8_t x86_pad_repeat_on = 1;

static uint32_t hold_since[X86_PAD_BUTTONS];    /* ms the button went down */
static uint32_t last_rep[X86_PAD_BUTTONS];      /* ms of the last repeat   */
static uint32_t held_prev;

/*
 * KEY NAMES. Letters, digits and F1..F12 are computed from the keyboard rows
 * rather than listed (they are most of the names and all of the RAM_EMU this
 * table would cost -- every byte here is guest pager memory). The rest is one
 * packed string, NAME\0 per entry, with the make codes in a parallel array.
 */
static const char key_rows[4][11] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const uint8_t key_row_base[4] = { 0x02, 0x10, 0x1E, 0x2C };

static const char key_names[] =
    "esc\0-\0=\0backspace\0tab\0[\0]\0enter\0ctrl\0;\0'\0`\0shift\0\\\0"
    ",\0.\0/\0rshift\0alt\0space\0capslock\0home\0up\0pgup\0left\0right\0"
    "end\0down\0pgdn\0ins\0del\0kp0\0kp1\0kp2\0kp3\0kp4\0kp5\0kp6\0kp7\0kp8\0"
    "kp9\0kpminus\0kpplus\0kpmul\0semicolon\0"
    /* Spanish */
    "intro\0espacio\0mayus\0retroceso\0supr\0arriba\0abajo\0izquierda\0derecha\0"
    "inicio\0fin\0repag\0avpag\0";
static const uint8_t key_codes[] = {
    0x01, 0x0C, 0x0D, 0x0E, 0x0F, 0x1A, 0x1B, 0x1C, 0x1D, 0x27, 0x28, 0x29, 0x2A, 0x2B,
    0x33, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3A, 0x47, 0x48, 0x49, 0x4B, 0x4D,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48,
    0x49, 0x4A, 0x4E, 0x37, 0x27,
    0x1C, 0x39, 0x2A, 0x0E, 0x53, 0x48, 0x50, 0x4B, 0x4D,
    0x47, 0x4F, 0x49, 0x51,
};

/* button names -> poll_input() bit; aliases share a bit */
static const struct { const char *name; uint8_t bit; } button_names[] = {
    {"up",0},{"arriba",0},{"down",1},{"abajo",1},{"left",2},{"izquierda",2},
    {"right",3},{"derecha",3},{"a",4},{"b",5},{"game",X86_PAD_GAME},
    {"time",7},{"start",8},{"x",8},{"select",9},{"y",9},
    {0,0}
};

static unsigned char lower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }
static int is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* case-insensitive compare of [s,e) with a NUL-terminated lower-case name */
static int eq(const unsigned char *s, const unsigned char *e, const char *name)
{
    while (s < e && *name) {
        if (lower(*s) != (unsigned char)*name)
            return 0;
        s++; name++;
    }
    return s == e && !*name;
}

/* make code for a key name, or 0 */
static int key_lookup(const unsigned char *s, const unsigned char *e)
{
    unsigned r, k = 0;
    const char *p = key_names;
    if (e - s == 1) {                                   /* a letter or a digit */
        unsigned char c = lower(*s);
        for (r = 0; r < 4; r++) {
            const char *q;
            for (q = key_rows[r]; *q; q++)
                if ((unsigned char)*q == c)
                    return key_row_base[r] + (int)(q - key_rows[r]);
        }
    }
    if (e - s >= 2 && e - s <= 3 && lower(*s) == 'f') {  /* f1..f12 */
        unsigned f = 0;
        const unsigned char *d;
        for (d = s + 1; d < e; d++) {
            if (*d < '0' || *d > '9')
                { f = 0; break; }
            f = f * 10 + (unsigned)(*d - '0');
        }
        if (f >= 1 && f <= 10) return 0x3A + (int)f;
        if (f == 11 || f == 12) return 0x4C + (int)f;
    }
    while (*p) {
        if (eq(s, e, p))
            return key_codes[k];
        p += strlen(p) + 1;
        k++;
    }
    return 0;
}

static int is_modifier(uint8_t sc) { return sc == 0x1D || sc == 0x2A || sc == 0x36 || sc == 0x38; }

void x86_pad_defaults(const uint8_t single[X86_PAD_BUTTONS])
{
    unsigned i;
    memset(x86_pad_keys, 0, sizeof x86_pad_keys);
    for (i = 0; i < X86_PAD_BUTTONS; i++)
        x86_pad_keys[i][0] = single[i];
    x86_pad_repeat_on = 1;
    held_prev = 0;
}

/* "ctrl+c" -> {0x1D, 0x2E, 0}; "none" -> {0}. 0 on a bad name. */
static int parse_keys(const unsigned char *v, const unsigned char *ve, uint8_t out[X86_PAD_KEYS])
{
    unsigned n = 0;
    uint8_t tmp[X86_PAD_KEYS] = {0, 0, 0};
    if (eq(v, ve, "none") || eq(v, ve, "nada")) {
        memset(out, 0, X86_PAD_KEYS);
        return 1;
    }
    while (v < ve) {
        const unsigned char *p = v + 1, *te;      /* v+1: a key named "+" is not supported */
        while (p < ve && *p != '+')
            p++;
        te = p;
        while (v < te && is_space(*v)) v++;
        while (te > v && is_space(te[-1])) te--;
        {
            int sc = key_lookup(v, te);
            if (sc <= 0 || n >= X86_PAD_KEYS)
                return 0;
            tmp[n++] = (uint8_t)sc;
        }
        v = p < ve ? p + 1 : ve;
    }
    if (!n)
        return 0;
    memcpy(out, tmp, X86_PAD_KEYS);
    return 1;
}

void x86_pad_parse(const unsigned char *buf, unsigned long len, x86_pad_result_t *out)
{
    x86_pad_result_t r = {0, 0};
    unsigned long i = 0;
    int in_pad = 0;

    while (buf && i < len) {
        unsigned long ls = i, le, p;
        const unsigned char *k, *ke, *v, *ve;
        while (i < len && buf[i] != '\n')
            i++;
        le = i;
        if (i < len)
            i++;
        while (ls < le && is_space(buf[ls])) ls++;
        while (le > ls && is_space(buf[le - 1])) le--;
        if (ls == le || buf[ls] == '#' || buf[ls] == ';')
            continue;
        if (buf[ls] == '[') {
            for (p = ls + 1; p < le && buf[p] != ']'; p++)
                ;
            in_pad = (p < le) && eq(buf + ls + 1, buf + p, "gamepad");
            continue;
        }
        if (!in_pad)
            continue;
        /* trailing comment: whitespace then # or ; (a ';' key must be written
           "semicolon", which is why that name exists) */
        for (p = ls; p + 1 < le; p++)
            if (is_space(buf[p]) && (buf[p + 1] == '#' || buf[p + 1] == ';')) {
                le = p;
                break;
            }
        while (le > ls && is_space(buf[le - 1])) le--;
        for (p = ls; p < le && buf[p] != '=' && buf[p] != ':'; p++)
            ;
        if (p == le) { r.bad++; continue; }
        k = buf + ls; ke = buf + p;
        v = buf + p + 1; ve = buf + le;
        while (ke > k && is_space(ke[-1])) ke--;
        while (v < ve && is_space(*v)) v++;

        if (eq(k, ke, "repeat") || eq(k, ke, "repetir")) {
            x86_pad_repeat_on = !(eq(v, ve, "off") || eq(v, ve, "false") || eq(v, ve, "no") || eq(v, ve, "0"));
            r.applied++;
            continue;
        }
        {
            unsigned b;
            int found = -1;
            for (b = 0; button_names[b].name; b++)
                if (eq(k, ke, button_names[b].name)) { found = button_names[b].bit; break; }
            if (found < 0 || (found == X86_PAD_GAME && !X86_PAD_CTL_ALLOWGAME)) { r.bad++; continue; }
            if (parse_keys(v, ve, x86_pad_keys[found])) r.applied++;
            else r.bad++;
        }
    }
    if (out)
        *out = r;
}

void x86_pad_button(unsigned i, int down, void (*emit)(uint8_t sc))
{
    int n = 0, j;
    if (i >= X86_PAD_BUTTONS)
        return;
    while (n < X86_PAD_KEYS && x86_pad_keys[i][n])
        n++;
    if (down)
        for (j = 0; j < n; j++) emit(x86_pad_keys[i][j]);
    else
        for (j = n - 1; j >= 0; j--) emit((uint8_t)(x86_pad_keys[i][j] | 0x80));
}

unsigned x86_pad_repeat(uint32_t held, uint32_t now_ms, void (*emit)(uint8_t sc))
{
    unsigned i, sent = 0;
    for (i = 0; i < X86_PAD_BUTTONS; i++) {
        uint32_t m = 1u << i;
        if (!(held & m))
            continue;
        if (!(held_prev & m)) {                     /* went down this frame */
            hold_since[i] = now_ms;
            last_rep[i] = now_ms;
            continue;
        }
        if (!x86_pad_repeat_on)
            continue;
        if (now_ms - hold_since[i] < X86_PAD_REPEAT_DELAY_MS
            || now_ms - last_rep[i] < X86_PAD_REPEAT_RATE_MS)
            continue;
        {
            int n = 0;
            while (n < X86_PAD_KEYS && x86_pad_keys[i][n])
                n++;
            if (n && !is_modifier(x86_pad_keys[i][n - 1])) {
                emit(x86_pad_keys[i][n - 1]);
                sent++;
            }
        }
        last_rep[i] = now_ms;
    }
    held_prev = held;
    return sent;
}
