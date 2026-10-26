#pragma once

// Boot timing and the boot indicator.
//
// bsBootMark(): one "BOOT <ms since the process started> (+<ms since the previous mark>) <what>" log line per boot
// phase, until bsBootFinished() (the first presented frame). Always on in the Vita build (the clock includes loading
// the executable); elsewhere only with the environment variable BS_BOOT_LOG set.
//
// bsBootTick(): cheap call for the long blocking steps of the boot (data.win parse, shader compiles, global scripts,
// fonts, waiting for the big texture page). While the Vita boot indicator is up it redraws the game's loading icon
// when its animation frame changes; otherwise it does nothing. Only call it where no frame is being drawn.

#include <stdbool.h>
#include <stdint.h>

#if defined(__GNUC__)
#define BS_BOOT_PRINTF __attribute__((format(printf, 1, 2)))
#else
#define BS_BOOT_PRINTF
#endif

void bsBootMark(const char* fmt, ...) BS_BOOT_PRINTF;
void bsBootFinished(void);
bool bsBootMarksOn(void);
uint64_t bsBootNowUs(void);

void bsBootSetTickHook(void (*hook)(void));
void bsBootTick(void);

#ifdef PLATFORM_VITA
// src/vita/boot_indicator.c: the game's loading icon (sprLoadingIcon) in the bottom-right corner, on black.
void bsBootIndicatorBegin(void);   // right after vglInit: loads app0:loading_icon.png, shows the first frame
void bsBootIndicatorEnd(void);     // before the game's first frame
#else
static inline void bsBootIndicatorBegin(void) {}
static inline void bsBootIndicatorEnd(void) {}
#endif
