#ifndef _BS_INPUT_TRACE_H_
#define _BS_INPUT_TRACE_H_

// A short, deduplicated trace of the input the game asks for: which keys it polls, which gamepad buttons, on which
// device, and what it got. "The touch buttons do nothing" cannot be worked out from a screenshot - either the app
// sends a key the game never reads, or the game never asks for the gamepad at all - and both are visible here.
//
// Only the Android build writes it (it ends up in the in-app log), and only the first few hundred distinct calls:
// a game polls its keys every frame, so an unlimited trace would be megabytes of the same line.

#include <stdint.h>

#if defined(__ANDROID__)

#include <stdbool.h>
#include "log.h"

#define BS_INPUT_TRACE_SLOTS 2048
#define BS_INPUT_TRACE_LINES 500

typedef struct {
    uint64_t keys[BS_INPUT_TRACE_SLOTS];
    int32_t last[BS_INPUT_TRACE_SLOTS];
    int32_t lines;
} BsInputTrace;

static BsInputTrace bsInputTrace = {0};

static uint64_t bsInputTraceHash(const char* name, int32_t a, int32_t b) {
    uint64_t h = 1469598103934665603ULL;
    while (*name != '\0') {
        h ^= (uint64_t) (unsigned char) *name++;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t) (uint32_t) a;
    h *= 1099511628211ULL;
    h ^= (uint64_t) (uint32_t) b;
    h *= 1099511628211ULL;
    return h == 0 ? 1 : h;
}

/** Notes one call: the first time it is seen, and every time its answer changes to something other than 0. */
static inline void bsInputTraceNote(const char* name, int32_t a, int32_t b, int32_t value) {
    if (bsInputTrace.lines >= BS_INPUT_TRACE_LINES) return;
    uint64_t key = bsInputTraceHash(name, a, b);
    uint32_t slot = (uint32_t) (key % BS_INPUT_TRACE_SLOTS);
    for (uint32_t probe = 0; probe < 8; probe++) {
        uint32_t at = (slot + probe) % BS_INPUT_TRACE_SLOTS;
        if (bsInputTrace.keys[at] == key) {
            if (bsInputTrace.last[at] != value && value != 0) {
                bsInputTrace.last[at] = value;
                bsInputTrace.lines++;
                logInfo("INPUT %s(%d, %d) = %d\n", name, a, b, value);
            }
            return;
        }
        if (bsInputTrace.keys[at] == 0) {
            bsInputTrace.keys[at] = key;
            bsInputTrace.last[at] = value;
            bsInputTrace.lines++;
            logInfo("INPUT %s(%d, %d) = %d\n", name, a, b, value);
            return;
        }
    }
}

#define BS_TRACE_INPUT(name, a, b, value) bsInputTraceNote(name, (int32_t) (a), (int32_t) (b), (int32_t) (value))

#else

#define BS_TRACE_INPUT(name, a, b, value) ((void) 0)

#endif

#endif /* _BS_INPUT_TRACE_H_ */
