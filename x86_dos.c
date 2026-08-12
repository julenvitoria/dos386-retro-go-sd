/*
 * DOS layer. PSP, MCB arena, MZ loader and INT 21h.
 *
 * Under construction: the dispatch and the memory model land first so the
 * services can be added against a working arena rather than the other way
 * round. Every unimplemented function reports itself instead of returning
 * success -- a DOS call that silently "works" fails much later somewhere
 * unrelated, which is the most expensive shape of bug this project has hit.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"

int x86_dos_terminate(void)
{
    cpu_state_ptr->exit_reason = X86_EXIT_TERMINATE;
    return 1;
}

int x86_dos_int21(uint8_t func)
{
    switch (func) {
    case 0x4C:
    case 0x00:
        return x86_dos_terminate();
    case 0x30:                       /* DOS version: report 5.00 */
        x86_set_ax(0x0005);
        return 1;
    case 0x02:                       /* character output */
        x86_video_teletype((uint8_t)(cpu_state_ptr->regs[2] & 0xFF));
        return 1;
    case 0x09: {                     /* $-terminated string output */
        uint32_t lin = cpu_state_ptr->segs[3].base
                     + (cpu_state_ptr->regs[2] & 0xFFFF);
        for (unsigned i = 0; i < 1024; i++) {
            uint8_t *p = x86_guest_ptr(lin + i, 1);
            if (!p || *p == '$')
                break;
            x86_video_teletype(*p);
        }
        return 1;
    }
    default:
        x86_hle_unhandled_vector = 0x21;
        x86_hle_unhandled_func = func;
        return 0;
    }
}
