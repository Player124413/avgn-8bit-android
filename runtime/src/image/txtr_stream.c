#include "txtr_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>

#include "gettime.h"
#include "qoi_rows.h"

#define READ_CHUNK (256 * 1024)  // file reads (cache stream / BZ2 blob)
#define QOI_WINDOW (64 * 1024)   // decompressed QOI bytes per decode call (BZ2 path)
#define QOI_CARRY 16             // an op cut off at a buffer end is at most 5 bytes

const char* TxtrStream_dataWinPath = NULL;

// ===[ CRC-32 (zlib polynomial, slice-by-4) ]===
static uint32_t crcTable[4][256];
static volatile int crcReady = 0;

static void crcInit(void) {
    if (crcReady) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = crcTable[0][i];
        for (int t = 1; t < 4; t++) { c = crcTable[0][c & 0xFF] ^ (c >> 8); crcTable[t][i] = c; }
    }
    __sync_synchronize();
    crcReady = 1; // TxtrStream_crc32(0, NULL, 0) on the main thread before starting workers builds it once
}

uint32_t TxtrStream_crc32(uint32_t crc, const uint8_t* p, size_t n) {
    crcInit();
    uint32_t c = ~crc;
    while (n > 0 && ((uintptr_t) p & 3) != 0) { c = crcTable[0][(c ^ *p++) & 0xFF] ^ (c >> 8); n--; }
    while (n >= 4) {
        uint32_t w;
        memcpy(&w, p, 4); // little-endian (x86, ARM)
        c ^= w;
        c = crcTable[3][c & 0xFF] ^ crcTable[2][(c >> 8) & 0xFF] ^ crcTable[1][(c >> 16) & 0xFF] ^ crcTable[0][c >> 24];
        p += 4;
        n -= 4;
    }
    while (n-- > 0) c = crcTable[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ===[ helpers ]===
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }
static uint32_t get32(const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t) v); put32(p + 4, (uint32_t) (v >> 32)); }

static void note(TxtrStreamStats* st, const char* fmt, ...) {
    size_t used = strlen(st->note);
    if (used + 3 >= sizeof(st->note)) return;
    if (used > 0) { st->note[used++] = ';'; st->note[used++] = ' '; st->note[used] = '\0'; }
    va_list va;
    va_start(va, fmt);
    vsnprintf(st->note + used, sizeof(st->note) - used, fmt, va);
    va_end(va);
}

// The job's reads (256 KiB) and writes (64 KiB) are big: unbuffered streams make each one a single read/write call.
// Buffered, newlib's fread refills the stream buffer one buffer at a time (st_blksize, else BUFSIZ = 1 KiB on
// the Vita): up to ~24000 sceIoRead calls for the 13.6 MB cache file + the 10 MB blob.
static FILE* openUnbuffered(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (f != NULL) setvbuf(f, NULL, _IONBF, 0);
    return f;
}

static size_t timedRead(void* dst, size_t n, FILE* f, TxtrStreamStats* st) {
    uint64_t t0 = nowNanos();
    size_t got = fread(dst, 1, n, f);
    st->nsRead += nowNanos() - t0;
    st->bytesRead += got;
    return got;
}

// Cache header layout (little-endian, TXTR_CACHE_HEADER_SIZE bytes, then the stream):
//   0 magic "BSTXC001" | 8 header size | 12 page id | 16 width | 20 height | 24 blob offset | 28 blob size
//  32 data.win size (u64) | 40 blob head[16] | 56 blob tail[32] | 88 stream size | 92 stream CRC-32
//  96 format (1 = GameMaker QOI stream, "fioq" header included)
// 100 CRC-32 of 8 samples of the blob (32 bytes at blobSize * k / 9, k = 1..8) | 104.. zero
#define KEY_SAMPLES 8
#define KEY_SAMPLE_BYTES 32
typedef struct {
    uint8_t head[16];
    uint8_t tail[32];
    uint32_t sampleCrc;
} BlobKey;

static bool readBlobKey(const TxtrStreamJob* job, FILE* dw, BlobKey* key, TxtrStreamStats* st) {
    if (job->blobSize < sizeof(key->head) + sizeof(key->tail) + KEY_SAMPLE_BYTES) return false;
    if (fseek(dw, (long) job->blobOffset, SEEK_SET) != 0 || timedRead(key->head, sizeof(key->head), dw, st) != sizeof(key->head)) return false;
    if (fseek(dw, (long) (job->blobOffset + job->blobSize - sizeof(key->tail)), SEEK_SET) != 0 ||
        timedRead(key->tail, sizeof(key->tail), dw, st) != sizeof(key->tail)) return false;
    // the tail holds bzip2's CRC of the whole stream unless the blob is padded: samples from the middle as well
    key->sampleCrc = 0;
    for (uint32_t k = 1; k <= KEY_SAMPLES; k++) {
        uint8_t sample[KEY_SAMPLE_BYTES];
        uint32_t at = (uint32_t) (((uint64_t) job->blobSize * k) / (KEY_SAMPLES + 1));
        if (at > job->blobSize - KEY_SAMPLE_BYTES) at = job->blobSize - KEY_SAMPLE_BYTES;
        if (fseek(dw, (long) (job->blobOffset + at), SEEK_SET) != 0 || timedRead(sample, sizeof(sample), dw, st) != sizeof(sample)) return false;
        key->sampleCrc = TxtrStream_crc32(key->sampleCrc, sample, sizeof(sample));
    }
    return true;
}

static void buildHeader(uint8_t* h, const TxtrStreamJob* job, const BlobKey* key, uint32_t streamSize, uint32_t crc) {
    memset(h, 0, TXTR_CACHE_HEADER_SIZE);
    memcpy(h, TXTR_CACHE_MAGIC, 8);
    put32(h + 8, TXTR_CACHE_HEADER_SIZE);
    put32(h + 12, job->pageId);
    put32(h + 16, (uint32_t) job->width);
    put32(h + 20, (uint32_t) job->height);
    put32(h + 24, job->blobOffset);
    put32(h + 28, job->blobSize);
    put64(h + 32, job->dataWinSize);
    memcpy(h + 40, key->head, 16);
    memcpy(h + 56, key->tail, 32);
    put32(h + 88, streamSize);
    put32(h + 92, crc);
    put32(h + 96, 1);
    put32(h + 100, key->sampleCrc);
}

// ===[ decode from the cache file ]===
// Returns 1 = page decoded, 0 = cache not usable (state + note set; dst may be partly written).
static int decodeFromCache(const TxtrStreamJob* job, const char* cachePath, const BlobKey* key, volatile int32_t* rowsDone,
                           TxtrStreamStats* st, uint8_t* buf, uint32_t* rowbuf) {
    FILE* f = openUnbuffered(cachePath, "rb");
    if (f == NULL) { st->cacheState = TXTR_CACHE_MISSING; return 0; }
    uint8_t h[TXTR_CACHE_HEADER_SIZE], want[TXTR_CACHE_HEADER_SIZE];
    if (timedRead(h, sizeof(h), f, st) != sizeof(h)) { fclose(f); st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache header cut off"); return 0; }
    uint32_t streamSize = get32(h + 88), crcWant = get32(h + 92);
    buildHeader(want, job, key, streamSize, crcWant);
    if (memcmp(h, want, sizeof(h)) != 0) {
        fclose(f);
        st->cacheState = memcmp(h, TXTR_CACHE_MAGIC, 8) == 0 && get32(h + 8) == TXTR_CACHE_HEADER_SIZE ? TXTR_CACHE_STALE : TXTR_CACHE_DAMAGED;
        note(st, "%s", st->cacheState == TXTR_CACHE_STALE ? "cache is for another data.win" : "cache header damaged");
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) != (long) (TXTR_CACHE_HEADER_SIZE + (uint64_t) streamSize) || streamSize < 12 ||
        fseek(f, TXTR_CACHE_HEADER_SIZE, SEEK_SET) != 0) {
        fclose(f); st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache size mismatch (%lu stream bytes in the header)", (unsigned long) streamSize); return 0;
    }
    st->streamBytes = streamSize;

    BsQoiState q;
    bsQoiInit(&q);
    uint32_t crc = 0;
    uint64_t left = streamSize;
    size_t len = 0;           // bytes in buf not yet decoded
    bool header = false;
    int32_t y = 0;
    while (y < job->height) {
        if (left > 0 && len < QOI_CARRY) {
            size_t want = left < READ_CHUNK ? (size_t) left : READ_CHUNK;
            size_t got = timedRead(buf + len, want, f, st);
            if (got != want) { fclose(f); st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache read failed"); return 0; }
            uint64_t t0 = nowNanos();
            crc = TxtrStream_crc32(crc, buf + len, got);
            st->nsCrc += nowNanos() - t0;
            len += got;
            left -= got;
        }
        size_t off = 0;
        if (!header) {
            if (len < 12 || memcmp(buf, "fioq", 4) != 0 || (buf[4] | (buf[5] << 8)) != job->width || (buf[6] | (buf[7] << 8)) != job->height) {
                fclose(f); st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache stream is not this page's QOI"); return 0;
            }
            header = true;
            off = 12;
        }
        size_t used = 0;
        uint64_t t0 = nowNanos();
        int rows = bsQoiDecodeRows(&q, buf + off, len - off, &used, rowbuf, job->width,
                                   job->dst + (size_t) y * job->dstStride, job->dstStride, job->height - y, left == 0);
        st->nsQoi += nowNanos() - t0;
        if (rows < 0 || (rows == 0 && used == 0 && left == 0)) {
            fclose(f); st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache stream decode error at row %ld", (long) y); return 0;
        }
        y += rows;
        if (rowsDone) *rowsDone = y;
        off += used;
        memmove(buf, buf + off, len - off);
        len -= off;
    }
    // the rest of the stream (QOI padding) for the CRC
    while (left > 0) {
        size_t want = left < READ_CHUNK ? (size_t) left : READ_CHUNK;
        if (timedRead(buf, want, f, st) != want) { fclose(f); st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache read failed"); return 0; }
        uint64_t t0 = nowNanos();
        crc = TxtrStream_crc32(crc, buf, want);
        st->nsCrc += nowNanos() - t0;
        left -= want;
    }
    fclose(f);
    if (crc != crcWant) { st->cacheState = TXTR_CACHE_DAMAGED; note(st, "cache CRC mismatch"); return 0; }
    st->cacheState = TXTR_CACHE_USED;
    st->source = TXTR_SRC_CACHE;
    st->cacheUsedPath = cachePath;
    return 1;
}

// ===[ decode from the BZ2 blob in data.win, optionally writing the cache ]===
// The blob comes from data.win (dw, at job->blobOffset) or from memory (mem, job->blobSize bytes; no cache then).
static bool decodeFromBlob(const TxtrStreamJob* job, FILE* dw, const uint8_t* mem, const BlobKey* key, volatile int32_t* rowsDone,
                           TxtrStreamStats* st, uint8_t* inBuf, uint8_t* qbuf, uint32_t* rowbuf) {
    size_t hdr = job->gm2022_5 ? 12 : 8;
    if (key->head[0] != '2' || key->head[1] != 'z' || key->head[2] != 'o' || key->head[3] != 'q' ||
        (key->head[4] | (key->head[5] << 8)) != job->width || (key->head[6] | (key->head[7] << 8)) != job->height || job->blobSize <= hdr) {
        note(st, "blob of page %lu is not a matching 2zoq page", (unsigned long) job->pageId);
        return false;
    }
    if (mem == NULL && fseek(dw, (long) (job->blobOffset + hdr), SEEK_SET) != 0) { note(st, "data.win seek failed"); return false; }
    const uint8_t* memNext = mem != NULL ? mem + hdr : NULL;

    FILE* cache = NULL;
    char tmpPath[512];
    if (job->cachePath != NULL && job->writeCache) {
        snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", job->cachePath);
        uint64_t t0 = nowNanos();
        cache = openUnbuffered(tmpPath, "wb");
        if (cache != NULL) {
            uint8_t h[TXTR_CACHE_HEADER_SIZE];
            buildHeader(h, job, key, 0, 0); // size + CRC filled in at the end
            if (fwrite(h, 1, sizeof(h), cache) != sizeof(h)) { fclose(cache); cache = NULL; remove(tmpPath); }
        }
        st->nsCacheWrite += nowNanos() - t0;
        if (cache == NULL) note(st, "can't create %s.tmp", job->cachePath);
    }

    bz_stream bz;
    memset(&bz, 0, sizeof(bz));
    if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) {
        if (cache) { fclose(cache); remove(tmpPath); }
        note(st, "BZ2 init failed");
        return false;
    }
    uint64_t compLeft = job->blobSize - hdr;
    BsQoiState q;
    bsQoiInit(&q);
    uint32_t crc = 0;
    uint64_t streamBytes = 0;
    size_t len = 0;
    bool header = false, streamEnd = false, ok = false, cacheOk = cache != NULL;
    int32_t y = 0;
    const char* err = NULL;
    while (y < job->height || !streamEnd) {
        // refill the QOI window from bzip2
        if (!streamEnd && (len < QOI_CARRY || y >= job->height)) {
            if (bz.avail_in == 0 && compLeft > 0) {
                size_t want = compLeft < READ_CHUNK ? (size_t) compLeft : READ_CHUNK;
                if (memNext != NULL) {
                    bz.next_in = (char*) memNext;
                    memNext += want;
                } else {
                    if (timedRead(inBuf, want, dw, st) != want) { err = "data.win read failed"; break; }
                    bz.next_in = (char*) inBuf;
                }
                compLeft -= want;
                bz.avail_in = (unsigned int) want;
            }
            if (y >= job->height) len = 0; // past the last row: only draining the stream (for the cache and bzip2's CRC check)
            bz.next_out = (char*) qbuf + len;
            bz.avail_out = (unsigned int) (QOI_WINDOW - len);
            uint64_t t0 = nowNanos();
            int rc = BZ2_bzDecompress(&bz);
            st->nsBz2 += nowNanos() - t0;
            size_t produced = (QOI_WINDOW - len) - bz.avail_out;
            if (rc == BZ_STREAM_END) streamEnd = true;
            else if (rc != BZ_OK) { err = "bzip2 data error"; break; }
            else if (produced == 0 && bz.avail_in == 0 && compLeft == 0) { err = "bzip2 stream cut off"; break; }
            if (produced > 0 && cacheOk) {
                uint64_t t1 = nowNanos();
                if (fwrite(qbuf + len, 1, produced, cache) != produced) cacheOk = false;
                crc = TxtrStream_crc32(crc, qbuf + len, produced);
                st->nsCacheWrite += nowNanos() - t1;
            }
            streamBytes += produced;
            len += produced;
        }
        if (y >= job->height) continue;
        size_t off = 0;
        if (!header) {
            if (len < 12) { if (streamEnd) { err = "QOI header cut off"; break; } continue; }
            if (memcmp(qbuf, "fioq", 4) != 0 || (qbuf[4] | (qbuf[5] << 8)) != job->width || (qbuf[6] | (qbuf[7] << 8)) != job->height) {
                err = "blob does not hold this page's QOI";
                break;
            }
            header = true;
            off = 12;
        }
        size_t used = 0;
        uint64_t t0 = nowNanos();
        int rows = bsQoiDecodeRows(&q, qbuf + off, len - off, &used, rowbuf, job->width,
                                   job->dst + (size_t) y * job->dstStride, job->dstStride, job->height - y, streamEnd);
        st->nsQoi += nowNanos() - t0;
        if (rows < 0 || (rows == 0 && used == 0 && streamEnd)) { err = "QOI decode error"; break; }
        y += rows;
        if (rowsDone) *rowsDone = y;
        off += used;
        memmove(qbuf, qbuf + off, len - off);
        len -= off;
    }
    BZ2_bzDecompressEnd(&bz);
    st->streamBytes = streamBytes;
    if (y >= job->height) {
        ok = true; // the page is complete even if the stream's tail was bad (as the original decoder: it never looked)
        st->source = TXTR_SRC_BZ2;
    }
    if (err != NULL) note(st, "%s", err);
    if (cache != NULL) {
        uint64_t t0 = nowNanos();
        bool commit = cacheOk && ok && streamEnd && err == NULL && streamBytes <= 0xFFFFFFFFu;
        if (commit) {
            uint8_t h[TXTR_CACHE_HEADER_SIZE];
            buildHeader(h, job, key, (uint32_t) streamBytes, crc);
            commit = fseek(cache, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof(h), cache) == sizeof(h);
        }
        if (fclose(cache) != 0) commit = false;
        if (commit) {
            remove(job->cachePath); // rename() won't replace an existing file everywhere (the Vita's newlib)
            commit = rename(tmpPath, job->cachePath) == 0;
        }
        if (!commit) {
            remove(tmpPath);
            note(st, "cache not written%s", cacheOk ? "" : " (write failed: card full?)");
        }
        st->cacheWritten = commit;
        st->nsCacheWrite += nowNanos() - t0;
    }
    return ok;
}

bool TxtrStream_decodePage(const TxtrStreamJob* job, volatile int32_t* rowsDone, TxtrStreamStats* st) {
    memset(st, 0, sizeof(*st));
    uint64_t start = nowNanos();
    if (job->width <= 0 || job->height <= 0 || job->dst == NULL || job->dataWinPath == NULL) {
        note(st, "bad job");
        return false;
    }
    uint8_t* inBuf = (uint8_t*) malloc(READ_CHUNK + QOI_CARRY);
    uint8_t* qbuf = (uint8_t*) malloc(QOI_WINDOW + QOI_CARRY);
    uint32_t* rowbuf = (uint32_t*) malloc((size_t) job->width * 4);
    FILE* dw = openUnbuffered(job->dataWinPath, "rb");
    bool ok = false;
    BlobKey key;
    if (inBuf == NULL || qbuf == NULL || rowbuf == NULL) {
        note(st, "out of memory");
    } else if (dw == NULL) {
        note(st, "can't open %s", job->dataWinPath);
    } else if (!readBlobKey(job, dw, &key, st)) {
        note(st, "can't read the blob from %s", job->dataWinPath);
    } else {
        st->cacheState = TXTR_CACHE_OFF;
        // the read-only copy next to data.win first (it ships with that data.win), then the writable one
        if (job->bundleCachePath != NULL) {
            if (decodeFromCache(job, job->bundleCachePath, &key, rowsDone, st, inBuf, rowbuf) == 1) ok = true;
            else {
                if (rowsDone) *rowsDone = 0;
                note(st, "%s not usable (%s)", job->bundleCachePath,
                     st->cacheState == TXTR_CACHE_MISSING ? "missing" : st->cacheState == TXTR_CACHE_STALE ? "stale" : "damaged");
            }
        }
        if (!ok && job->cachePath != NULL) {
            if (decodeFromCache(job, job->cachePath, &key, rowsDone, st, inBuf, rowbuf) == 1) ok = true;
            else if (rowsDone) *rowsDone = 0;
        }
        if (!ok) {
            // a cache that turned out bad is rewritten; one that is merely missing or stale too
            ok = decodeFromBlob(job, dw, NULL, &key, rowsDone, st, inBuf, qbuf, rowbuf);
        }
    }
    if (dw) fclose(dw);
    free(inBuf);
    free(qbuf);
    free(rowbuf);
    __sync_synchronize(); // texture memory written on this thread is read by the GPU after the main thread joins
    st->nsTotal = nowNanos() - start;
    return ok;
}

bool TxtrStream_decodeBz2QoiMemory(const uint8_t* blob, size_t blobSize, bool gm2022_5, int32_t width, int32_t height,
                                   uint8_t* dst, size_t dstStride, TxtrStreamStats* st) {
    memset(st, 0, sizeof(*st));
    uint64_t start = nowNanos();
    if (blob == NULL || blobSize < 16 || blobSize > 0xFFFFFFFFu || width <= 0 || height <= 0 || dst == NULL) { note(st, "bad blob"); return false; }
    TxtrStreamJob job;
    memset(&job, 0, sizeof(job));
    job.blobSize = (uint32_t) blobSize;
    job.gm2022_5 = gm2022_5;
    job.width = width;
    job.height = height;
    job.dst = dst;
    job.dstStride = dstStride;
    BlobKey key;
    memcpy(key.head, blob, sizeof(key.head));
    memset(key.tail, 0, sizeof(key.tail));
    key.sampleCrc = 0;
    uint8_t* qbuf = (uint8_t*) malloc(QOI_WINDOW + QOI_CARRY);
    uint32_t* rowbuf = (uint32_t*) malloc((size_t) width * 4);
    bool ok = false;
    if (qbuf == NULL || rowbuf == NULL) note(st, "out of memory");
    else ok = decodeFromBlob(&job, NULL, blob, &key, NULL, st, NULL, qbuf, rowbuf);
    free(qbuf);
    free(rowbuf);
    st->nsTotal = nowNanos() - start;
    return ok;
}
