// Resumable GameMaker-QOI ("fioq") pixel decoder that works a row at a time.
// Pixels are decoded as 32-bit words (bytes r, g, b, a in memory) into a small cached row buffer, and each
// finished row is copied to the destination with one memcpy. On the Vita the destination is the texture's
// uncached GPU memory: one burst copy per 16 KiB row instead of four byte stores per pixel.
// Output is byte-identical to ImageDecoder's decodeQoi / the original streaming loop, including their
// end-of-data behaviour (a stream that ends between ops repeats the last pixel; one cut inside an op fails).
#ifndef BS_QOI_ROWS_H
#define BS_QOI_ROWS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

typedef struct {
    uint32_t index[64];
    uint32_t px;      // current pixel, r | g << 8 | b << 16 | a << 24
    int32_t run;      // pixels still to repeat px for
    int32_t x;        // pixels already in the row buffer
} BsQoiState;

static inline void bsQoiInit(BsQoiState* st) {
    memset(st, 0, sizeof(*st));
    st->px = 0xFF000000u; // r = g = b = 0, a = 255
}

static inline uint32_t bsQoiHash(uint32_t p) {
    uint32_t h = p ^ (p >> 16);
    return (h ^ (h >> 8)) & 63;
}

// Decodes from in[0..inLen) into rowbuf (width words), copying each completed row to dst (rows dstStride
// apart) until rowsLeft rows are done or the input runs out. *consumed = input bytes used (an op cut off at
// the end of the input is left unconsumed unless final). Returns the number of rows written, or -1 on error.
// With final set the input is all there is: running out between ops repeats the current pixel to the end of
// the image (as the original decoders do), running out inside an op is an error.
static int bsQoiDecodeRows(BsQoiState* st, const uint8_t* in, size_t inLen, size_t* consumed,
                           uint32_t* rowbuf, int32_t width, uint8_t* dst, size_t dstStride, int32_t rowsLeft, bool final) {
    uint32_t px = st->px;
    int32_t run = st->run, x = st->x;
    size_t pos = 0;
    int32_t rows = 0;
    while (rows < rowsLeft) {
        if (x == width) {
            memcpy(dst + (size_t) rows * dstStride, rowbuf, (size_t) width * 4);
            rows++;
            x = 0;
            continue;
        }
        if (run > 0) {
            int32_t n = width - x;
            if (run < n) n = run;
            uint32_t* o = rowbuf + x;
            for (int32_t i = 0; i < n; i++) o[i] = px;
            x += n;
            run -= n;
            continue;
        }
        if (pos >= inLen) {
            if (!final) break;
            run = INT32_MAX; // out of data between ops: the current pixel repeats to the end
            continue;
        }
        uint32_t b1 = in[pos];
        if ((b1 & 0xC0) == 0x00) {                 // INDEX
            px = st->index[b1 & 0x3F];
            pos += 1;
        } else if ((b1 & 0xE0) == 0x40) {          // RUN_8: this pixel + (b1 & 31) more
            run = (int32_t) (b1 & 0x1F);
            pos += 1;
        } else if ((b1 & 0xE0) == 0x60) {          // RUN_16
            if (pos + 1 >= inLen) { if (!final) break; return -1; }
            run = (int32_t) ((((b1 & 0x1F) << 8) | in[pos + 1]) + 32);
            pos += 2;
        } else if ((b1 & 0xC0) == 0x80) {          // DIFF_8 (2-2-2, two's complement)
            uint32_t r = (px + ((((b1 >> 4) & 3) ^ 2) - 2)) & 0xFF;
            uint32_t g = ((px >> 8) + ((((b1 >> 2) & 3) ^ 2) - 2)) & 0xFF;
            uint32_t b = ((px >> 16) + (((b1 & 3) ^ 2) - 2)) & 0xFF;
            px = (px & 0xFF000000u) | r | (g << 8) | (b << 16);
            pos += 1;
        } else if ((b1 & 0xE0) == 0xC0) {          // DIFF_16 (5-4-4)
            if (pos + 1 >= inLen) { if (!final) break; return -1; }
            uint32_t m = (b1 << 8) | in[pos + 1];
            uint32_t r = (px + ((((m >> 8) & 0x1F) ^ 16) - 16)) & 0xFF;
            uint32_t g = ((px >> 8) + ((((m >> 4) & 0x0F) ^ 8) - 8)) & 0xFF;
            uint32_t b = ((px >> 16) + (((m & 0x0F) ^ 8) - 8)) & 0xFF;
            px = (px & 0xFF000000u) | r | (g << 8) | (b << 16);
            pos += 2;
        } else if ((b1 & 0xF0) == 0xE0) {          // DIFF_24 (5-5-5-5)
            if (pos + 2 >= inLen) { if (!final) break; return -1; }
            uint32_t m = (b1 << 16) | ((uint32_t) in[pos + 1] << 8) | in[pos + 2];
            uint32_t r = (px + ((((m >> 15) & 0x1F) ^ 16) - 16)) & 0xFF;
            uint32_t g = ((px >> 8) + ((((m >> 10) & 0x1F) ^ 16) - 16)) & 0xFF;
            uint32_t b = ((px >> 16) + ((((m >> 5) & 0x1F) ^ 16) - 16)) & 0xFF;
            uint32_t a = ((px >> 24) + (((m & 0x1F) ^ 16) - 16)) & 0xFF;
            px = r | (g << 8) | (b << 16) | (a << 24);
            pos += 3;
        } else {                                   // COLOR: the flagged channels follow as raw bytes
            size_t need = 1 + ((b1 >> 3) & 1) + ((b1 >> 2) & 1) + ((b1 >> 1) & 1) + (b1 & 1);
            if (pos + need > inLen) { if (!final) break; return -1; }
            size_t p = pos + 1;
            if (b1 & 8) { px = (px & 0xFFFFFF00u) | in[p++]; }
            if (b1 & 4) { px = (px & 0xFFFF00FFu) | ((uint32_t) in[p++] << 8); }
            if (b1 & 2) { px = (px & 0xFF00FFFFu) | ((uint32_t) in[p++] << 16); }
            if (b1 & 1) { px = (px & 0x00FFFFFFu) | ((uint32_t) in[p++] << 24); }
            pos = p;
        }
        st->index[bsQoiHash(px)] = px;
        rowbuf[x++] = px;
    }
    st->px = px;
    st->run = run;
    st->x = x;
    *consumed = pos;
    return rows;
}

#endif
