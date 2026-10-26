// font_add() support: rasterises a TrueType/OpenType font with FreeType into a glyph atlas texture and
// registers it as a regular (non-sprite) font. GameMaker sizes runtime fonts in points at 96 DPI.
#include "font_ttf.h"

#ifdef BS_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>
#include "runner.h"
#include "renderer.h"
#include "file_system.h"
#include "data_win.h"
#include "utils.h"
#include "log.h"
#include "gettime.h"
#include "boot_progress.h"

static FT_Library ftLibrary = nullptr;

typedef struct {
    uint32_t codepoint;
    int32_t bitmapW, bitmapH, bitmapLeft, bitmapTop, advance;
    uint8_t* alpha;   // bitmapW * bitmapH coverage (0 or 255 for mono rendering)
    int32_t atlasX, atlasY, cellW;
} RasterGlyph;

static bool wantCodepoint(FT_Face face, uint32_t cp) {
    return FT_Get_Char_Index(face, cp) != 0;
}

int32_t FontTTF_add(Runner* runner, const char* path, float size, bool bold, bool italic, int32_t first, int32_t last) {
    Renderer* renderer = runner->renderer;
    if (renderer == nullptr || renderer->vtable->createTpagFromPixels == nullptr) return -1;
    uint64_t startNs = nowNanos();
    if (ftLibrary == nullptr && FT_Init_FreeType(&ftLibrary) != 0) {
        logWarn("font_add: FreeType init failed\n");
        return -1;
    }

    uint8_t* fileData = nullptr;
    int32_t fileSize = 0;
    FileSystem* fs = runner->fileSystem;
    if (!fs->vtable->readFileBinary(fs, path, &fileData, &fileSize) || fileSize <= 0) {
        logWarn("font_add: cannot read '%s'\n", path);
        return -1;
    }
    uint64_t readNs = nowNanos() - startNs;

    FT_Face face;
    if (FT_New_Memory_Face(ftLibrary, fileData, fileSize, 0, &face) != 0) {
        logWarn("font_add: '%s' is not a usable font\n", path);
        free(fileData);
        return -1;
    }
    FT_Set_Char_Size(face, 0, (FT_F26Dot6) (size * 64.0f), 96, 96);
    int32_t ascent = (int32_t) (face->size->metrics.ascender >> 6);
    int32_t descent = (int32_t) (-(face->size->metrics.descender >> 6));
    int32_t lineH = ascent + descent;
    if (lineH <= 0) lineH = (int32_t) (size * 96.0f / 72.0f + 0.5f);

    // Requested range plus the Latin/Cyrillic/punctuation blocks the game's translations use.
    if (first < 0) first = 32;
    if (last < first) last = 127;
    static const uint32_t extraRanges[][2] = {
        {0xA0, 0x17F}, {0x400, 0x45F}, {0x2010, 0x2027}, {0x20AC, 0x20AC},
    };
    uint32_t cap = 1024, count = 0;
    RasterGlyph* glyphs = (RasterGlyph*) calloc(cap, sizeof(RasterGlyph));
    for (int pass = -1; pass < (int) (sizeof(extraRanges) / sizeof(extraRanges[0])); pass++) {
        uint32_t lo = pass < 0 ? (uint32_t) first : extraRanges[pass][0];
        uint32_t hi = pass < 0 ? (uint32_t) last : extraRanges[pass][1];
        for (uint32_t cp = lo; cp <= hi && cp <= 0xFFFF; cp++) {
            if ((cp & 31) == 0) bsBootTick(); // the fonts are rasterised during the boot (rmInitialize)
            if (pass >= 0 && cp >= (uint32_t) first && cp <= (uint32_t) last) continue;
            if (cp != 32 && !wantCodepoint(face, cp)) continue;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME) != 0) continue;
            FT_GlyphSlot slot = face->glyph;
            FT_Bitmap* bm = &slot->bitmap;
            if (count == cap) { cap *= 2; glyphs = (RasterGlyph*) realloc(glyphs, cap * sizeof(RasterGlyph)); }
            RasterGlyph* g = &glyphs[count++];
            memset(g, 0, sizeof(*g));
            g->codepoint = cp;
            g->bitmapW = (int32_t) bm->width;
            g->bitmapH = (int32_t) bm->rows;
            g->bitmapLeft = slot->bitmap_left;
            g->bitmapTop = slot->bitmap_top;
            g->advance = (int32_t) (slot->advance.x >> 6);
            if (g->bitmapW > 0 && g->bitmapH > 0) {
                g->alpha = (uint8_t*) malloc((size_t) g->bitmapW * g->bitmapH);
                for (int32_t y = 0; y < g->bitmapH; y++) {
                    const uint8_t* row = bm->buffer + y * bm->pitch;
                    for (int32_t x = 0; x < g->bitmapW; x++) {
                        uint8_t v = bm->pixel_mode == FT_PIXEL_MODE_MONO ? ((row[x >> 3] >> (7 - (x & 7))) & 1 ? 255 : 0)
                                                                          : row[x];
                        g->alpha[y * g->bitmapW + x] = v;
                    }
                }
            }
        }
    }
    FT_Done_Face(face);
    free(fileData);
    if (count == 0) { free(glyphs); return -1; }

    // Shelf-pack full-height cells (GameMaker glyph rects span the whole line height).
    const int32_t atlasW = 512, pad = 1;
    int32_t x = 0, y = 0;
    for (uint32_t i = 0; i < count; i++) {
        RasterGlyph* g = &glyphs[i];
        g->cellW = g->bitmapW > 0 ? g->bitmapW : 1;
        if (x + g->cellW + pad > atlasW) { x = 0; y += lineH + pad; }
        g->atlasX = x; g->atlasY = y;
        x += g->cellW + pad;
    }
    int32_t usedH = y + lineH + pad, atlasH = 8;
    while (atlasH < usedH) atlasH <<= 1;

    uint8_t* rgba = (uint8_t*) calloc((size_t) atlasW * atlasH, 4);
    for (uint32_t i = 0; i < count; i++) {
        RasterGlyph* g = &glyphs[i];
        int32_t top = ascent - g->bitmapTop;
        for (int32_t gy = 0; gy < g->bitmapH; gy++) {
            int32_t ty = g->atlasY + top + gy;
            if (ty < g->atlasY || ty >= g->atlasY + lineH || ty >= atlasH) continue;
            for (int32_t gx = 0; gx < g->bitmapW; gx++) {
                uint8_t* p = rgba + ((size_t) ty * atlasW + g->atlasX + gx) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = g->alpha[gy * g->bitmapW + gx];
            }
        }
    }
    int32_t tpagIndex = renderer->vtable->createTpagFromPixels(renderer, rgba, atlasW, atlasH);
    free(rgba);
    if (tpagIndex < 0) {
        for (uint32_t i = 0; i < count; i++) free(glyphs[i].alpha);
        free(glyphs);
        return -1;
    }

    DataWin* dw = runner->dataWin;
    uint32_t fontIndex = dw->font.count;
    dw->font.count++;
    dw->font.fonts = (Font*) safeRealloc(dw->font.fonts, dw->font.count * sizeof(Font));
    Font* font = &dw->font.fonts[fontIndex];
    memset(font, 0, sizeof(*font));
    const char* base = strrchr(path, '/');
    font->present = true;
    font->name = safeStrdup(base ? base + 1 : path);
    font->displayName = font->name;
    font->emSize = size;
    font->bold = bold;
    font->italic = italic;
    font->rangeStart = (uint16_t) first;
    font->rangeEnd = (uint32_t) last;
    font->tpagIndex = tpagIndex;
    font->scaleX = 1.0f;
    font->scaleY = 1.0f;
    font->ascender = (uint32_t) ascent;
    font->hasAscender = true;
    font->lineHeight = (uint32_t) lineH;
    font->hasLineHeight = true;
    font->maxGlyphHeight = (uint32_t) lineH;
    font->spriteIndex = -1;
    font->glyphCount = count;
    font->glyphs = (FontGlyph*) safeCalloc(count, sizeof(FontGlyph));
    for (uint32_t i = 0; i < count; i++) {
        RasterGlyph* g = &glyphs[i];
        FontGlyph* fg = &font->glyphs[i];
        fg->character = (uint16_t) g->codepoint;
        fg->sourceX = (uint16_t) g->atlasX;
        fg->sourceY = (uint16_t) g->atlasY;
        fg->sourceWidth = (uint16_t) (g->bitmapW > 0 ? g->bitmapW : 0);
        fg->sourceHeight = (uint16_t) lineH;
        fg->shift = (int16_t) g->advance;
        fg->offset = (int16_t) g->bitmapLeft;
        free(g->alpha);
    }
    free(glyphs);
    Font_buildGlyphLUT(font);
    logInfo("font_add: '%s' %.1fpt -> font %u (%u glyphs, line %d, atlas %dx%d) in %.1f ms (file read %.1f ms, %d KiB)\n", path, size, fontIndex, count, lineH, atlasW, atlasH,
            (double) (nowNanos() - startNs) / 1e6, (double) readNs / 1e6, (int) (fileSize >> 10));
    return (int32_t) fontIndex;
}

#else

int32_t FontTTF_add(Runner* runner, const char* path, float size, bool bold, bool italic, int32_t first, int32_t last) {
    (void) runner; (void) path; (void) size; (void) bold; (void) italic; (void) first; (void) last;
    return -1;
}

#endif
