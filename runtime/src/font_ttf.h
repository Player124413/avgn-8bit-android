#ifndef _BS_FONT_TTF_H_
#define _BS_FONT_TTF_H_
#include "common.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct Runner Runner;

// font_add(name, size, bold, italic, first, last): rasterise a TrueType font into a new font index.
// Returns -1 when FreeType support is not compiled in (BS_HAVE_FREETYPE) or the file can't be used.
int32_t FontTTF_add(Runner* runner, const char* path, float size, bool bold, bool italic, int32_t first, int32_t last);

#endif /* _BS_FONT_TTF_H_ */
