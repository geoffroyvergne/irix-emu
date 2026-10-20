/*
 * glpass-probe: whether the emulator's GL bridge answers this glpass
 * library, for the boot script (/etc/init.d/glpass). Run it with the
 * library's directory first in LD_LIBRARYN32_PATH:
 *   LD_LIBRARYN32_PATH=/usr/lib32/glpass /usr/lib32/glpass/glpass-probe -v
 * Exit status (GLP_PROBE_*):
 *   0  the bridge answers: OpenGL can run on the host GPU
 *   1  no emulator: the hypercall traps (real hardware), caught as SIGILL
 *   2  the emulator has no host GL
 *   3  the emulator's glpass sources differ from this library's: rebuild
 * Any other status (the library did not load) also means "not glpass".
 */
#include <stdio.h>
#include <string.h>

#include "glpass_protocol.h"

extern int glpass_probe(void);

int main(int argc, char **argv)
{
    static const char *const what[] = {
        "the emulator's GL bridge answers: OpenGL runs on the host GPU",
        "no emulator (the hypercall traps): IRIX's own OpenGL",
        "the emulator has no host GL: IRIX's own OpenGL",
        "this library and the emulator come from different glpass sources: IRIX's own OpenGL until it is rebuilt",
    };
    const int result = glpass_probe();
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        printf("glpass-probe: %s\n", result >= 0 && result <= 3 ? what[result] : "unknown result");
    }
    return result;
}
