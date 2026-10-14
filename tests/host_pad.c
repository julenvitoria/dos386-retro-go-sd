/*
 * Host harness for x86_pad.c (per-title [gamepad] mapping + typematic repeat)
 * and for x86_cfg.c leaving that section alone. Runner: tests/test_pad.py.
 * No target: both files are freestanding.
 */
#include <stdio.h>
#include <string.h>
#include "x86_pad.h"
#include "x86_cfg.h"

static int checks, failures;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("FAIL: %s\n", what); }
}

static uint8_t emitted[256];
static unsigned n_emitted;
static void emit(uint8_t sc) { if (n_emitted < sizeof emitted) emitted[n_emitted++] = sc; }

static const uint8_t defaults[X86_PAD_BUTTONS] = { 0x48, 0x50, 0x4B, 0x4D, 0x39, 0x01, 0x00, 0x15, 0x1C, 0x31 };

static void parse(const char *txt, x86_pad_result_t *r)
{
    x86_pad_defaults(defaults);
    x86_pad_parse((const unsigned char *)txt, (unsigned long)strlen(txt), r);
}

int main(void)
{
    x86_pad_result_t r;

    /* no [gamepad] section: the compiled-in table, untouched */
    parse("[cpu]\ncycles = 3000\na = shift\n", &r);
    check(r.applied == 0 && r.bad == 0, "keys outside [gamepad] are not this module's");
    check(x86_pad_keys[4][0] == 0x39 && x86_pad_keys[4][1] == 0, "default A stays Space");

    /* bindings, combos, aliases, none, comments, CRLF, case */
    parse("; title\r\n[GamePad]\r\nA = Shift   ; grab\r\nb=ctrl+alt+del\r\ntime : n\r\n"
          "select = none\r\narriba = q\r\nx = f5\r\n", &r);
    check(r.applied == 6 && r.bad == 0, "six bindings applied, none refused");
    check(x86_pad_keys[4][0] == 0x2A && x86_pad_keys[4][1] == 0, "a = shift");
    check(x86_pad_keys[5][0] == 0x1D && x86_pad_keys[5][1] == 0x38 && x86_pad_keys[5][2] == 0x53, "b = ctrl+alt+del");
    check(x86_pad_keys[7][0] == 0x31, "time = n (':' separator)");
    check(x86_pad_keys[9][0] == 0, "select = none unbinds");
    check(x86_pad_keys[0][0] == 0x10, "arriba (Spanish alias) = q");
    check(x86_pad_keys[8][0] == 0x3F, "x (alias of start) = f5");
    check(x86_pad_keys[1][0] == 0x50, "an unnamed button keeps its default");

    /* refusals: unknown key, unknown button, four keys, and GAME */
    parse("[gamepad]\na = banana\nwheel = space\nb = a+b+c+d\ngame = esc\nup = w\n", &r);
    check(r.applied == 1 && r.bad == 4, "four refusals, the good line still applies");
    check(x86_pad_keys[4][0] == 0x39, "a bad value keeps the default");
    check(x86_pad_keys[X86_PAD_GAME][0] == 0, "game is refused: the OSK door cannot be rebound");
    check(x86_pad_keys[0][0] == 0x11, "up = w after the refusals");

    /* a later section ends [gamepad] */
    parse("[gamepad]\na = z\n[dos]\nb = x\n", &r);
    check(x86_pad_keys[4][0] == 0x2C && x86_pad_keys[5][0] == 0x01, "[gamepad] ends at the next header");

    /* press / release order for a combo */
    parse("[gamepad]\nb = ctrl+c\n", &r);
    n_emitted = 0;
    x86_pad_button(5, 1, emit);
    x86_pad_button(5, 0, emit);
    check(n_emitted == 4 && emitted[0] == 0x1D && emitted[1] == 0x2E && emitted[2] == 0xAE && emitted[3] == 0x9D,
          "combo pressed in order, released in reverse");
    n_emitted = 0;
    x86_pad_button(X86_PAD_GAME, 1, emit);
    check(n_emitted == 0, "an unbound button emits nothing");

    /* typematic repeat: nothing before 500 ms, then every 50 ms, last key only */
    parse("[gamepad]\nb = ctrl+c\na = shift\n", &r);
    n_emitted = 0;
    {
        unsigned t;
        for (t = 0; t <= 1000; t += 10)
            x86_pad_repeat((1u << 5) | (1u << 4), 10000 + t, emit);
    }
    check(n_emitted == 11, "held 1 s: repeats at 500..1000 ms every 50 ms");
    {
        unsigned i, only_c = 1;
        for (i = 0; i < n_emitted; i++) if (emitted[i] != 0x2E) only_c = 0;
        check(only_c, "only the combo's last key repeats; a held modifier never does");
    }
    n_emitted = 0;
    x86_pad_repeat(0, 20000, emit);
    x86_pad_repeat(1u << 5, 20010, emit);
    x86_pad_repeat(1u << 5, 20300, emit);
    check(n_emitted == 0, "a release resets the delay");
    parse("[gamepad]\nrepeat = off\n", &r);
    n_emitted = 0;
    x86_pad_repeat(1u << 0, 0, emit);
    x86_pad_repeat(1u << 0, 2000, emit);
    check(r.applied == 1 && n_emitted == 0, "repeat = off");

    /* x86_cfg.c skips [gamepad]: its keys are not "unknown" there */
    {
        const char *txt = "[gamepad]\na = shift\nfoo = bar\n[cpu]\ncycles = 3000\n";
        x86_cfg_result_t cr;
        x86_cfg_status_t st = x86_cfg_parse((const unsigned char *)txt, (unsigned long)strlen(txt), &cr);
        check(st == X86_CFG_OK && cr.unknown == 0 && cr.malformed == 0 && cr.applied == 1,
              "x86_cfg leaves [gamepad] to x86_pad and still applies [cpu]");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
