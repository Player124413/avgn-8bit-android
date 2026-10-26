#include "boot_progress.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "gettime.h"
#include "log.h"

#ifdef PLATFORM_VITA
#include <psp2/kernel/processmgr.h>
#endif

static void (*tickHook)(void) = nullptr;
static int marksState = 0; // 0 = not decided, 1 = on, -1 = off (or finished)
static uint64_t lastMarkUs = 0;

uint64_t bsBootNowUs(void) {
#ifdef PLATFORM_VITA
    return (uint64_t) sceKernelGetProcessTimeWide(); // since the process started, executable loading included
#else
    static uint64_t base = 0;
    uint64_t now = nowNanos() / 1000;
    if (base == 0) base = now;
    return now - base;
#endif
}

bool bsBootMarksOn(void) {
    if (marksState == 0) {
#ifdef PLATFORM_VITA
        marksState = 1;
#else
        marksState = getenv("BS_BOOT_LOG") != nullptr ? 1 : -1;
#endif
    }
    return marksState > 0;
}

void bsBootMark(const char* fmt, ...) {
    if (!bsBootMarksOn()) return;
    char what[320];
    va_list va;
    va_start(va, fmt);
    vsnprintf(what, sizeof(what), fmt, va);
    va_end(va);
    uint64_t now = bsBootNowUs();
    logInfo("BOOT %8.1f ms (+%7.1f) %s\n", (double) now / 1000.0, (double) (now - lastMarkUs) / 1000.0, what);
    lastMarkUs = now;
}

void bsBootFinished(void) {
    marksState = -1;
}

void bsBootSetTickHook(void (*hook)(void)) {
    tickHook = hook;
}

void bsBootTick(void) {
    if (tickHook != nullptr) tickHook();
}
