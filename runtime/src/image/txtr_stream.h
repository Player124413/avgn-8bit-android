#ifndef _BS_TXTR_STREAM_H_
#define _BS_TXTR_STREAM_H_

// Big BZ2+QOI texture page -> RGBA8 memory (the Vita's texture memory), as a self-contained job that can run on a
// worker thread: it opens its own handles (data.win, the cache file) and touches nothing else of the runner.
//
// Texture cache: the page's QOI stream (what the BZ2 blob decompresses to, ~13.6 MB for the 4096x4096 page) is
// kept in a file next to data.win. Decoding it skips bzip2, which is ~90% of the page's decode time (on hardware
// the whole page took ~14.5 s). The file is written by tools/make_txtr_cache.py at deploy time, or by this code
// after a boot that had to use the BZ2 blob (a read-only copy next to data.win, e.g. in the VPK, can be tried first:
// bundleCachePath). It is keyed on data.win: page id, size, blob offset/size, data.win's
// size, the blob's first 16 and last 32 bytes (the tail holds bzip2's CRC of the whole stream) and a CRC of 8 samples
// from the middle of the blob; a stream
// CRC-32 and the file size catch a damaged or cut-off file, which then falls back to the BZ2 blob (and is rewritten).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TXTR_CACHE_MAGIC "BSTXC001"
#define TXTR_CACHE_HEADER_SIZE 128

// The data.win the runner loaded (set by loop.c before the renderer starts): prefetch jobs reopen it.
extern const char* TxtrStream_dataWinPath;

typedef struct {
    const char* dataWinPath;   // the job reopens data.win itself (the runner's FILE* belongs to the main thread)
    uint64_t dataWinSize;      // 0: not part of the cache key
    uint32_t pageId;
    uint32_t blobOffset;       // the page's "2zoq" blob in data.win
    uint32_t blobSize;
    bool gm2022_5;             // 12-byte 2zoq header (else 8)
    int32_t width, height;     // expected page size (TXTR entry); the blob must match
    uint8_t* dst;              // RGBA8 rows, dstStride bytes apart
    size_t dstStride;
    const char* cachePath;     // nullptr: no cache
    bool writeCache;           // (re)write cachePath after a BZ2 decode
    const char* bundleCachePath; // read-only cache tried before cachePath (the Vita's app0:, next to data.win), never
                                 // written; nullptr: none. A BZ2 decode then (re)writes cachePath.
} TxtrStreamJob;

enum {
    TXTR_SRC_NONE = 0,
    TXTR_SRC_CACHE = 1,
    TXTR_SRC_BZ2 = 2,
};

enum {
    TXTR_CACHE_OFF = 0,        // no cache path
    TXTR_CACHE_USED = 1,
    TXTR_CACHE_MISSING = 2,
    TXTR_CACHE_STALE = 3,      // key mismatch (another data.win)
    TXTR_CACHE_DAMAGED = 4,    // size / CRC / decode error
};

typedef struct {
    int source;                // TXTR_SRC_*
    int cacheState;            // TXTR_CACHE_*
    bool cacheWritten;
    const char* cacheUsedPath; // the cache file the page came from (job->bundleCachePath or job->cachePath), or nullptr
    uint64_t nsTotal;
    uint64_t nsRead;           // file reads (cache stream or BZ2 blob)
    uint64_t nsBz2;            // BZ2_bzDecompress
    uint64_t nsQoi;            // QOI decode + row copies into dst
    uint64_t nsCrc;
    uint64_t nsCacheWrite;     // writing the cache file (BZ2 path)
    uint64_t bytesRead;
    uint64_t streamBytes;      // QOI stream size
    char note[192];            // why the cache wasn't used / written, or the error
} TxtrStreamStats;

// Decodes the page into job->dst. rowsDone (optional) counts finished rows for progress display.
// Returns false if neither the cache nor the BZ2 blob produced the page (dst is then undefined).
bool TxtrStream_decodePage(const TxtrStreamJob* job, volatile int32_t* rowsDone, TxtrStreamStats* stats);

// Same decode from a blob already in memory (no cache). For ImageDecoder_streamBz2QoiInto.
bool TxtrStream_decodeBz2QoiMemory(const uint8_t* blob, size_t blobSize, bool gm2022_5, int32_t width, int32_t height,
                                   uint8_t* dst, size_t dstStride, TxtrStreamStats* stats);

// CRC-32 (zlib's polynomial), as Python's zlib.crc32; start with crc = 0.
uint32_t TxtrStream_crc32(uint32_t crc, const uint8_t* data, size_t len);

#endif /* _BS_TXTR_STREAM_H_ */
