#ifndef PICO_COMPAT_H
#define PICO_COMPAT_H

// Host (SDL) stub for the Pico SDK's pico.h. The emulator core only uses the
// section-placement macros; on the desktop they are no-ops.

#include <stdint.h>
#include <stdbool.h>

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif
#ifndef __not_in_flash
#define __not_in_flash(g)
#endif
#ifndef __time_critical_func
#define __time_critical_func(f) f
#endif
#ifndef __scratch_x
#define __scratch_x(g)
#endif
#ifndef __scratch_y
#define __scratch_y(g)
#endif

#endif // PICO_COMPAT_H
