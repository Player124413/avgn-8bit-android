#ifndef _BS_GL_RENDERER_H_
#define _BS_GL_RENDERER_H_

#include "common.h"
#include "gl_common.h"
#include "renderer.h"
#include "runner.h"
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__SWITCH__)
#include <GLES3/gl3.h>
#elif PLATFORM_VITA
#include <vitaGL.h>
#define GL_BOOL 0x8B56
#else
#include <glad/glad.h>
#endif

typedef enum {
    BATCHTYPE_QUAD,
    BATCHTYPE_TRIANGLE
} BatchType;

// ===[ GLRenderer Struct ]===
typedef struct {
    char* name; // owned
    int32_t location;
    GLenum type;
    uint32_t samplerSlot;
} GLShaderUniform;

typedef struct {
    GLuint shaderId;
    bool compiled;
    uint32_t uniformCount;
    GLShaderUniform* uniforms;

    // cached uniforms
    GLShaderUniform* gmBaseTexture;
    GLShaderUniform* gmMatrices;
    GLShaderUniform* gmFogColour;
    GLShaderUniform* gmAlphaTestEnabled;
    GLShaderUniform* gmAlphaRefValue;

    bool vitaGrid; // Vita CRT rewrite with per-vertex terms: glDrawSurface draws it as a 64x56 grid
    struct VitaCrtBake* vitaBake; // Vita CRT rewrite with baked terms (gl_renderer.c "CRT bake"), else NULL
} GMLShader;

typedef struct GLModernRenderer {
    GLRenderer base; // Must be first field for struct embedding
    
    GMLShader* defaultShaderProgram;
    GMLShader* fastDefaultProgram;   // Vita: default program without fog / alpha test / local UV, nullptr if absent
    GLint fastDefaultWVP;            // its uWorldViewProjection
    GMLShader* gmlShaders;
    uint32_t gmlShaderCount;

    bool fogEnable;
    uint32_t fogColor; // BGR
    float fogStart, fogEnd;

    GLuint vao, vertexBufferVao, vbo, ebo;

    BatchType batchType;
    int32_t batchCount;
    GLuint currentTextureId;

    GLuint hostFramebuffer; // present target for the composited frame, where 0 == the window
    GLuint* surfaceDepth;
    GlVertex* primitiveVertices;
    size_t primitiveVertexCapacity;
    bool primitiveLocalUV, batchLocalUV;
    float primitiveUVRect[4], batchUVRect[4];
    bool depthTest, depthWrite, cull;
    GLenum frontFace;
    bool savedDepthTest, savedDepthWrite, savedCull;
    bool guiActive;
    Matrix4f savedGUIWorld;
    GLint uTextureRect, uLocalUV;

    bool isGL3; // TRUE if running on OpenGL (ES) 3.x+
    bool isGLES;  // TRUE if running on OpenGL ES (GLES)

    // Cached default shader uniforms
    GLShaderUniform* uWorldViewProjection;
    GLShaderUniform* uWorldView;
    GLShaderUniform* uFogRange;
    GLShaderUniform* uFogColor;
    GLShaderUniform* uAlphaTestRef;
    GLShaderUniform* uAlphaTestEnabled;
    GLShaderUniform* uTexture;

    // shdPalettizer fast path (GLES only, see initFastPalettizer): the game's full-screen palette pass does a
    // 56-colour nearest search per pixel; this replaces it with a lookup in a table built by the same search.
    int32_t palettizerIndex;            // shader index of shdPalettizer, -1 when absent or disabled
    GMLShader fastPalettizer;
    uint32_t palPaletteSlot, palLutSlot;
    GLuint stageTexture[MAX_TEXTURE_STAGES + 1]; // last texture texture_set_stage bound per unit
    struct { GLuint palette, lut, color; bool filter; } palLuts[4]; // color: Vita colour table (0 = none)
    GMLShader colorPalettizer;          // Vita: shdPalettizer through the colour table (see "colour table")
    bool colorPalettizerOk;
    int32_t palLutCount;
} GLModernRenderer;

bool GLRenderer_ensureTextureLoaded(GLRenderer* gl, uint32_t pageId);
#ifdef PLATFORM_VITA
// Boot (loop.c): wait for the big texture pages decoding on worker threads (animating the boot indicator), and
// refresh the batch buffer after the indicator's frames.
void GLRenderer_vitaFinishPrefetch(GLRenderer* gl);
void GLRenderer_vitaAfterBootIndicator(GLRenderer* gl);
#endif
Renderer* GLRenderer_create(void);

#endif /* _BS_GL_RENDERER_H_ */
