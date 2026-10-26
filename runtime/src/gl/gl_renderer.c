#include "gl_renderer.h"
#include "vita/vita_paths.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "runner.h"
#include "file_system.h"

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__SWITCH__)
#include <GLES3/gl3.h>
#elif PLATFORM_VITA
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include "vita_textures.h"
#else
#include <glad/glad.h>
#endif
#include <ctype.h>
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "math_compat.h"

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"
#include "image_decoder.h"
#include "gl_common.h"
#include "gl_wrappers.h"
#include "gettime.h"
#include "vm_builtins.h"
#include "boot_progress.h"
#ifdef PLATFORM_VITA
#include "txtr_stream.h"
#endif

// Port profiling: per-frame GL call counters (reported by the loop's PERF line)
extern uint32_t bsGlDraws, bsGlFbo, bsGlProg, bsGlClears, bsGlTex, bsGlRead;
void bsTrackBind(uint32_t fbo);
void bsTrackWork(void);
#ifdef PLATFORM_VITA
// GPU profiler ("GPU profiler (Vita)" below): true only inside a sampled frame; each draw/clear reports to it first.
// BS_GPUPROF / BS_GPUPROF_PROG pass their argument through (evaluated once) and run before the GL call itself.
extern bool bsGpuProfOn;
void bsGpuProfWork(int kind, uint32_t count);
static GLuint gpuProfProgram; // last program bound with glUseProgram (labels the profiler's segments)
static inline uint32_t bsGpuProfArg(int kind, uint32_t v) { if (bsGpuProfOn) bsGpuProfWork(kind, v); return v; }
#define BS_GPUPROF(kind, v) bsGpuProfArg(kind, (uint32_t) (v))
#define BS_GPUPROF_PROG(p) (gpuProfProgram = (p))
#else
#define BS_GPUPROF(kind, v) (v)
#define BS_GPUPROF_PROG(p) (p)
#endif
#define glDrawArrays(mode, first, count) (bsGlDraws++, bsTrackWork(), glDrawArrays(mode, first, BS_GPUPROF(1, count)))
#define glDrawElements(mode, count, type, indices) (bsGlDraws++, bsTrackWork(), glDrawElements(mode, BS_GPUPROF(0, count), type, indices))
#define glBindFramebuffer(target, fbo) (bsGlFbo++, bsTrackBind((uint32_t) (fbo)), glBindFramebuffer(target, fbo))
#define glUseProgram(p) (bsGlProg++, glUseProgram(BS_GPUPROF_PROG(p)))
#define glClear(mask) (bsGlClears++, bsTrackWork(), glClear(BS_GPUPROF(2, mask)))
#define glTexImage2D(...) (bsGlTex++, glTexImage2D(__VA_ARGS__))
#define glReadPixels(...) (bsGlRead++, glReadPixels(__VA_ARGS__))

// ===[ Constants ]===
#define MAX_QUADS 4096
#define VERTICES_PER_TRIANGLE 3
#define VERTICES_PER_QUAD 4
#define INDICES_PER_QUAD 6

// ===[ Shader Sources ]===

static const char* baseVertexShader =
    "uniform mat4 uWorldViewProjection;\n"
    "uniform mat4 uWorldView;\n"
    "uniform vec2 uFogRange;\n"
    "uniform vec4 uFogColor;\n"
    "void main() {\n"
    "    gl_Position = uWorldViewProjection * vec4(aPos, 1.0);\n"
    // GML exposes Direct3D-style clip Z; OpenGL clips against [-w, w].
    "    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "    float depth = (uWorldView * vec4(aPos, 1.0)).z;\n"
    "    vFogFactor = (1.0 - (uFogRange.y - depth) * uFogRange.x) * uFogColor.a;\n"
    "}\n";

static const char* baseFragmentShader =
    "uniform sampler2D uTexture;\n"
    "uniform float uAlphaTestRef;\n"
    "uniform bool uAlphaTestEnabled;\n"
    "uniform vec4 uFogColor;\n"
    "uniform vec4 uTextureRect;\n"
    "uniform bool uLocalUV;\n"
    "void main() {\n"
    "    vec2 uv = uLocalUV ? uTextureRect.xy + fract(vTexCoord) * uTextureRect.zw : vTexCoord;\n"
    "    vec4 c = TEXTURE_2D(uTexture, uv) * vColor;\n"
    "    if (uAlphaTestEnabled) {\n"
    "        if (uAlphaTestRef >= c.a) discard;\n"
    "    }\n"
    "    c.rgb = mix(c.rgb, uFogColor.rgb, clamp(vFogFactor, 0.0, 1.0));\n"
    "    FRAG_COLOR = c;\n"
    "}\n";

// ===[ Runtime OpenGL extension checks ]===

static bool hasFBO() {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__VITA__) && !defined(__SWITCH__)
    return glGenFramebuffers;
#else
    return true;
#endif
}

static bool hasVAO() {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__VITA__) && !defined(__SWITCH__)
    return glGenVertexArrays;
#else
    return true;
#endif
}

// ===[ Shader Compilation ]===

#ifdef PLATFORM_VITA
// Malloc'd copy of src with every occurrence of `from` replaced by `to`; adds the number of replacements to *count.
static char* vitaReplaceAll(const char* src, const char* from, const char* to, int* count) {
    size_t fromLen = strlen(from), toLen = strlen(to), n = 0;
    for (const char* p = strstr(src, from); p != nullptr; p = strstr(p + fromLen, from)) n++;
    char* out = (char*) safeMalloc(strlen(src) + n * (toLen > fromLen ? toLen - fromLen : 0) + 1);
    char* o = out;
    const char* s = src;
    for (const char* p = strstr(s, from); p != nullptr; p = strstr(s, from)) {
        memcpy(o, s, (size_t) (p - s));
        o += p - s;
        memcpy(o, to, toLen);
        o += toLen;
        s = p + fromLen;
    }
    strcpy(o, s);
    *count += (int) n;
    return out;
}

// replaces every instance of (thing *= blahblah) with (thing = thing * (blahblah).)
// vitaGL doesn't support the operator *= so...
// this assumes that every line ends with ; so its a bit finicky
char *fixShaderForVita(const char *src) {
    size_t src_len = strlen(src);
    size_t cap = src_len * 2 + 64;
    char *out = malloc(cap);
    size_t out_len = 0;
    if (!out) return NULL;

    #define ENSURE(n) do { \
    if (out_len + (size_t)(n) + 1 > cap) { \
        cap = (out_len + (size_t)(n) + 1) * 2; \
        char *tmp = realloc(out, cap); \
        if (!tmp) { free(out); return NULL; } \
            out = tmp; \
    } \
    } while (0)

    #define APPEND(s, n) do { ENSURE(n); memcpy(out + out_len, (s), (n)); out_len += (n); } while (0)
    #define APPEND_STR(s) APPEND((s), strlen(s))
    #define APPEND_CH(c) do { ENSURE(1); out[out_len++] = (char)(c); } while (0)

    size_t i = 0;
    while (i < src_len) {
        char c = src[i];
        if (c == '/' && i + 1 < src_len && src[i + 1] == '/') {
            size_t start = i;
            while (i < src_len && src[i] != '\n') i++;
            APPEND(src + start, i - start);
            continue;
        }

        if (c == '/' && i + 1 < src_len && src[i + 1] == '*') {
            size_t start = i;
            i += 2;
            while (i + 1 < src_len && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = (i + 1 < src_len) ? i + 2 : src_len;
            APPEND(src + start, i - start);
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            size_t id_start = i;
            size_t j = i;

            while (j < src_len && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;

            size_t k = j;
            for (;;) {
                size_t save = k;
                while (k < src_len && isspace((unsigned char)src[k])) k++;

                if (k < src_len && src[k] == '.') {
                    size_t after_dot = k + 1;
                    while (after_dot < src_len && isspace((unsigned char)src[after_dot])) after_dot++;
                    if (after_dot < src_len && (isalpha((unsigned char)src[after_dot]) || src[after_dot] == '_')) {
                        k = after_dot;
                        while (k < src_len && (isalnum((unsigned char)src[k]) || src[k] == '_')) k++;
                        continue;
                    }
                    k = save;
                    break;
                } else if (k < src_len && src[k] == '[') {
                    int depth = 1;
                    size_t m = k + 1;
                    while (m < src_len && depth > 0) {
                        if (src[m] == '[') depth++;
                        else if (src[m] == ']') depth--;
                        m++;
                    }
                    if (depth == 0) { k = m; continue; }
                    k = save;
                    break;
                } else {
                    k = save;
                    break;
                }
            }

            size_t m = k;
            while (m < src_len && isspace((unsigned char)src[m])) m++;

            if (m + 1 < src_len && src[m] == '*' && src[m + 1] == '=' &&
                !(m + 2 < src_len && src[m + 2] == '=')) {

                size_t rhs_start = m + 2;
            size_t p = rhs_start;
            int depth = 0;
            while (p < src_len) {
                char rc = src[p];
                if (rc == '(' || rc == '[') depth++;
                else if (rc == ')' || rc == ']') depth--;
                else if (rc == ';' && depth == 0) break;
                p++;
            }

            if (p < src_len && src[p] == ';') {
                size_t rhs_end = p;
                while (rhs_end > rhs_start && isspace((unsigned char)src[rhs_end - 1])) rhs_end--;
                size_t rhs_s = rhs_start;
                while (rhs_s < rhs_end && isspace((unsigned char)src[rhs_s])) rhs_s++;

                APPEND(src + id_start, k - id_start);   /* lvalue */
                APPEND_STR(" = ");
                APPEND(src + id_start, k - id_start);   /* lvalue again */
                APPEND_STR(" * (");
                APPEND(src + rhs_s, rhs_end - rhs_s);   /* expr */
                APPEND_CH(')');
                APPEND_CH(';');

                i = p + 1;
                continue;
            }
                }
        }

        APPEND_CH(c);
        i++;
    }

    ENSURE(0);
    out[out_len] = '\0';
    return out;

    #undef ENSURE
    #undef APPEND
    #undef APPEND_STR
    #undef APPEND_CH
}
#endif

#ifdef PLATFORM_VITA
// ===[ CRT filter rewrites (Vita) ]===
// objGameRenderer.mthRenderCRT draws BjortFX's CRT passes at 512x448: shd_retroscreen_screenfilter (CRT type 1+),
// shd_retroscreen_distortion (type 2+), shd_retroscreen_postprocessing (type 3). As written they cost about 25, 10
// and 20+ ms of GPU time per frame on the Vita (all fp32: vitaGL ignores mediump; sin/cos/pow are instruction
// sequences on SGX; 5 + 4 + 17 dependent texture reads). Each rewrite keeps the GameMaker header of the original
// sources (gm_* uniforms, in_* attributes, v_vTexcoord / v_vColour) and replaces what follows it: the vertex shader
// from "void main()", the fragment shader from "#define _YY_GLSLES_ 1". Uniform and sampler names the game sets are
// unchanged; ones a rewrite no longer reads become inactive (shader_get_uniform -> -1, setting it is a no-op).
// CRT_GRID (screen filter, distortion): glDrawSurface draws the pass as a 64x56 grid and the smooth terms are
// evaluated per vertex. <data dir>/nocrtfast disables all rewrites, <data dir>/nocrtgrid only the grid,
// <data dir>/nocrthalfglow only the half-res glow of CRT type 3, <data dir>/nocrtmerge the merged passes (no Pass2
// copy; type 3: glow composite in the final stretch, no Pass5), <data dir>/nocrtfinalglow only the latter.
// A rewrite is compiled before (and instead of) the SceShaccCg sign-bug patch below; if SceShaccCg rejects it, the
// original sources (with that patch) are compiled as before. See the shader loop for why that needs
// VGL_MODE_SHADER_PAIR.

#define CRT_SF_SMOOTH \
    "// smooth terms of the screen filter: x = sampleRainbowtint()'s plasma value, y/z = static red/blue tint,\n" \
    "// w = film grain amplitude mix(0.05, uFilmgrainIntensity, smoothstep(0.5, 0.7, distance to the centre)).\n" \
    "// Centre offsets only ever as x - 0.5: SceShaccCg drops a sign when both x - 0.5 and 0.5 - x appear (crtFix).\n" \
    "vec4 crtSmoothTerms(vec2 uv)\n" \
    "{\n" \
    "    vec2 ruv = uv;\n" \
    "    ruv.x = ruv.x + sin(ruv.y * 6.28318530718 + uTime) * 0.1;\n" \
    "    ruv.y = ruv.y + cos(ruv.x * 9.42477796077 + 1.57079632679 + uTime) * 0.1;\n" \
    "    ruv = ruv * 10.0;\n" \
    "    vec4 waves = sin(vec4(ruv.x, ruv.y, ruv.x + ruv.y, length(ruv - vec2(0.5))) + uTime);\n" \
    "    vec2 c = uv - vec2(0.5);\n" \
    "    return vec4(waves.x + waves.y + waves.z + waves.w,\n" \
    "                mix(0.85, 1.1, uv.y),\n" \
    "                mix(0.95, 1.2, abs(c.x) * 2.0),\n" \
    "                mix(0.05, uFilmgrainIntensity, smoothstep(0.5, 0.7, length(c))));\n" \
    "}\n"

static const char* crtScreenfilterVS =
    "uniform float uTime;\n"
    "uniform float uFilmgrainIntensity;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "varying vec2 v_vNoiseA;\n"
    "varying vec2 v_vNoiseB;\n"
    "varying vec2 v_vNoiseC;\n"
    "#ifdef CRT_GRID\n"
    "uniform vec2 uShadowmaskTexelSize;\n"
    "varying vec2 v_vMaskUV;\n"
    "varying vec4 v_vSmooth;\n"
    CRT_SF_SMOOTH
    "#endif\n"
    "void main()\n"
    "{\n"
    "    vec4 object_space_pos = vec4( in_Position.x, in_Position.y, in_Position.z, 1.0);\n"
    "    gl_Position = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * object_space_pos;\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "    // noise() coordinates are affine in uv: plain varyings make them non-dependent reads. The 64x64 noise page is a\n"
    "    // power-of-two texture (GL_REPEAT), so the wrap does the fract(); the time offset is wrapped here, which keeps\n"
    "    // the coordinates small.\n"
    "    vec2 noiseScale = uNoiseTexelSize / uScreenTexelSize;\n"
    "    vec2 noiseOffset = vec2(fract(uTime * 32.0));\n"
    "    v_vNoiseA = in_TextureCoord * noiseScale + noiseOffset;\n"
    "    v_vNoiseB = in_TextureCoord.yx * noiseScale + noiseOffset;\n"
    "    v_vNoiseC = vec2(1.0 - in_TextureCoord.x, in_TextureCoord.y) * noiseScale + noiseOffset;\n"
    "#ifdef CRT_GRID\n"
    "    // sampleShadowmask(): its coordinate at every pixel centre is exactly a mask texel edge, so the original's texel\n"
    "    // choice depends on rounding, which the grid's smaller triangles change. +0.5 texel takes the texel exact\n"
    "    // arithmetic gives, the same in every cell (the single-quad variant keeps the original expression).\n"
    "    v_vMaskUV = in_TextureCoord * (uShadowmaskTexelSize / uScreenTexelSize) * 6.0 + uShadowmaskTexelSize * 0.5;\n"
    "    v_vSmooth = crtSmoothTerms(in_TextureCoord);\n"
    "#endif\n"
    "}\n";

static const char* crtScreenfilterFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_screenfilter, Vita rewrite (gl_renderer.c \"CRT filter rewrites\")\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "varying vec2 v_vNoiseA;\n"
    "varying vec2 v_vNoiseB;\n"
    "varying vec2 v_vNoiseC;\n"
    "uniform float uShadowmaskIntensity;\n"
    "uniform float uScanlineIntensity;\n"
    "uniform float uTintIntensity;\n"
    "uniform float uVignetteIntensity;\n"
    "uniform float uBrightness;\n"
    "uniform float uBleedIntensity;\n"
    "uniform float uBleedSize;\n"
    "uniform float uColorLimit;\n"
    "uniform sampler2D uTexNoise;\n"
    "uniform sampler2D uTexShadowmask;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "#ifdef CRT_GRID\n"
    "varying vec2 v_vMaskUV;\n"
    "varying vec4 v_vSmooth;\n"
    "#else\n"
    "uniform vec2 uShadowmaskTexelSize;\n"
    "uniform float uTime;\n"
    "uniform float uFilmgrainIntensity;\n"
    CRT_SF_SMOOTH
    "#endif\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "#ifdef CRT_GRID\n"
    "    vec4 sm = v_vSmooth;\n"
    "#else\n"
    "    vec4 sm = crtSmoothTerms(uv);\n"
    "#endif\n"
    "    vec3 col = floor((v_vColour.rgb * texture2D(gm_BaseTexture, uv).rgb) * uColorLimit + 0.5) / uColorLimit;\n"
    "    float dithernoise = texture2D(uTexNoise, v_vNoiseA).r;\n"
    "    // colour bleed: neighbour at -uBleedSize * (noise - 0.3) texels\n"
    "    vec3 neighbor = texture2D(gm_BaseTexture, vec2(uv.x + uScreenTexelSize.x * uBleedSize * (0.3 - dithernoise), uv.y)).rgb;\n"
    "    float luma = (col.r + col.g + col.b) / 3.0;\n"
    "    vec3 bleedmix = smoothstep(vec3(0.4, 0.5, 0.3), vec3(0.9), neighbor) * (smoothstep(1.0, 0.0, luma) * 0.8 + 0.2);\n"
    "    col = clamp(col + neighbor * bleedmix * uBleedIntensity * vec3(1.0, 0.8, 0.9), 0.0, 1.0);\n"
    "    // static tint, then the plasma tint (sin(p + PI) == -sin(p), sin(p + PI / 2) == cos(p)). The minus sits in a\n"
    "    // constant, not on sc.x: SceShaccCg has lost the sign of a negated value that is also used unnegated.\n"
    "    col = col * vec3(sm.y, 1.0, sm.z);\n"
    "    vec2 sc = sin(vec2(sm.x, sm.x + 1.57079632679));\n"
    "    col = col * mix(vec3(1.0), vec3(sc.x, sc.x, sc.y) * vec3(0.5, -0.5, 0.5) + 0.5, uTintIntensity);\n"
    "    // scanlines and shadow mask\n"
    "    float scanline = mix(1.0 - uScanlineIntensity, 1.0, abs(fract(uv.y * (0.25 / uScreenTexelSize.y)) - 0.5) * 2.0);\n"
    "#ifdef CRT_GRID\n"
    "    vec3 mask = texture2D(uTexShadowmask, v_vMaskUV).rgb;\n"
    "#else\n"
    "    // the original expression: every pixel centre maps exactly onto a mask texel edge, so the texel it gets depends\n"
    "    // on this arithmetic's rounding, and any other form of it changes the mask pattern in places\n"
    "    vec3 mask = texture2D(uTexShadowmask, fract(uv / uScreenTexelSize * uShadowmaskTexelSize * 6.0)).rgb;\n"
    "#endif\n"
    "    col = col * (mix(vec3(1.0), mask, uShadowmaskIntensity) * scanline);\n"
    "    // brightness / contrast. The game's contrast is always 1 (global.varCRTContrast), where the original\n"
    "    // pow(abs(2c - 1), vec3(1.0 / contrast)) * sign(c - 0.5) + 0.5 is 2c - 0.5\n"
    "    col = clamp(col + uBrightness, 0.0, 1.0) * 2.0 - 0.5;\n"
    "    // vignette (both smoothsteps are exactly 0 outside the tested band) and film grain\n"
    "    luma = (col.r + col.g + col.b) / 3.0;\n"
    "    vec2 cdelta = uv - vec2(0.5);\n"
    "    float cdist = length(cdelta);\n"
    "    float cbox = max(abs(cdelta.x), abs(cdelta.y));\n"
    "    if (cdist > 0.68 || cbox > 0.48)\n"
    "    {\n"
    "        col = col * (1.0 - max(smoothstep(0.68, 0.7, cdist), smoothstep(0.48, 0.5, cbox)) * uVignetteIntensity);\n"
    "    }\n"
    "    float filmgrain = sm.w * smoothstep(1.0, 0.0, luma);\n"
    "    col = col + vec3(dithernoise, texture2D(uTexNoise, v_vNoiseB).r, texture2D(uTexNoise, v_vNoiseC).r) * filmgrain;\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static const char* crtDistortionVS =
    "uniform float uTime;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "varying vec2 v_vNoiseA;\n"
    "#ifdef CRT_GRID\n"
    "uniform float uScreenDistortIntensity;\n"
    "uniform vec2 uSpecularLightOffset;\n"
    "varying vec2 v_vCRTUV;\n"
    "varying float v_vLightDist2;\n"
    "vec2 getCRTUV(vec2 uv, float distortion)\n"
    "{\n"
    "    vec2 p = uv - vec2(0.5);\n"
    "    return p * mix(1.0, 1.0 + distortion, length(p)) + 0.5;\n"
    "}\n"
    "#endif\n"
    "void main()\n"
    "{\n"
    "    vec4 object_space_pos = vec4( in_Position.x, in_Position.y, in_Position.z, 1.0);\n"
    "    gl_Position = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * object_space_pos;\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "    // noise(uv) as a plain varying (64x64 power-of-two page = GL_REPEAT, so no fract())\n"
    "    v_vNoiseA = in_TextureCoord * (uNoiseTexelSize / uScreenTexelSize) + vec2(fract(uTime * 32.0));\n"
    "#ifdef CRT_GRID\n"
    "    // smooth per vertex of the 64x56 grid: the warped uv (max 0.003 px off) and the squared light distance\n"
    "    v_vCRTUV = getCRTUV(in_TextureCoord, uScreenDistortIntensity);\n"
    "    vec2 ld = getCRTUV(in_TextureCoord - uSpecularLightOffset, uScreenDistortIntensity) - vec2(0.5);\n"
    "    v_vLightDist2 = dot(ld, ld);\n"
    "#endif\n"
    "}\n";

static const char* crtDistortionFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_distortion, Vita rewrite (gl_renderer.c \"CRT filter rewrites\"). Centre offsets only as x - 0.5\n"
    "// (SceShaccCg drops the sign of one when both x - 0.5 and 0.5 - x appear: the 180-degree rotation, see crtFix).\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "varying vec2 v_vNoiseA;\n"
    "uniform float uTime;\n"
    "uniform float uScreenDistortIntensity;\n"
    "uniform float uBorderReflectionIntensity;\n"
    "uniform vec4 uSpecularLight;\n"
    "uniform sampler2D uTexNoise;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "#ifdef CRT_GRID\n"
    "varying vec2 v_vCRTUV;\n"
    "varying float v_vLightDist2;\n"
    "#else\n"
    "uniform vec2 uSpecularLightOffset;\n"
    "#endif\n"
    "float noise(vec2 uv)\n"
    "{\n"
    "    return texture2D(uTexNoise, fract(uv / uScreenTexelSize * uNoiseTexelSize + vec2(uTime * 32.0))).r;\n"
    "}\n"
    "vec2 getCRTUV(vec2 uv, float distortion)\n"
    "{\n"
    "    vec2 p = uv - vec2(0.5);\n"
    "    return p * mix(1.0, 1.0 + distortion, length(p)) + 0.5;\n"
    "}\n"
    "// the original textureCRT_Border() (borderfade = 1); only needed where the screen fades into its reflection\n"
    "vec3 crtBorder(vec2 uv, float distortion, float dithernoise)\n"
    "{\n"
    "    float distortfactor = length(uv - vec2(0.5));\n"
    "    vec2 buv = getCRTUV(uv, distortion);\n"
    "    float warpedCenterDist = length(buv - vec2(0.5));\n"
    "    buv = fract(buv * 0.5) * 2.0;\n"
    "    buv = vec2(1.0) - abs(buv - vec2(1.0));\n"
    "    vec2 bc = abs(buv - vec2(0.5));\n"
    "    float bordercornerfade = 1.0 - max(bc.x + bc.y - 0.75, 0.0) * 2.0;\n"
    "    float borderdistortfactor = smoothstep(0.4, 0.7, distortfactor) + max(max(bc.x, bc.y) - 1.0, 0.0);\n"
    "    buv = (buv - vec2(0.5)) * mix(1.0, 1.0 - distortion, borderdistortfactor * 0.4) + 0.5;\n"
    "    vec2 blurfactor = uScreenTexelSize * 8.0 * smoothstep(0.4, 0.625, warpedCenterDist);\n"
    "    buv = buv + blurfactor * vec2(dithernoise * 2.0 - 1.0, noise(buv.yx) * 2.0 - 1.0);\n"
    "    vec2 bd = abs(buv - vec2(0.5));\n"
    "    float fadefactorBorders = max(smoothstep(0.4, 0.5, bd.x), smoothstep(0.4, 0.5, bd.y)) * pow(bordercornerfade, 3.0);\n"
    "    return mix(vec3(0.0), texture2D(gm_BaseTexture, buv).rgb, fadefactorBorders);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    float dithernoise = texture2D(uTexNoise, v_vNoiseA).r;\n"
    "#ifdef CRT_GRID\n"
    "    vec2 crtuv = v_vCRTUV;\n"
    "    vec3 crt = texture2D(gm_BaseTexture, v_vCRTUV).rgb;\n"
    "    float crtLightDist = sqrt(v_vLightDist2);\n"
    "#else\n"
    "    vec2 crtuv = getCRTUV(uv, uScreenDistortIntensity);\n"
    "    vec3 crt = texture2D(gm_BaseTexture, crtuv).rgb;\n"
    "    float crtLightDist = distance(getCRTUV(uv - uSpecularLightOffset, uScreenDistortIntensity), vec2(0.5));\n"
    "#endif\n"
    "    vec2 wd = abs(crtuv - vec2(0.5));\n"
    "    float fadefactor = max(smoothstep(0.49, 0.5, wd.x), smoothstep(0.49, 0.5, wd.y));\n"
    "    // mix(screen, border, 0.0) == screen: the border reflection only runs in the outer ~9% of the pixels\n"
    "    if (fadefactor > 0.0)\n"
    "    {\n"
    "        crt = mix(crt, crtBorder(uv, uScreenDistortIntensity, dithernoise) * uBorderReflectionIntensity, fadefactor);\n"
    "    }\n"
    "    vec3 final = v_vColour.rgb * crt + uSpecularLight.rgb * smoothstep(0.3, 0.0, crtLightDist + dithernoise * 0.1) * uSpecularLight.a;\n"
    "    gl_FragColor = vec4(final, 1.0);\n"
    "}\n";

static const char* crtPostprocessingVS =
    "uniform float uTime;\n"
    "uniform float uVitaGlowOnly;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "varying vec2 v_vNoiseP;\n"
    "void main()\n"
    "{\n"
    "    vec4 object_space_pos = vec4( in_Position.x, in_Position.y, in_Position.z, 1.0);\n"
    "    gl_Position = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * object_space_pos;\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "    // the original noise(uv) samples floor(uv / texel) * noiseTexel: at a pixel centre that is uv / texel - 0.5.\n"
    "    // In the half-res glow pass (uVitaGlowOnly = 1, target = half size, texel = full-res texel) output pixel X sits\n"
    "    // at full-res 2X + 1 and takes the noise of full-res pixel 2X.\n"
    "    v_vNoiseP = (in_TextureCoord / uScreenTexelSize - (0.5 + 0.5 * uVitaGlowOnly)) * uNoiseTexelSize + vec2(fract(uTime * 32.0));\n"
    "}\n";

static const char* crtPostprocessingFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_postprocessing, Vita rewrite (gl_renderer.c \"CRT filter rewrites\"): the original's 8-step loop\n"
    "// unrolled, noise as a plain varying. uVitaGlowOnly = 1 (set by the half-res glow GML edit) outputs the glow alone.\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "varying vec2 v_vNoiseP;\n"
    "uniform float uGlowFactor;\n"
    "uniform float uGlowTintIntensity;\n"
    "uniform float uBlurSize;\n"
    "uniform float uBlurZoomIntensity;\n"
    "uniform float uVitaGlowOnly;\n"
    "uniform sampler2D uTexNoise;\n"
    "uniform sampler2D uTexRainbow;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "// one iteration of the original loop: tint * sampleScreen(uv + uvoff, 1.0 - zoomoff, zoomoff) * blurIterationWeight\n"
    "vec3 glowTap(vec2 uv, float i_interp)\n"
    "{\n"
    "    vec2 suv = uv + vec2((i_interp - 0.5) * 2.0 * uBlurSize * uScreenTexelSize.x, 0.0);\n"
    "    float zoomoff = (i_interp - 0.5) * 2.0 * uBlurZoomIntensity;\n"
    "    vec3 tint = mix(vec3(1.0), texture2D(uTexRainbow, vec2(i_interp, 0.5)).rgb, uGlowTintIntensity);\n"
    "    vec2 p = suv - vec2(0.5);\n"
    "    p = p * mix(1.0, 1.0 + zoomoff, length(p));\n"
    "    suv = p / (1.0 - zoomoff) + 0.5;\n"
    "    return tint * texture2D(gm_BaseTexture, suv).rgb * (1.0 / 9.0);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    float d = 0.125 * texture2D(uTexNoise, v_vNoiseP).r;\n"
    "    vec3 glowblur = glowTap(uv, d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.125 + d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.25 + d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.375 + d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.5 + d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.625 + d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.75 + d);\n"
    "    glowblur = glowblur + glowTap(uv, 0.875 + d);\n"
    "    vec3 final = texture2D(gm_BaseTexture, uv).rgb * (1.0 - uVitaGlowOnly) + clamp(glowblur * uGlowFactor, vec3(0.0), vec3(0.3, 0.425, 0.5));\n"
    "    gl_FragColor = v_vColour * vec4(final, 1.0);\n"
    "}\n";

// The body below is preceded by the defines vitaCrtRewriteFor() picks: CRT_HALFGLOW (half-res glow composite),
// CRT_MERGE (merged CRT passes), CRT_FINALGLOW (glow composite drawn as the final stretch).
static const char* crtGlowCompositeFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_screenfilter_switch is never used by the game; on the Vita it is the half-res glow composite\n"
    "// (objGameRenderer.mthRenderCRT, CRT type 3, gml edit \"half-res glow\"): Pass4 + bilinear-upscaled glow, drawn into\n"
    "// Pass5, or (CRT_FINALGLOW, gml edit \"merged CRT passes\") as the final stretch onto the screen, which drops Pass5.\n"
    "// It also carries the uniforms the GML tests with shader_get_uniform to pick the Vita paths: uVitaGlowComposite\n"
    "// (half-res glow), uVitaCrtMerge (the screen filter reads Pass1, no Pass2 copy), uVitaGlowFinal (no Pass5).\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "uniform sampler2D uTexGlow;\n"
    "#ifdef CRT_HALFGLOW\n"
    "uniform float uVitaGlowComposite;\n"
    "#endif\n"
    "#ifdef CRT_MERGE\n"
    "uniform float uVitaCrtMerge;\n"
    "#endif\n"
    "#ifdef CRT_FINALGLOW\n"
    "uniform float uVitaGlowFinal;\n"
    "#endif\n"
    "void main()\n"
    "{\n"
    "    vec3 final = texture2D(gm_BaseTexture, v_vTexcoord).rgb;\n"
    "#ifdef CRT_HALFGLOW\n"
    "    final = final + texture2D(uTexGlow, v_vTexcoord).rgb * uVitaGlowComposite;\n"
    "#endif\n"
    "    // the gate uniforms only have to be active: the GML never sets them above 1 (unset = 0), so this is 1.0\n"
    "    float gate = 1.0;\n"
    "#ifdef CRT_MERGE\n"
    "    gate = max(gate, uVitaCrtMerge);\n"
    "#endif\n"
    "#ifdef CRT_FINALGLOW\n"
    "    gate = max(gate, uVitaGlowFinal);\n"
    "#endif\n"
    "    // gate (== 1.0) multiplies the colour so the gate uniforms stay active and the alpha stays the vertex alpha\n"
    "    // (\"Opaque draws\" classifies v_vColour * vec4(..., 1.0) as unblendable)\n"
    "    gl_FragColor = v_vColour * vec4(final * gate, 1.0);\n"
    "}\n";

typedef struct { const char* name; const char* vertexBody; const char* fragmentBody; bool grid; } VitaCrtRewrite;

static bool vitaMarkerFile(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

// NULL when the shader has no rewrite or the rewrites are switched off
static const VitaCrtRewrite* vitaCrtRewriteFor(const char* name) {
    static VitaCrtRewrite table[4];
    static int count = -1; // -1 = not set up yet
    if (count < 0) {
        count = 0;
        if (vitaMarkerFile(BS_VITA_PATH("nocrtfast"))) {
            logInfo("GL: CRT filter rewrites disabled (nocrtfast)\n");
        } else {
            bool grid = !vitaMarkerFile(BS_VITA_PATH("nocrtgrid"));
            if (!grid) logInfo("GL: CRT filter grid disabled (nocrtgrid)\n");
            table[count++] = (VitaCrtRewrite) { "shd_retroscreen_screenfilter", crtScreenfilterVS, crtScreenfilterFS, grid };
            table[count++] = (VitaCrtRewrite) { "shd_retroscreen_distortion", crtDistortionVS, crtDistortionFS, grid };
            table[count++] = (VitaCrtRewrite) { "shd_retroscreen_postprocessing", crtPostprocessingVS, crtPostprocessingFS, false };
            // never used by the game: the half-res glow composite (gml edit "half-res glow"), original vertex shader.
            // Without it the GML keeps the full-res glow (<data dir>/nocrthalfglow, for A/B against the original).
            // Its gate uniforms also switch on the merged CRT passes (gml edits "merged CRT passes"): the screen
            // filter reads Pass1 instead of Pass2, its 1:1 surface_copy_part copy (all types), and in type 3 the glow
            // composite is the final stretch instead of a Pass5 render pass. <data dir>/nocrtmerge turns both off,
            // <data dir>/nocrtfinalglow only the second. If SceShaccCg rejects this shader, the original is compiled:
            // no gate uniforms, so the GML keeps Pass2, Pass5 and the full-res glow (as with nocrthalfglow).
            bool halfGlow = !vitaMarkerFile(BS_VITA_PATH("nocrthalfglow"));
            bool merge = !vitaMarkerFile(BS_VITA_PATH("nocrtmerge"));
            bool finalGlow = halfGlow && merge && !vitaMarkerFile(BS_VITA_PATH("nocrtfinalglow"));
            if (!halfGlow) logInfo("GL: CRT half-res glow disabled (nocrthalfglow)\n");
            if (!merge) logInfo("GL: CRT pass merging disabled (nocrtmerge)\n");
            else if (halfGlow && !finalGlow) logInfo("GL: CRT glow in the final stretch disabled (nocrtfinalglow)\n");
            if (halfGlow || merge) {
                static char* compositeFS = NULL;
                const char* defs[3] = { halfGlow ? "#define CRT_HALFGLOW 1\n" : "", merge ? "#define CRT_MERGE 1\n" : "",
                                        finalGlow ? "#define CRT_FINALGLOW 1\n" : "" };
                size_t len = strlen(defs[0]) + strlen(defs[1]) + strlen(defs[2]) + strlen(crtGlowCompositeFS) + 1;
                if (compositeFS == NULL) compositeFS = (char*) safeMalloc(len); // kept: the table is static
                snprintf(compositeFS, len, "%s%s%s%s", defs[0], defs[1], defs[2], crtGlowCompositeFS);
                table[count++] = (VitaCrtRewrite) { "shd_retroscreen_screenfilter_switch", NULL, compositeFS, false };
            }
        }
    }
    if (name == NULL) return NULL;
    for (int i = 0; i < count; i++)
        if (strcmp(table[i].name, name) == 0) return &table[i];
    return NULL;
}

// The original source up to (not including) marker, "#define CRT_GRID 1" for grid passes, then body.
static char* vitaCrtSplice(const char* src, const char* marker, bool grid, const char* body) {
    const char* at = src != NULL ? strstr(src, marker) : NULL;
    if (at == NULL) return NULL;
    static const char* gridDefine = "#define CRT_GRID 1\n";
    size_t head = (size_t) (at - src);
    char* out = (char*) safeMalloc(head + strlen(gridDefine) + strlen(body) + 1);
    memcpy(out, src, head);
    out[head] = '\0';
    if (grid) strcat(out, gridDefine);
    strcat(out, body);
    return out;
}
#endif

#ifdef PLATFORM_VITA
// ===[ CRT bake (Vita) ]===
// The screen filter and distortion passes, with every term that depends neither on uTime nor on the frame baked into
// a texture once, the plasma tint into a small texture once per frame (instead of the 64x56 vertex grid above).
// For each of the two shaders there are three programs:
//   - bake program: the original vertex shader and the time-independent terms, drawn into an RGBA16F texture the
//     size of the pass's viewport with exactly the pass's own geometry and matrices (flushBatch, "CRT bake" below),
//     so every bake texel was computed from the same interpolated uv as the pixel that reads it. Redrawn only when
//     its key (viewport size, the batch's vertex positions / uvs, gm_Matrices, the uniforms and textures the bake
//     reads) changes.
//       screen filter: rgb = static tint * shadow mask * scanline (the original mask expression, so its
//                      rounding-dependent texel choice is the original's), a = vignette factor
//       distortion:    xy = getCRTUV(uv) - uv, z = border fade factor, w = 1 - specular light distance / 0.3
//   - plasma program (screen filter only): sin and cos of sampleRainbowtint()'s plasma value and the film grain
//     distance factor on a 64x56-cell grid, drawn into a 72x58 RGBA8 texture whenever uTime changes; the pass
//     samples it bilinearly and renormalises (sin, cos) (interpolation error <= ~2/255, the grid's).
//   - the pass program (the GML shader itself): what is left per pixel, reading the bake by window position.
// Uniforms only the bake / plasma programs use get stand-in handles from shader_get_uniform (their values are only
// recorded), the shadow mask sampler a texture unit of its own (CRT_BAKE_UNIT_MASK). Bake textures are freed with the
// surface they were drawn for (CRT off frees them all). <data dir>/nocrtbake (or a failed compile) keeps the
// rewrites above. Centre offsets only as x - 0.5 (SceShaccCg sign bug, see crtFix).
#define CRT_BAKE_UNIT_MASK 4   // the shadow mask for the screen filter's bake program (texture_set_stage target)
#define CRT_BAKE_UNIT_PLASMA 5
#define CRT_BAKE_UNIT_BAKE 6   // (7 is the palettizer table)
#define CRT_PLASMA_GX 64       // plasma grid cells; texel (i, j) holds the plasma at uv (i / GX, j / GY)
#define CRT_PLASMA_GY 56
#define CRT_PLASMA_W 72        // GX + 1 texels used; width a multiple of 8 (vitaGL's linear texture / colour surface
                               // stride), sizes even (vitaGL halves viewport sizes as integers)
#define CRT_PLASMA_H 58
#define CRT_BAKE_STR2(x) #x
#define CRT_BAKE_STR(x) CRT_BAKE_STR2(x)

// the original vertex shader's main (bake programs: same gl_Position and v_vTexcoord as the pass)
static const char* crtBakeVS =
    "void main()\n"
    "{\n"
    "    vec4 object_space_pos = vec4( in_Position.x, in_Position.y, in_Position.z, 1.0);\n"
    "    gl_Position = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * object_space_pos;\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "}\n";

static const char* crtBakeScreenfilterFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_screenfilter, Vita bake program (gl_renderer.c \"CRT bake\"): the time-independent terms\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "uniform float uShadowmaskIntensity;\n"
    "uniform float uScanlineIntensity;\n"
    "uniform float uVignetteIntensity;\n"
    "uniform sampler2D uTexShadowmask;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uShadowmaskTexelSize;\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    vec2 cdelta = uv - vec2(0.5);\n"
    "    vec3 statictint = vec3(mix(0.85, 1.1, uv.y), 1.0, mix(0.95, 1.2, abs(cdelta.x) * 2.0));\n"
    "    float scanline = mix(1.0 - uScanlineIntensity, 1.0, abs(fract(uv.y * (0.25 / uScreenTexelSize.y)) - 0.5) * 2.0);\n"
    "    // the original expression (every pixel centre maps exactly onto a mask texel edge: the texel it gets depends\n"
    "    // on this arithmetic's rounding, which is the same here as in the original: same uv, same expression)\n"
    "    vec3 mask = mix(vec3(1.0), texture2D(uTexShadowmask, fract(uv / uScreenTexelSize * uShadowmaskTexelSize * 6.0)).rgb, uShadowmaskIntensity);\n"
    "    float vignette = max(smoothstep(0.68, 0.7, length(cdelta)), smoothstep(0.48, 0.5, max(abs(cdelta.x), abs(cdelta.y))));\n"
    "    gl_FragColor = vec4(statictint * mask * scanline, mix(1.0, 0.0, vignette * uVignetteIntensity));\n"
    "}\n";

// plasma program: vertex positions are clip coordinates, in_TextureCoord the uv the plasma is evaluated at
static const char* crtPlasmaVS =
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(in_Position.x, in_Position.y, 0.0, 1.0);\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "}\n";

static const char* crtPlasmaFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_screenfilter, Vita plasma program (gl_renderer.c \"CRT bake\"): sampleRainbowtint()'s plasma\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "uniform float uTime;\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    vec2 ruv = uv;\n"
    "    ruv.x = ruv.x + sin(ruv.y * 6.28318530718 + uTime) * 0.1;\n"
    "    ruv.y = ruv.y + cos(ruv.x * 9.42477796077 + 1.57079632679 + uTime) * 0.1;\n"
    "    ruv = ruv * 10.0;\n"
    "    vec4 waves = sin(vec4(ruv.x, ruv.y, ruv.x + ruv.y, length(ruv - vec2(0.5))) + uTime);\n"
    "    float plasma = waves.x + waves.y + waves.z + waves.w;\n"
    "    vec2 plasmaSinCos = sin(vec2(plasma, plasma + 1.57079632679));\n"
    "    gl_FragColor = vec4(plasmaSinCos * 0.5 + 0.5, smoothstep(0.5, 0.7, length(uv - vec2(0.5))), 1.0);\n"
    "}\n";

static const char* crtBakedScreenfilterVS =
    "uniform float uTime;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "uniform vec2 uVitaBakeScale;\n"
    "varying vec2 v_vNoiseA;\n"
    "varying vec2 v_vNoiseB;\n"
    "varying vec2 v_vNoiseC;\n"
    "varying vec2 v_vBakeUV;\n"
    "varying vec2 v_vPlasmaUV;\n"
    "void main()\n"
    "{\n"
    "    vec4 object_space_pos = vec4( in_Position.x, in_Position.y, in_Position.z, 1.0);\n"
    "    vec4 clipPos = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * object_space_pos;\n"
    "    gl_Position = clipPos;\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "    vec2 noiseScale = uNoiseTexelSize / uScreenTexelSize;\n"
    "    vec2 noiseOffset = vec2(fract(uTime * 32.0));\n"
    "    v_vNoiseA = in_TextureCoord * noiseScale + noiseOffset;\n"
    "    v_vNoiseB = in_TextureCoord.yx * noiseScale + noiseOffset;\n"
    "    v_vNoiseC = vec2(1.0 - in_TextureCoord.x, in_TextureCoord.y) * noiseScale + noiseOffset;\n"
    "    // the bake texture's texel for this pixel: its window position in the viewport (uVitaBakeScale corrects for\n"
    "    // vitaGL's integer viewport halving; 1 for even sizes)\n"
    "    v_vBakeUV = (clipPos.xy / clipPos.w * 0.5 + 0.5) * uVitaBakeScale;\n"
    "    v_vPlasmaUV = (in_TextureCoord * vec2(" CRT_BAKE_STR(CRT_PLASMA_GX) ".0, " CRT_BAKE_STR(CRT_PLASMA_GY) ".0) + 0.5) / vec2("
        CRT_BAKE_STR(CRT_PLASMA_W) ".0, " CRT_BAKE_STR(CRT_PLASMA_H) ".0);\n"
    "}\n";

static const char* crtBakedScreenfilterFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_screenfilter, Vita pass program with baked terms (gl_renderer.c \"CRT bake\")\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "varying vec2 v_vNoiseA;\n"
    "varying vec2 v_vNoiseB;\n"
    "varying vec2 v_vNoiseC;\n"
    "varying vec2 v_vBakeUV;\n"
    "varying vec2 v_vPlasmaUV;\n"
    "uniform float uTintIntensity;\n"
    "uniform float uFilmgrainIntensity;\n"
    "uniform float uBrightness;\n"
    "uniform float uBleedIntensity;\n"
    "uniform float uBleedSize;\n"
    "uniform float uColorLimit;\n"
    "uniform sampler2D uTexNoise;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform sampler2D uVitaBake;\n"
    "uniform sampler2D uVitaPlasma;\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    vec3 col = floor((v_vColour.rgb * texture2D(gm_BaseTexture, uv).rgb) * uColorLimit + 0.5) / uColorLimit;\n"
    "    float dithernoise = texture2D(uTexNoise, v_vNoiseA).r;\n"
    "    // colour bleed: neighbour at -uBleedSize * (noise - 0.3) texels\n"
    "    vec3 neighbor = texture2D(gm_BaseTexture, vec2(uv.x + uScreenTexelSize.x * uBleedSize * (0.3 - dithernoise), uv.y)).rgb;\n"
    "    float luma = dot(col, vec3(0.333333333));\n"
    "    vec3 bleedmix = smoothstep(vec3(0.4, 0.5, 0.3), vec3(0.9), neighbor) * (smoothstep(1.0, 0.0, luma) * 0.8 + 0.2);\n"
    "    col = clamp(col + neighbor * bleedmix * uBleedIntensity * vec3(1.0, 0.8, 0.9), 0.0, 1.0);\n"
    "    // baked: static tint * shadow mask * scanline, vignette factor\n"
    "    vec4 bake = texture2D(uVitaBake, v_vBakeUV);\n"
    "    // plasma tint: (sin p, sin(p + PI), sin(p + PI / 2)) = (s, -s, c) from the interpolated, renormalised (s, c).\n"
    "    // The minus sits in a constant, not on sc.x: SceShaccCg has lost the sign of a negated value that is also\n"
    "    // used unnegated.\n"
    "    vec4 plasma = texture2D(uVitaPlasma, v_vPlasmaUV);\n"
    "    vec2 sc = plasma.xy * 2.0 - 1.0;\n"
    "    sc = sc * inversesqrt(dot(sc, sc));\n"
    "    vec3 tint = vec3(sc.x, sc.x, sc.y) * (vec3(0.5, -0.5, 0.5) * uTintIntensity) + (1.0 - 0.5 * uTintIntensity);\n"
    "    col = col * bake.rgb * tint;\n"
    "    // brightness / contrast (the game's contrast is always 1, see the rewrite above)\n"
    "    col = clamp(col + uBrightness, 0.0, 1.0) * 2.0 - 0.5;\n"
    "    luma = dot(col, vec3(0.333333333));\n"
    "    float filmgrain = mix(0.05, uFilmgrainIntensity, plasma.z) * smoothstep(1.0, 0.0, luma);\n"
    "    col = col * bake.a + vec3(dithernoise, texture2D(uTexNoise, v_vNoiseB).r, texture2D(uTexNoise, v_vNoiseC).r) * filmgrain;\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static const char* crtBakeDistortionFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_distortion, Vita bake program (gl_renderer.c \"CRT bake\"): the time-independent terms\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "uniform float uScreenDistortIntensity;\n"
    "uniform vec2 uSpecularLightOffset;\n"
    "vec2 getCRTUV(vec2 uv, float distortion)\n"
    "{\n"
    "    vec2 p = uv - vec2(0.5);\n"
    "    return p * mix(1.0, 1.0 + distortion, length(p)) + 0.5;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    vec2 wd = abs(getCRTUV(uv, uScreenDistortIntensity) - vec2(0.5));\n"
    "    float fadefactor = max(smoothstep(0.49, 0.5, wd.x), smoothstep(0.49, 0.5, wd.y));\n"
    "    float crtLightDist = distance(getCRTUV(uv - uSpecularLightOffset, uScreenDistortIntensity), vec2(0.5));\n"
    "    // the warp as an offset, getCRTUV(uv) - uv = p * distortion * |p| (|offset| < 0.018: fp16 keeps it within 8e-6\n"
    "    // uv). Not written as getCRTUV(uv) - uv: reassociated, that is p * k + (0.5 - uv) next to p = uv - 0.5, the\n"
    "    // pattern SceShaccCg loses a sign in. w: smoothstep(0.3, 0.0, d)'s (d - 0.3) / -0.3 without the noise term.\n"
    "    vec2 p = uv - vec2(0.5);\n"
    "    gl_FragColor = vec4(p * (length(p) * uScreenDistortIntensity), fadefactor, crtLightDist * (-1.0 / 0.3) + 1.0);\n"
    "}\n";

static const char* crtBakedDistortionVS =
    "uniform float uTime;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "uniform vec2 uVitaBakeScale;\n"
    "varying vec2 v_vNoiseA;\n"
    "varying vec2 v_vBakeUV;\n"
    "void main()\n"
    "{\n"
    "    vec4 object_space_pos = vec4( in_Position.x, in_Position.y, in_Position.z, 1.0);\n"
    "    vec4 clipPos = gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION] * object_space_pos;\n"
    "    gl_Position = clipPos;\n"
    "    v_vColour = in_Colour;\n"
    "    v_vTexcoord = in_TextureCoord;\n"
    "    v_vNoiseA = in_TextureCoord * (uNoiseTexelSize / uScreenTexelSize) + vec2(fract(uTime * 32.0));\n"
    "    v_vBakeUV = (clipPos.xy / clipPos.w * 0.5 + 0.5) * uVitaBakeScale;\n"
    "}\n";

static const char* crtBakedDistortionFS =
    "#define _YY_GLSLES_ 1\n"
    "// shd_retroscreen_distortion, Vita pass program with baked terms (gl_renderer.c \"CRT bake\")\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "varying vec2 v_vNoiseA;\n"
    "varying vec2 v_vBakeUV;\n"
    "uniform float uTime;\n"
    "uniform float uScreenDistortIntensity;\n"
    "uniform float uBorderReflectionIntensity;\n"
    "uniform vec4 uSpecularLight;\n"
    "uniform sampler2D uTexNoise;\n"
    "uniform vec2 uScreenTexelSize;\n"
    "uniform vec2 uNoiseTexelSize;\n"
    "uniform sampler2D uVitaBake;\n"
    "float noise(vec2 uv)\n"
    "{\n"
    "    return texture2D(uTexNoise, fract(uv / uScreenTexelSize * uNoiseTexelSize + vec2(uTime * 32.0))).r;\n"
    "}\n"
    "vec2 getCRTUV(vec2 uv, float distortion)\n"
    "{\n"
    "    vec2 p = uv - vec2(0.5);\n"
    "    return p * mix(1.0, 1.0 + distortion, length(p)) + 0.5;\n"
    "}\n"
    "// the original textureCRT_Border() (borderfade = 1); only needed where the screen fades into its reflection\n"
    "vec3 crtBorder(vec2 uv, float distortion, float dithernoise)\n"
    "{\n"
    "    float distortfactor = length(uv - vec2(0.5));\n"
    "    vec2 buv = getCRTUV(uv, distortion);\n"
    "    float warpedCenterDist = length(buv - vec2(0.5));\n"
    "    buv = fract(buv * 0.5) * 2.0;\n"
    "    buv = vec2(1.0) - abs(buv - vec2(1.0));\n"
    "    vec2 bc = abs(buv - vec2(0.5));\n"
    "    float bordercornerfade = 1.0 - max(bc.x + bc.y - 0.75, 0.0) * 2.0;\n"
    "    float borderdistortfactor = smoothstep(0.4, 0.7, distortfactor) + max(max(bc.x, bc.y) - 1.0, 0.0);\n"
    "    buv = (buv - vec2(0.5)) * mix(1.0, 1.0 - distortion, borderdistortfactor * 0.4) + 0.5;\n"
    "    vec2 blurfactor = uScreenTexelSize * 8.0 * smoothstep(0.4, 0.625, warpedCenterDist);\n"
    "    buv = buv + blurfactor * vec2(dithernoise * 2.0 - 1.0, noise(buv.yx) * 2.0 - 1.0);\n"
    "    vec2 bd = abs(buv - vec2(0.5));\n"
    "    float fadefactorBorders = max(smoothstep(0.4, 0.5, bd.x), smoothstep(0.4, 0.5, bd.y)) * pow(bordercornerfade, 3.0);\n"
    "    return mix(vec3(0.0), texture2D(gm_BaseTexture, buv).rgb, fadefactorBorders);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = v_vTexcoord;\n"
    "    // baked: warp offset, border fade factor, specular light term\n"
    "    vec4 bake = texture2D(uVitaBake, v_vBakeUV);\n"
    "    float dithernoise = texture2D(uTexNoise, v_vNoiseA).r;\n"
    "    vec3 crt = texture2D(gm_BaseTexture, uv + bake.xy).rgb;\n"
    "    // mix(screen, border, 0.0) == screen: the border reflection only runs in the outer ~10% of the pixels\n"
    "    if (bake.z > 0.0)\n"
    "    {\n"
    "        crt = mix(crt, crtBorder(uv, uScreenDistortIntensity, dithernoise) * uBorderReflectionIntensity, bake.z);\n"
    "    }\n"
    "    // smoothstep(0.3, 0.0, crtLightDist + dithernoise * 0.1)\n"
    "    float lightterm = clamp(bake.w - dithernoise * (0.1 / 0.3), 0.0, 1.0);\n"
    "    vec3 final = v_vColour.rgb * crt + uSpecularLight.rgb * (lightterm * lightterm * (3.0 - 2.0 * lightterm)) * uSpecularLight.a;\n"
    "    gl_FragColor = vec4(final, 1.0);\n"
    "}\n";

typedef struct {
    char name[32];
    GLenum type;
    int32_t handle;  // the pass program's location, or a stand-in (< -1) when only the bake / plasma programs use it
    float value[4];  // last value the game set
} VitaCrtBakeUniform;

typedef struct {
    GMLShader prog;
    int32_t count;
    struct { GLint location; int32_t uniform; } set[8]; // its uniforms, from VitaCrtBake.uniforms
} VitaCrtBakeProgram;

typedef struct VitaCrtBake {
    bool screenfilter;          // else distortion
    VitaCrtBakeProgram bake, plasma;
    VitaCrtBakeUniform uniforms[12];
    int32_t uniformCount;
    int32_t timeUniform;        // index of uTime in uniforms (plasma), -1 if none
    GLint scaleLocation;        // the pass program's uVitaBakeScale
    // bake texture
    GLuint bakeTexture;
    int32_t bakeW, bakeH;
    bool baked;
    uint64_t bakeKey;
    GLuint bakeTarget;          // the framebuffer the pass drew into when it was baked (freed with it)
    // plasma texture
    GLuint plasmaTexture, plasmaFbo;
    bool plasmaValid;
    float plasmaTime;
    bool failed;                // couldn't allocate: drawn with whatever is bound (logged once)
} VitaCrtBake;
#endif

static GLuint compileShader(GLenum type, const char* source, bool* ok) {
    #ifdef PLATFORM_VITA
    const char* actualSource = fixShaderForVita(source);
    #else
    const char* actualSource = source;
    #endif
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &actualSource, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512] = ""; // vitaGL built without HAVE_SHARK_LOG leaves it untouched
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        logError("GL: Shader compilation failed: %s\n", infoLog);
        glDeleteShader(shader);
        #ifdef PLATFORM_VITA
        free((char*)actualSource);
        #endif
        *ok = false;
        return 0;
    }
    *ok = true;
    #ifdef PLATFORM_VITA
    free((char*)actualSource);
    #endif
    return shader;
}

static GLuint linkProgram(const char* name, uint32_t vertexAttributeCount, const char** vertexAttributes, GLuint vertShader, GLuint fragShader, bool *success2) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);

    repeat(vertexAttributeCount, i) {
        glBindAttribLocation(program, i, vertexAttributes[i]);
    }

    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        logError("GL: Shader %s linking failed: %s\n", name, infoLog);
        *success2 = false;
    } else {
        *success2 = true;
        logInfo("GL: Shader %s succesfully linked!\n", name);
    }
    return program;
}

static GLShaderUniform* getShaderUniform(GMLShader* shader, const char* name, GLenum type) {
    GLint location = glGetUniformLocation(shader->shaderId, name);
    if (location < 0) return NULL;

    GLShaderUniform* uniform = (GLShaderUniform*) safeCalloc(1, sizeof(GLShaderUniform));
    uniform->location = location;
    uniform->type = type;
    return uniform;
}

// shdPalettizer fast path (defined below glInit's helpers)
static GLuint palettizerTable(GLModernRenderer* m, GLuint palette);
static inline bool fastPalettizerActive(GLModernRenderer* m);
#ifdef PLATFORM_VITA
static GLuint palettizerColorTable(GLModernRenderer* m, GLuint palette);
#endif
static void glApplyGmlProgram(Renderer* renderer, GMLShader* gmlShader);
static void freeShader(GMLShader* shader);
#ifdef PLATFORM_VITA
static void vitaCrtBakeBeforeDraw(GLModernRenderer* m, GMLShader* shader, GlVertex* vertices, bool quads, int32_t count);
#endif

#ifdef PLATFORM_VITA
// ===[ Opaque draws (Vita) ]===
// GXM renders each scene tile by tile, and every tile starts from the ISP background object, whose pixel program reads
// the colour surface's previous contents from memory (SceGxmColorSurface.backgroundTex). Like any other object it is
// removed by hidden surface removal where an opaque draw covers it. A blended (translucent) draw needs what is under
// it: the background is loaded for every pixel it touches, and the shader patcher appends the blend code to its
// fragment program. vitaGL 2bdbe89 has no load/store control (no invalidate/discard call, NULL valid region): every
// scene stores its whole surface, and the only way to skip the load is to cover the surface with opaque draws.
// Every pass of the frame was blended (GameMaker's bm_normal), also the ones whose every fragment has alpha 1, where
// src * 1 + dst * 0 == src: blending them only cost the background load and the blend instructions. flushBatch draws
// a batch with blending off when its fragments have alpha exactly 1.0 and the mode is bm_normal: the same pixels,
// alpha included. Alpha-1 sources:
//  - GML shaders whose single gl_FragColor write is vec4(..., 1.0), or v_vColour * vec4(..., 1.0) with every vertex
//    alpha 255: the CRT screen filter, distortion, postprocessing and glow composite (rewritten or original);
//    shdPalettizer (original or table version) with an opaque palette texture;
//  - the default program (texture * vertex colour) with every vertex alpha 255 and an opaque texture: the white
//    texture (rectangles), a data.win page whose texels are all opaque, or a surface whose last full write was an
//    alpha-1 draw covering all of it (vitaSurfaceOpaque, cleared by any other write).
// So Pass1 to Pass5 and the glow pass become opaque, and the final screen scene too (the black rectangle and the game
// image, plus the border art through the gml edit "opaque border", gated by the probe in glShaderGetUniform): the
// border and rectangle pixels under the image are never shaded. Same pixels provided the GPU blends alpha 255 as 1.0
// and filters / interpolates equal values exactly (any miss would be 1/255). <data dir>/noopaque turns all of it off;
// <data dir>/noopaquelinear keeps it for nearest-filtered textures only (border edit off).
enum { VITA_ALPHA_UNKNOWN = 0, VITA_ALPHA_ONE, VITA_ALPHA_VERTEX, VITA_ALPHA_PALETTE };
#define VITA_OPAQUE_PROBE_NAME "bs_vita_opaque_border"
#define VITA_OPAQUE_PROBE_HANDLE 0x7FFFFF01 // what shader_get_uniform returns for the probe; never a GL location
static int vitaOpaqueMode = -1;          // -1 markers not read yet, 0 off, 1 on, 2 nearest-filtered textures only
static uint8_t* vitaShaderAlpha;         // per GML shader: VITA_ALPHA_*
static bool* vitaShaderNoDiscard;        // per GML shader: no discard after the GameMaker header
static uint32_t vitaShaderCount;
static int32_t vitaPaletteSlot = -1;     // shdPalettizer's spritePalette sampler slot
static uint8_t* vitaPageOpaque;          // per data.win page: decoded on the CPU and every texel's alpha is 255
static uint32_t vitaPageCount;
static uint8_t* vitaSurfaceOpaque;       // per surface: every texel's alpha is 255
static uint32_t vitaSurfaceCap;
static bool vitaBatchAlpha255 = true;    // every vertex of the current batch has alpha 255
static bool vitaBatchCovers = false;     // the current batch covers all of its target surface
extern uint32_t bsGlUnblended;           // loop.c PERF line "unblended=": batch draws with blending off

static int vitaOpaque(void) {
    if (vitaOpaqueMode < 0) {
        vitaOpaqueMode = vitaMarkerFile(BS_VITA_PATH("noopaque")) ? 0 : vitaMarkerFile(BS_VITA_PATH("noopaquelinear")) ? 2 : 1;
        logInfo("GL: opaque draws %s\n", vitaOpaqueMode == 0 ? "disabled (noopaque)" : vitaOpaqueMode == 2 ? "for nearest-filtered textures only (noopaquelinear)" : "on");
    }
    return vitaOpaqueMode;
}

static size_t vitaCountOf(const char* s, const char* what) {
    size_t n = 0, len = strlen(what);
    for (const char* p = s; (p = strstr(p, what)) != NULL; p += len) n++;
    return n;
}

// VITA_ALPHA_ONE / VITA_ALPHA_VERTEX if the shader's only gl_FragColor write is the last statement of main and is
// vec4(<...>, 1.0) / v_vColour * vec4(<...>, 1.0) (the latter only if the vertex shader passes in_Colour through).
static uint8_t vitaFragColorKind(const char* vs, const char* fs) {
    const char* at = fs != NULL ? strstr(fs, "gl_FragColor") : NULL;
    if (at == NULL || strstr(at + 1, "gl_FragColor") != NULL) return VITA_ALPHA_UNKNOWN;
    char expr[256];
    size_t n = 0;
    const char* p = at + strlen("gl_FragColor");
    for (; *p != '\0' && *p != ';'; p++) {
        if (isspace((unsigned char) *p)) continue;
        if (n + 1 >= sizeof(expr)) return VITA_ALPHA_UNKNOWN;
        expr[n++] = *p;
    }
    expr[n] = '\0';
    if (*p != ';') return VITA_ALPHA_UNKNOWN;
    // only a // comment, the closing brace of main and the end of the source may follow
    p++;
    int braces = 0;
    while (*p != '\0') {
        if (isspace((unsigned char) *p)) { p++; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p != '\0' && *p != '\n') p++; continue; }
        if (*p == '}' && braces == 0) { braces = 1; p++; continue; }
        return VITA_ALPHA_UNKNOWN;
    }
    if (braces != 1) return VITA_ALPHA_UNKNOWN;
    uint8_t kind;
    const char* args;
    if (strncmp(expr, "=vec4(", 6) == 0) { kind = VITA_ALPHA_ONE; args = expr + 6; }
    else if (strncmp(expr, "=v_vColour*vec4(", 16) == 0) { kind = VITA_ALPHA_VERTEX; args = expr + 16; }
    else return VITA_ALPHA_UNKNOWN;
    // the vec4( must close at the very end, and its last top-level argument must be the literal 1.0
    int depth = 1;
    const char* lastComma = NULL;
    const char* q = args;
    for (; *q != '\0'; q++) {
        if (*q == '(') depth++;
        else if (*q == ')') { if (--depth == 0) break; }
        else if (*q == ',' && depth == 1) lastComma = q;
    }
    if (depth != 0 || q[1] != '\0' || lastComma == NULL || q - lastComma != 4 || strncmp(lastComma, ",1.0", 4) != 0)
        return VITA_ALPHA_UNKNOWN;
    // v_vColour must be in_Colour as is: its declaration and that one assignment are its only mentions (so no
    // v_vColour.a *= ..., v_vColour=..., or a function writing it through an out parameter)
    if (kind == VITA_ALPHA_VERTEX && (vs == NULL || vitaCountOf(vs, "v_vColour") != 2 ||
            strstr(vs, "varying vec4 v_vColour;") == NULL || strstr(vs, "v_vColour = in_Colour;") == NULL))
        return VITA_ALPHA_UNKNOWN;
    return kind;
}

// Called for every GML shader that compiled, with the sources it was compiled from.
static void vitaClassifyShader(int32_t index, const char* name, const char* vs, const char* fs) {
    if (index < 0 || (uint32_t) index >= vitaShaderCount || fs == NULL) return;
    uint8_t kind = VITA_ALPHA_UNKNOWN;
    if (name != NULL && strcmp(name, "shdPalettizer") == 0) {
        // fCol = pCol (a palette sample) on the loop's first pass (i == 0), and gl_FragColor = fCol
        if (vitaCountOf(fs, "gl_FragColor") == 1 && strstr(fs, "gl_FragColor = fCol;") != NULL &&
                strstr(fs, "if (i == 0 || dist < bestDist)") != NULL && strstr(fs, "fCol = pCol;") != NULL)
            kind = VITA_ALPHA_PALETTE;
    } else {
        kind = vitaFragColorKind(vs, fs);
    }
    vitaShaderAlpha[index] = kind;
    // the GameMaker header defines DoAlphaTest (with a discard); the shader's own code starts at _YY_GLSLES_
    const char* body = strstr(fs, "#define _YY_GLSLES_ 1");
    vitaShaderNoDiscard[index] = body != NULL ? strstr(body, "discard") == NULL && strstr(body, "DoAlphaTest(") == NULL
                                              : strstr(fs, "discard") == NULL;
    if (kind != VITA_ALPHA_UNKNOWN)
        logInfo("GL: %s: output alpha is %s\n", name != NULL ? name : "?",
                kind == VITA_ALPHA_ONE ? "1.0" : kind == VITA_ALPHA_VERTEX ? "the vertex alpha" : "the palette's");
}

static void vitaSetSurfaceOpaque(int32_t surface, bool opaque) {
    if (surface < 0) return;
    if ((uint32_t) surface >= vitaSurfaceCap) {
        if (!opaque) return; // untracked = not opaque
        uint32_t cap = (uint32_t) surface + 16;
        vitaSurfaceOpaque = (uint8_t*) safeRealloc(vitaSurfaceOpaque, cap);
        memset(vitaSurfaceOpaque + vitaSurfaceCap, 0, cap - vitaSurfaceCap);
        vitaSurfaceCap = cap;
    }
    vitaSurfaceOpaque[surface] = opaque ? 1 : 0;
}

// The surface whose FBO is the current draw target, -1 for the screen or anything else.
static int32_t vitaBoundSurface(GLRenderer* gl) {
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    if (fb == 0) return -1;
    for (uint32_t i = 0; i < gl->surfaceCount; i++)
        if (gl->surfaces[i] == (GLuint) fb) return (int32_t) i;
    return -1;
}

// Every texel of tex has alpha 255 (so does any filtered sample of it).
static bool vitaTextureOpaque(GLRenderer* gl, GLuint tex) {
    if (tex == 0 || (vitaOpaqueMode == 2 && gl->base.texFilter)) return false;
    if (tex == gl->whiteTexture) return true;
    for (uint32_t i = 0; i < gl->surfaceCount; i++)
        if (gl->surfaceTexture[i] == tex) return i < vitaSurfaceCap && vitaSurfaceOpaque[i] != 0;
    for (uint32_t i = 0; i < vitaPageCount && i < gl->textureCount; i++)
        if (gl->glTextures[i] == tex) return vitaPageOpaque[i] != 0;
    return false;
}

// Every fragment the current batch writes has alpha exactly 1.0.
static bool vitaBatchAlphaOne(GLModernRenderer* m) {
    GLRenderer* gl = &m->base;
    if (vitaOpaque() == 0) return false;
    int32_t s = gl->base.currentShader;
    if (s < 0) return vitaBatchAlpha255 && vitaTextureOpaque(gl, m->currentTextureId); // texture * vertex colour
    if ((uint32_t) s >= vitaShaderCount) return false;
    switch (vitaShaderAlpha[s]) {
        case VITA_ALPHA_ONE: return true;
        case VITA_ALPHA_VERTEX: return vitaBatchAlpha255;
        case VITA_ALPHA_PALETTE:
            return vitaPaletteSlot >= 0 && vitaPaletteSlot <= MAX_TEXTURE_STAGES && vitaTextureOpaque(gl, m->stageTexture[vitaPaletteSlot]);
        default: return false;
    }
}

// After a draw into the bound target: alphaOne = its fragments have alpha 1.0, blendOn = GL blending was on for it.
static void vitaNoteBatchWrite(GLModernRenderer* m, bool alphaOne, bool blendOn) {
    GLRenderer* gl = &m->base;
    if (vitaOpaque() == 0 || !gl->colorWriteA) return; // alpha masked off: the target's alpha is unchanged
    int32_t t = vitaBoundSurface(gl);
    if (t < 0) return;
    // bm_normal on alpha too: a * a + d * (1 - a) == 1 for a == 1; blending off writes a
    bool writesOne = alphaOne && (!blendOn || gl->currentBlendMode == bm_normal);
    if (!writesOne) vitaSetSurfaceOpaque(t, false);
    else if (vitaBatchCovers) vitaSetSurfaceOpaque(t, true);
}

// A write this file doesn't analyse (direct draws, vertex buffers, clears): the target's alpha becomes unknown.
static void vitaNoteUnknownWrite(GLRenderer* gl) {
    if (vitaOpaque() != 0) vitaSetSurfaceOpaque(vitaBoundSurface(gl), false);
}

// True if a draw of the rectangle (ax, ay)-(bx, by) (GML coordinates, current matrices and state) covers every pixel
// of the bound target surface and writes all of them: no depth test, culling, alpha test or shader discard, the full
// viewport, no scissor cutting into it, and the extreme pixel centres inside the transformed quad by 1/64 px or more.
static bool vitaDrawCoversTarget(GLModernRenderer* m, int32_t target, float ax, float ay, float bx, float by) {
    GLRenderer* gl = &m->base;
    Renderer* r = &gl->base;
    if (target < 0 || m->depthTest || m->cull) return false;
    int32_t s = r->currentShader;
    if (s < 0 ? gl->alphaTestEnable : ((uint32_t) s >= vitaShaderCount || !vitaShaderNoDiscard[s])) return false;
    float tw = (float) gl->surfaceWidth[target], th = (float) gl->surfaceHeight[target];
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[0] != 0 || vp[1] != 0 || (float) vp[2] != tw || (float) vp[3] != th) return false;
    // vitaGL maps clip space onto 2 * (size >> 1) pixels (glViewport: x_scale = width >> 1, y_scale = height >> 1), so
    // with an odd width / height the last column / row is never drawn (the model below assumes [0, size])
    if ((vp[2] & 1) != 0 || (vp[3] & 1) != 0) return false;
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        GLint sc[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_SCISSOR_BOX, sc);
        if (sc[0] > 0 || sc[1] > 0 || (float) (sc[0] + sc[2]) < tw || (float) (sc[1] + sc[3]) < th) return false;
    }
    const float* M = r->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION].m; // m[col * 4 + row]
    const float z = r->drawDepth;
    const float cx[4] = { ax, bx, bx, ax }, cy[4] = { ay, ay, by, by };
    float px[4], py[4];
    for (int i = 0; i < 4; i++) {
        float X = M[0] * cx[i] + M[4] * cy[i] + M[8] * z + M[12];
        float Y = M[1] * cx[i] + M[5] * cy[i] + M[9] * z + M[13];
        float Z = M[2] * cx[i] + M[6] * cy[i] + M[10] * z + M[14];
        float W = M[3] * cx[i] + M[7] * cy[i] + M[11] * z + M[15];
        // inside the depth range both for GML shaders (z) and the default program (2z - w)
        if (!(W > 0.0f) || Z < -W || Z > W || Z < 0.0f) return false;
        px[i] = (X / W + 1.0f) * 0.5f * tw;
        py[i] = (Y / W + 1.0f) * 0.5f * th;
    }
    float area = 0.0f;
    for (int i = 0; i < 4; i++) area += px[i] * py[(i + 1) & 3] - px[(i + 1) & 3] * py[i];
    if (fabsf(area) < 1.0f) return false;
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    const float qx[4] = { 0.5f, tw - 0.5f, tw - 0.5f, 0.5f }, qy[4] = { 0.5f, 0.5f, th - 0.5f, th - 0.5f };
    for (int e = 0; e < 4; e++) {
        float ex = px[(e + 1) & 3] - px[e], ey = py[(e + 1) & 3] - py[e];
        float len = sqrtf(ex * ex + ey * ey);
        if (!(len > 0.0f)) return false;
        for (int k = 0; k < 4; k++) {
            float dist = sign * (ex * (qy[k] - py[e]) - ey * (qx[k] - px[e])) / len;
            if (!(dist >= 1.0f / 64.0f)) return false;
        }
    }
    return true;
}

#ifdef PLATFORM_VITA
// ===[ Direct final output (Vita) ]===
// With the CRT filter off, mthRenderSimple draws the application surface through shdPalettizer into Pass1 (1:1),
// copies Pass1 into Pass2 (surface_copy_part: a 1:1 nearest draw with blending off) and draws Pass2 onto the screen
// at 512x448(.5). In the pixel-perfect mode that last draw is nearest-sampled too, so screen pixel (x0 + x, y0 + y)
// shows palettizer(application surface texel (x, y)), and the gml edit "direct final" draws exactly that: the
// application surface through shdPalettizer onto the screen, without the Pass1 and Pass2 render passes (two 512x448
// scenes of the frame's five). When the final stretch interpolates it only skips the Pass2 copy (Pass2 == Pass1).
// The edit asks for it with shader_get_uniform(shdPalettizer, VITA_DIRECT_PROBE_NAME) != -1 right where Pass1 would
// be drawn; the answer is yes only in the state in which the three draws give the pixels the one draw gives: texture
// filter off (Pass1 samples the application surface and the palette nearest, as the final draw then does), bm_normal
// with blending on (the palette is opaque, so Pass1 == the palettizer output and the final draw writes it as is),
// no alpha test, fog or colour mask (they act on the default program's final draw only), no shader set. The edit
// itself checks the palettizer is on, the final draw is nearest and the application surface is _a0 x _a1.
// <data dir>/nodirectfinal turns it off (the probe answers -1).
#define VITA_DIRECT_PROBE_NAME "bs_vita_direct_final"
#define VITA_DIRECT_PROBE_HANDLE 0x7FFFFF02 // never a GL location; the uniform setters ignore it

static bool vitaDirectFinalOk(GLModernRenderer* m) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = vitaMarkerFile(BS_VITA_PATH("nodirectfinal")) ? 0 : 1;
        logInfo("GL: direct final output %s\n", enabled ? "on" : "disabled (nodirectfinal)");
    }
    GLRenderer* gl = &m->base;
    Renderer* r = &gl->base;
    return enabled == 1 && !r->texFilter && gl->blendEnable && gl->currentBlendMode == bm_normal && !gl->alphaTestEnable &&
           !m->fogEnable && r->currentShader == -1 && gl->colorWriteR && gl->colorWriteG && gl->colorWriteB && gl->colorWriteA;
}
#endif

// ===[ Deferred screen clear (Vita) ]===
// loop.c clears the screen (colour, host framebuffer) at the start of every frame, before the game draws into the
// application surface. That made the clear a GXM scene of its own on the 960x544 display surface (a full-screen fill
// and a 2 MB store), and the final scene loaded it back wherever nothing opaque covers. Deferred, the same clear
// (colour and scissor state as at the frame start) is issued right before the first GPU work on the host framebuffer
// (vitaBeforeGpuWork in flushBatch, the direct draws and the clears), so it is the first draw of the final scene:
// one scene less per frame, and hidden surface removal drops it under the opaque border / game image. Nothing reads
// the host framebuffer between the frame start and its first draw; loop.c flushes a pending clear before
// screenshots and the swap. <data dir>/nodeferclear turns it off.
static struct {
    int8_t enabled; // -1 marker not read yet
    bool pending;
    GLuint fbo;
    GLfloat color[4];
    GLboolean scissor;
    GLint box[4];
} vitaHostClear = { -1, false, 0, { 0, 0, 0, 0 }, GL_FALSE, { 0, 0, 0, 0 } };

// loop.c, instead of its frame-start glClear: false = clear now as before.
bool bsVitaDeferHostClear(uint32_t hostFbo) {
    if (vitaHostClear.enabled < 0) {
        vitaHostClear.enabled = vitaMarkerFile(BS_VITA_PATH("nodeferclear")) ? 0 : 1;
        logInfo("GL: deferred screen clear %s\n", vitaHostClear.enabled ? "on" : "disabled (nodeferclear)");
    }
    if (!vitaHostClear.enabled) return false;
    vitaHostClear.pending = true;
    vitaHostClear.fbo = (GLuint) hostFbo;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, vitaHostClear.color);
    vitaHostClear.scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, vitaHostClear.box);
    return true;
}

static void vitaIssueHostClear(void) {
    vitaHostClear.pending = false;
    GLfloat color[4];
    GLint box[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glClearColor(vitaHostClear.color[0], vitaHostClear.color[1], vitaHostClear.color[2], vitaHostClear.color[3]);
    glScissor(vitaHostClear.box[0], vitaHostClear.box[1], vitaHostClear.box[2], vitaHostClear.box[3]);
    if (vitaHostClear.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    (glClear)(GL_COLOR_BUFFER_BIT); // not the PERF macro: loop.c's clear was never counted in clear=
    glClearColor(color[0], color[1], color[2], color[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

// Before any GPU work: the pending screen clear goes first if the work is on the host framebuffer.
static void vitaBeforeGpuWork(void) {
    if (!vitaHostClear.pending) return;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    if ((GLuint) fb == vitaHostClear.fbo) vitaIssueHostClear();
}

// loop.c, before reading the screen (screenshots) and before the swap.
void bsVitaFlushHostClear(void) {
    if (!vitaHostClear.pending) return;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    if ((GLuint) fb != vitaHostClear.fbo) (glBindFramebuffer)(GL_FRAMEBUFFER, vitaHostClear.fbo);
    bsTrackBind(vitaHostClear.fbo);
    vitaIssueHostClear();
    bsTrackWork();
    if ((GLuint) fb != vitaHostClear.fbo) (glBindFramebuffer)(GL_FRAMEBUFFER, (GLuint) fb);
    bsTrackBind((uint32_t) fb);
}
#endif

#ifdef PLATFORM_VITA
// ===[ Palette swapper loop variants (Vita) ]===
// shd_pal_swapper (the game's retro_pal_swapper: enemies, bosses, the player's bullets, glitch tiles...) runs on the
// Vita with its palette search as a fixed 64-step loop (glInit: "palette search rewritten as a fixed 64-step loop"):
// step k computes i = corner.y + k * u_pixelSize.y (corner = u_Uvs.xy) and does its work only while found == 0.0 &&
// i < u_Uvs.w. The palettes are 2-12 rows tall (cropped on the texture page), so most of the 64 steps do nothing, yet
// every pixel the shader draws pays for all of them: a lot for the big swapped sprites (Tomcat 192x112, EeeTee
// 224x112: 86k-100k pixels of the 2x application surface). The same source is also compiled with 4, 8 and 16 steps,
// and a draw uses the shortest whose dropped steps provably do nothing: for k >= N, corner.y + k * u_pixelSize.y >=
// corner.y + N * u_pixelSize.y >= u_Uvs.w + u_pixelSize.y (vitaPalSwapPick asks for one palette row to spare, far
// more than the fp32 rounding of i), so the 64-step program skips those steps too: the same pixels. The variants get
// every value the GML program gets (shader_set_uniform_* writes and the built-ins of glApplyGmlProgram are mirrored
// to them by uniform name) and the GML program's sampler units; flushBatch binds the chosen variant for the draw and
// the GML program again after it. <data dir>/nopalswapfast turns it off.
#define VITA_PALSWAP_VARIANTS 3
static const int vitaPalSwapSteps[VITA_PALSWAP_VARIANTS] = { 4, 8, 16 };
static struct {
    int32_t index;                              // GML shader index of shd_pal_swapper when it has variants, else -1
    bool ok[VITA_PALSWAP_VARIANTS];
    GMLShader variant[VITA_PALSWAP_VARIANTS];
    int32_t* map[VITA_PALSWAP_VARIANTS];        // per uniform of the GML program: the variant's location of that name
    int32_t locUvs, locPixelSize;               // the GML program's u_Uvs / u_pixelSize locations
    float uvs[4], pixelSize[4];                 // the values last written to them
    bool haveUvs, havePixelSize;
    bool logged[VITA_PALSWAP_VARIANTS + 1];     // first use of each program logged (+ the 64-step one)
} vitaPalSwap = { .index = -1, .locUvs = -1, .locPixelSize = -1 };

// The variant for the u_Uvs / u_pixelSize values the GML program has now, -1 for the 64-step program itself.
static int vitaPalSwapPick(void) {
    if (!vitaPalSwap.haveUvs || !vitaPalSwap.havePixelSize) return -1;
    double y = vitaPalSwap.uvs[1], w = vitaPalSwap.uvs[3], p = vitaPalSwap.pixelSize[1];
    if (!isfinite(y) || !isfinite(w) || !isfinite(p) || !(p >= 1e-6)) return -1;
    repeat(VITA_PALSWAP_VARIANTS, v)
        if (vitaPalSwap.ok[v] && y + (double) (vitaPalSwapSteps[v] - 1) * p >= w) return (int) v;
    return -1;
}

// The index in the GML program's uniform list of location loc, -1 if it isn't one of its uniforms.
static int32_t vitaPalSwapUniform(GLModernRenderer* m, int32_t loc) {
    if (vitaPalSwap.index < 0 || loc == -1) return -1;
    GMLShader* gml = &m->gmlShaders[vitaPalSwap.index];
    repeat(gml->uniformCount, b) if (gml->uniforms[b].location == loc) return (int32_t) b;
    return -1;
}

static void vitaPalSwapRecord(int32_t loc, const float* values, uint32_t n) {
    float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    memcpy(v, values, sizeof(float) * (n < 4 ? n : 4));
    if (loc == vitaPalSwap.locUvs) { memcpy(vitaPalSwap.uvs, v, sizeof(v)); vitaPalSwap.haveUvs = true; }
    if (loc == vitaPalSwap.locPixelSize) { memcpy(vitaPalSwap.pixelSize, v, sizeof(v)); vitaPalSwap.havePixelSize = true; }
}
#endif

// ===[ Batch Flush ]===

static void flushBatch(GLRenderer* gl) {
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    if (modernGl->batchCount == 0) return;
#ifdef PLATFORM_VITA
    vitaBeforeGpuWork(); // a deferred screen clear goes first ("Deferred screen clear")
    bool vitaAlphaOne = vitaBatchAlphaOne(modernGl); // see "Opaque draws (Vita)"
    int vitaPalSwapDraw = -1;     // "Palette swapper loop variants": the variant this batch is drawn with
    bool vitaPalColorDraw = false; // "colour table": this batch is drawn with the colour-table palettizer
#endif

    if (gl->base.currentShader != -1) {
        GMLShader* shader = &modernGl->gmlShaders[gl->base.currentShader];

        if (fastPalettizerActive(modernGl)) {
            GLuint table = palettizerTable(modernGl, modernGl->stageTexture[modernGl->palPaletteSlot]);
#ifdef PLATFORM_VITA
            GLuint colours = table != 0 ? palettizerColorTable(modernGl, modernGl->stageTexture[modernGl->palPaletteSlot]) : 0;
            if (colours != 0) {
                glApplyGmlProgram(&gl->base, &modernGl->colorPalettizer); // its built-ins from the current state
                table = colours; // read through the same texture unit
                vitaPalColorDraw = true;
            }
#endif
            if (table != 0) {
                glActiveTexture(GL_TEXTURE0 + modernGl->palLutSlot);
                glBindTexture(GL_TEXTURE_2D, table);
            } else {
                glApplyGmlProgram(&gl->base, shader); // no table (yet): this draw uses the original shader
            }
        }

        GLShaderUniform* uniform = shader->gmBaseTexture;
        if (uniform != nullptr)
            glActiveTexture(GL_TEXTURE0 + uniform->samplerSlot);
        glBindTexture(GL_TEXTURE_2D, modernGl->currentTextureId);
#ifdef PLATFORM_VITA
        if (gl->base.currentShader == vitaPalSwap.index) {
            vitaPalSwapDraw = vitaPalSwapPick();
            if (!vitaPalSwap.logged[vitaPalSwapDraw + 1]) {
                vitaPalSwap.logged[vitaPalSwapDraw + 1] = true;
                logInfo("GL: shd_pal_swapper: first draw with the %d-step loop (u_Uvs.y %.6f .w %.6f, u_pixelSize.y %.6f)\n",
                        vitaPalSwapDraw >= 0 ? vitaPalSwapSteps[vitaPalSwapDraw] : 64, (double) vitaPalSwap.uvs[1],
                        (double) vitaPalSwap.uvs[3], (double) vitaPalSwap.pixelSize[1]);
            }
            if (vitaPalSwapDraw >= 0) glUseProgram(vitaPalSwap.variant[vitaPalSwapDraw].shaderId);
        }
#endif
    } else {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, modernGl->currentTextureId);
        glUniform1i(modernGl->uLocalUV, modernGl->batchLocalUV);
        if (modernGl->batchLocalUV) glUniform4fv(modernGl->uTextureRect, 1, modernGl->batchUVRect);
    }

    int32_t indexCount = modernGl->batchCount * INDICES_PER_QUAD;

#ifdef PLATFORM_VITA
    // Same output as the default program in this state (see "fast default program" in glInit).
    bool fastDefault = gl->base.currentShader == -1 && modernGl->fastDefaultProgram != nullptr &&
        !modernGl->batchLocalUV && !gl->alphaTestEnable && !modernGl->fogEnable;
    if (fastDefault) glUseProgram(modernGl->fastDefaultProgram->shaderId);
#endif

    glBindBuffer(GL_ARRAY_BUFFER, modernGl->vbo);
#ifdef PLATFORM_VITA
    int32_t totalVboSize = MAX_QUADS * VERTICES_PER_QUAD * sizeof(GlVertex);
    GlVertex* batchVertices = gl->vertexData;
    vglBufferData(GL_ARRAY_BUFFER, (void*)gl->vertexData);
    gl->vertexData = (GlVertex*)vglAllocFromScratch((size_t)totalVboSize);
    //glBufferData(GL_ARRAY_BUFFER, totalVboSize, (void*)gl->vertexData, GL_DYNAMIC_DRAW);
#else
    int32_t singleVertexCount = (modernGl->batchType == BATCHTYPE_QUAD) ? VERTICES_PER_QUAD : VERTICES_PER_TRIANGLE;
    int32_t vertexCount = modernGl->batchCount * singleVertexCount;
    glBufferData(GL_ARRAY_BUFFER, vertexCount * sizeof(GlVertex), gl->vertexData, GL_STREAM_DRAW);
#endif


    if (hasVAO()) {
        glBindVertexArray(modernGl->vao);
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modernGl->ebo);

        int32_t stride = sizeof(GlVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(GlVertex, x));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*) offsetof(GlVertex, r));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(GlVertex, u));
        glEnableVertexAttribArray(2);
    }

#ifdef PLATFORM_VITA
    // CRT bake: bake / plasma passes when needed, and the textures the baked pass program reads
    if (gl->base.currentShader != -1 && modernGl->gmlShaders[gl->base.currentShader].vitaBake != nullptr)
        vitaCrtBakeBeforeDraw(modernGl, &modernGl->gmlShaders[gl->base.currentShader], batchVertices,
                              modernGl->batchType == BATCHTYPE_QUAD, modernGl->batchCount);
#endif

#ifdef PLATFORM_VITA
    // alpha 1 under bm_normal: src * 1 + dst * 0 == src, so blending off gives the same pixels without the background
    // load and the blend code (see "Opaque draws (Vita)")
    bool vitaBlendOn = glIsEnabled(GL_BLEND) == GL_TRUE;
    bool vitaUnblend = vitaAlphaOne && vitaBlendOn && gl->blendEnable && gl->currentBlendMode == bm_normal;
    if (vitaUnblend) glDisable(GL_BLEND);
#endif
    if (modernGl->batchType == BATCHTYPE_QUAD) {
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, nullptr);
    } else if (modernGl->batchType == BATCHTYPE_TRIANGLE) {
        glDrawArrays(GL_TRIANGLES, 0, modernGl->batchCount * VERTICES_PER_TRIANGLE);
    }
#ifdef PLATFORM_VITA
    if (vitaUnblend) glEnable(GL_BLEND);
    if (vitaUnblend || !vitaBlendOn) bsGlUnblended++;
    vitaNoteBatchWrite(modernGl, vitaAlphaOne, vitaBlendOn);
    if (vitaPalSwapDraw >= 0) glUseProgram(modernGl->gmlShaders[vitaPalSwap.index].shaderId);
    if (vitaPalColorDraw) glApplyGmlProgram(&gl->base, &modernGl->fastPalettizer); // what glGpuSetShader bound
#endif

    if (!hasVAO()) {
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
    }
#ifdef PLATFORM_VITA
    if (fastDefault) glUseProgram(modernGl->defaultShaderProgram->shaderId);
#endif

    modernGl->batchCount = 0;
    if (gl->base.currentShader == -1 && modernGl->batchLocalUV) glUniform1i(modernGl->uLocalUV, 0);
}

static void flushIfNeededAndSetActiveState(GLRenderer* gl, BatchType batchType, GLuint textureId) {
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;
    
    if (modernGl->batchCount != 0) {
        // TODO: This should be changed down the road from MAX_QUADS to MAX_WHATEVER_BATCH_TYPE_ARE_WE_USING
        if (modernGl->batchType != batchType || modernGl->currentTextureId != textureId || modernGl->batchLocalUV || modernGl->batchCount == MAX_QUADS) {
            flushBatch(gl);
        }
    }

    modernGl->batchType = batchType;
    modernGl->currentTextureId = textureId;
    modernGl->batchLocalUV = false;
}

static bool glResolveTextureHandle(GLRenderer* gl, uint32_t texHandle, TexturePageItem** outTpag, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH);

static GLenum primitiveTypeToGL(int32_t primitiveType) {
    switch (primitiveType) {
        case PRIMITIVE_POINTS:
            return GL_POINTS;
        case PRIMITIVE_LINES:
            return GL_LINES;
        case PRIMITIVE_LINE_STRIP:
            return GL_LINE_STRIP;
        case PRIMITIVE_TRIANGLES:
            return GL_TRIANGLES;
        case PRIMITIVE_TRIANGLE_STRIP:
            return GL_TRIANGLE_STRIP;
        case PRIMITIVE_TRIANGLE_FAN:
            return GL_TRIANGLE_FAN;
        default:
            return GL_TRIANGLES;
    }
}

static void glPrimitiveBegin(Renderer* renderer, int32_t primitiveType) {
    GLRenderer* gl = (GLRenderer*) renderer;
    ((GLModernRenderer*)gl)->primitiveLocalUV = false;
    GLCommon_primitiveBegin(&gl->currentPrimitive, primitiveType, gl->whiteTexture);
}

static bool glResolvePrimitiveTexture(GLRenderer* gl, int32_t texture, GLuint* textureId, float* rect) {
    if (texture <= 0)
        return false;

    TexturePageItem* tpag = nullptr;
    int32_t texW = 0;
    int32_t texH = 0;

    if (glResolveTextureHandle(
            gl, (uint32_t)texture,
            &tpag, textureId, &texW, &texH)) {

        if (tpag && texW > 0 && texH > 0) {
            rect[0] = (float)tpag->sourceX / texW;
            rect[1] = (float)tpag->sourceY / texH;
            rect[2] = (float)tpag->sourceWidth / texW;
            rect[3] = (float)tpag->sourceHeight / texH;
        }
        return *textureId != 0;
    }

    if (glIsTexture((GLuint)texture)) {
        *textureId = (GLuint)texture;
        return true;
    }

    return false;
}

static void glPrimitiveBeginTexture(
    Renderer* renderer,
    int32_t primitiveType,
    int32_t texture
) {
    GLRenderer* gl = (GLRenderer*)renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*)gl;
    modernGl->primitiveLocalUV = renderer->legacyTextureCoordinates;
    modernGl->primitiveUVRect[0] = modernGl->primitiveUVRect[1] = 0;
    modernGl->primitiveUVRect[2] = modernGl->primitiveUVRect[3] = 1;
    GLuint texId = 0;
    glResolvePrimitiveTexture(gl, texture, &texId, modernGl->primitiveUVRect);
    GLCommon_primitiveBeginTexture(gl, primitiveType, texId);
}

static void glBatchPrimitiveTriangle(GLModernRenderer* modernGl, GLuint textureId, int a, int b, int c) {
    GLRenderer* gl = &modernGl->base;
    bool localUV = modernGl->primitiveLocalUV;
    if (modernGl->batchCount != 0 &&
        (modernGl->batchType != BATCHTYPE_TRIANGLE || modernGl->currentTextureId != textureId ||
         modernGl->batchLocalUV != localUV || modernGl->batchCount == MAX_QUADS ||
         (localUV && memcmp(modernGl->batchUVRect, modernGl->primitiveUVRect, sizeof(modernGl->batchUVRect)) != 0))) {
        flushBatch(gl);
    }
#ifdef PLATFORM_VITA
    if (modernGl->batchCount == 0) vitaBatchCovers = false;
    vitaBatchAlpha255 = false; // triangle batches aren't analysed
#endif
    modernGl->batchType = BATCHTYPE_TRIANGLE;
    modernGl->currentTextureId = textureId;
    modernGl->batchLocalUV = localUV;
    if (localUV) memcpy(modernGl->batchUVRect, modernGl->primitiveUVRect, sizeof(modernGl->batchUVRect));
    GlVertex* target = gl->vertexData + modernGl->batchCount * VERTICES_PER_TRIANGLE;
    target[0] = modernGl->primitiveVertices[a];
    target[1] = modernGl->primitiveVertices[b];
    target[2] = modernGl->primitiveVertices[c];
    modernGl->batchCount++;
}

static void glPrimitiveEnd(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*)renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*)gl;

    GLenum mode;
    GLuint textureId;

    if (!GLCommon_primitivePrepare(
            &gl->currentPrimitive,
            gl->whiteTexture,
            &mode,
            &textureId))
        return;

    // Keep compatible geometry in the ordinary triangle batch. Separate
    // strips/fans are expanded with their original winding and vertex data.
    int32_t count = gl->currentPrimitive.vertexCount;
    if (renderer->currentShader == -1 && mode == GL_TRIANGLES) {
        for (int32_t i = 0; i + 2 < count; i += 3) glBatchPrimitiveTriangle(modernGl, textureId, i, i+1, i+2);
    } else if (renderer->currentShader == -1 && mode == GL_TRIANGLE_STRIP) {
        for (int32_t i = 0; i + 2 < count; i++) glBatchPrimitiveTriangle(modernGl, textureId, i+(i&1), i+1-(i&1), i+2);
    } else if (renderer->currentShader == -1 && mode == GL_TRIANGLE_FAN) {
        for (int32_t i = 1; i + 1 < count; i++) glBatchPrimitiveTriangle(modernGl, textureId, 0, i, i+1);
    } else {
        // Points, lines and custom shaders retain separate draw calls/topology.
        flushBatch(gl);
#ifdef PLATFORM_VITA
        vitaBeforeGpuWork();
#endif

        GLShaderUniform* sampler = renderer->currentShader == -1 ? nullptr
            : modernGl->gmlShaders[renderer->currentShader].gmBaseTexture;
        glActiveTexture(sampler ? GL_TEXTURE0 + sampler->samplerSlot : GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glBindBuffer(GL_ARRAY_BUFFER, modernGl->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(count * sizeof(GlVertex)), modernGl->primitiveVertices, GL_DYNAMIC_DRAW);

        if (hasVAO()) {
            glBindVertexArray(modernGl->vao);
        } else {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modernGl->ebo);
            int32_t stride = sizeof(GlVertex);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GlVertex, x));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GlVertex, r));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GlVertex, u));
            glEnableVertexAttribArray(2);
        }

        if (renderer->currentShader == -1 && modernGl->primitiveLocalUV) {
            glUniform4fv(modernGl->uTextureRect, 1, modernGl->primitiveUVRect);
            glUniform1i(modernGl->uLocalUV, 1);
        }
        glDrawArrays(mode, 0, count);
#ifdef PLATFORM_VITA
        vitaNoteUnknownWrite(gl);
#endif
        if (renderer->currentShader == -1) glUniform1i(modernGl->uLocalUV, 0);

        if (!hasVAO()) {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
        }
    }

    gl->currentPrimitive.vertexCount = 0;
}

static void glDrawVertex(Renderer* renderer, float x, float y, float z, uint32_t color, float alpha, float u, float v) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;
    if (gl->currentPrimitive.vertexCount < 0) {
        gl->currentPrimitive.vertexCount = 0;
    }
    if ((size_t)gl->currentPrimitive.vertexCount >= modernGl->primitiveVertexCapacity) {
        modernGl->primitiveVertexCapacity = modernGl->primitiveVertexCapacity ? modernGl->primitiveVertexCapacity * 2 : 256;
        modernGl->primitiveVertices = (GlVertex*)safeRealloc(modernGl->primitiveVertices, modernGl->primitiveVertexCapacity * sizeof(GlVertex));
    }
    GlVertex* vertex = &modernGl->primitiveVertices[gl->currentPrimitive.vertexCount++];
    vertex->x = x; vertex->y = y; vertex->z = z;
    vertex->u = u; vertex->v = v;
    vertex->r = (uint8_t)BGR_R(color);
    vertex->g = (uint8_t)BGR_G(color);
    vertex->b = (uint8_t)BGR_B(color);
    vertex->a = floatToUnormByte(alpha);
}

// ===[ Vtable Implementations ]===

// Boot timing: programs compiled (or loaded from vitaGL's shader cache) and the time it took
static uint32_t compileCount = 0;
static uint64_t compileNs = 0, compileSlowestNs = 0;
static char compileSlowest[64] = ""; // a copy: some callers pass a stack buffer (vitaCrtBakeCreate's tag)

static bool compileProgramImpl(GMLShader* gmlShader, const char* name, const char* vertexShaderSource, const char* fragmentShaderSource, uint32_t vertexAttributeCount, const char** vertexAttributes);

static bool compileProgram(GMLShader* gmlShader, const char* name, const char* vertexShaderSource, const char* fragmentShaderSource, uint32_t vertexAttributeCount, const char** vertexAttributes) {
    bsBootTick(); // shader compiles are a long step of the first boot (later boots read vitaGL's shader cache)
    uint64_t t0 = nowNanos();
    bool ok = compileProgramImpl(gmlShader, name, vertexShaderSource, fragmentShaderSource, vertexAttributeCount, vertexAttributes);
    uint64_t ns = nowNanos() - t0;
    compileCount++;
    compileNs += ns;
    if (ns > compileSlowestNs) { compileSlowestNs = ns; snprintf(compileSlowest, sizeof(compileSlowest), "%s", name != nullptr ? name : "?"); }
    return ok;
}

static bool compileProgramImpl(GMLShader* gmlShader, const char* name, const char* vertexShaderSource, const char* fragmentShaderSource, uint32_t vertexAttributeCount, const char** vertexAttributes) {
    logInfo("GL: Compiling %s vertex shader\n", name);
    bool vertexShaderOK = false;
    bool fragmentShaderOK = false;
    GLuint vertShaderT = compileShader(GL_VERTEX_SHADER, vertexShaderSource, &vertexShaderOK);
    logInfo("GL: Compiling %s fragment shader\n", name);
    // Compiled even when the vertex shader failed: under vitaGL's VGL_MODE_SHADER_PAIR (the Vita CRT rewrites) the
    // GLSL translator pairs each vertex shader with the next fragment shader it translates.
    GLuint fragShaderT = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource, &fragmentShaderOK);
    if (!vertexShaderOK || !fragmentShaderOK) {
        logError("GL: Failed to compile %s %s shader!\n", name, vertexShaderOK ? "fragment" : "vertex");
        if (vertexShaderOK) glDeleteShader(vertShaderT);
        if (fragmentShaderOK) glDeleteShader(fragShaderT);
        return false;
    }

    bool success;
    GLuint shaderId = linkProgram(name, vertexAttributeCount, vertexAttributes, vertShaderT, fragShaderT, &success);
    glDeleteShader(vertShaderT);
    glDeleteShader(fragShaderT);
    //Texture Set Stage BS has to be done bruh :(
    int32_t samplerIndex = 0;
    GLint uniformCount;
    glGetProgramiv(shaderId, GL_ACTIVE_UNIFORMS, &uniformCount);

    gmlShader->uniformCount = uniformCount;
    gmlShader->uniforms = (GLShaderUniform *)safeCalloc(uniformCount, sizeof(GLShaderUniform));

    // We can only get the length of a specific uniform in OpenGL 4.3+...
    GLint maxUniformNameLength = 0;
    glGetProgramiv(shaderId, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxUniformNameLength);

    repeat(uniformCount, b) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;

        char* uniformName = (char *)safeMalloc(maxUniformNameLength + 1);

        glGetActiveUniform(shaderId, b, maxUniformNameLength, &length, &size, &type, uniformName);

        gmlShader->uniforms[b].location = glGetUniformLocation(shaderId, uniformName);
        gmlShader->uniforms[b].name = uniformName;
        gmlShader->uniforms[b].type = type;

        if (type == GL_SAMPLER_2D) {
            glUseProgram(shaderId);
            glUniform1i(gmlShader->uniforms[b].location, samplerIndex);
            gmlShader->uniforms[b].samplerSlot = samplerIndex;
            samplerIndex += 1;
        }

        if (strcmp(uniformName, "gm_BaseTexture") == 0)
            gmlShader->gmBaseTexture = &gmlShader->uniforms[b];
#ifdef PLATFORM_VITA
        if (strcmp(uniformName, "gm_Matrices") == 0)
#else
        if (strcmp(uniformName, "gm_Matrices[0]") == 0)
#endif
            gmlShader->gmMatrices = &gmlShader->uniforms[b];
        if (strcmp(uniformName, "gm_FogColour") == 0)
            gmlShader->gmFogColour = &gmlShader->uniforms[b];
        if (strcmp(uniformName, "gm_AlphaTestEnabled") == 0)
            gmlShader->gmAlphaTestEnabled = &gmlShader->uniforms[b];
        if (strcmp(uniformName, "gm_AlphaRefValue") == 0)
            gmlShader->gmAlphaRefValue = &gmlShader->uniforms[b];
    }

    gmlShader->shaderId = shaderId;
    gmlShader->compiled = true;
    return true;
}

#ifdef PLATFORM_VITA
static void vitaPalSwapFree(void) {
    repeat(VITA_PALSWAP_VARIANTS, v) {
        if (vitaPalSwap.ok[v]) freeShader(&vitaPalSwap.variant[v]);
        free(vitaPalSwap.map[v]);
    }
    memset(&vitaPalSwap, 0, sizeof(vitaPalSwap));
    vitaPalSwap.index = vitaPalSwap.locUvs = vitaPalSwap.locPixelSize = -1;
}

// "Palette swapper loop variants (Vita)": compiles the 4 / 8 / 16-step programs from the 64-step source of GML
// shader `index` (already compiled) and maps their uniforms onto its uniforms by name. A variant that SceShaccCg
// rejects, or whose uniforms differ in any way from the GML program's, is not used.
static void vitaPalSwapInit(GLModernRenderer* m, int32_t index, const char* name, const char* vs, const char* fs,
                            uint32_t attributeCount, const char** attributes) {
    vitaPalSwapFree(); // a renderer created again (game_change) starts from nothing
    if (vitaMarkerFile(BS_VITA_PATH("nopalswapfast"))) {
        logInfo("GL: %s: short-loop variants disabled (nopalswapfast)\n", name);
        return;
    }
    static const char* loop = "for (int k = 0; k < 64; k++)";
    GMLShader* gml = &m->gmlShaders[index];
    if (vitaCountOf(fs, loop) != 1) {
        logWarn("GL: %s: 64-step loop not found, no short-loop variants\n", name);
        return;
    }
    int ready = 0;
    repeat(VITA_PALSWAP_VARIANTS, v) {
        char to[64], label[96];
        snprintf(to, sizeof(to), "for (int k = 0; k < %d; k++)", vitaPalSwapSteps[v]);
        snprintf(label, sizeof(label), "%s (%d steps)", name, vitaPalSwapSteps[v]);
        int replaced = 0;
        char* src = vitaReplaceAll(fs, loop, to, &replaced);
        GMLShader* var = &vitaPalSwap.variant[v];
        // SHADER_PAIR as for the CRT rewrites: a source SceShaccCg rejects fails here instead of leaving a NULL program
        vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
        bool ok = replaced == 1 && compileProgram(var, label, vs, src, attributeCount, attributes);
        vglSetSemanticBindingMode(VGL_MODE_POSTPONED); // vita/main.c's mode
        free(src);
        if (!ok) {
            logWarn("GL: %s: %d-step variant not used (did not compile)\n", name, vitaPalSwapSteps[v]);
            continue;
        }
        int32_t* map = (int32_t*) safeMalloc(sizeof(int32_t) * (gml->uniformCount + 1));
        bool same = var->uniformCount == gml->uniformCount;
        glUseProgram(var->shaderId);
        repeat(gml->uniformCount, b) {
            map[b] = -1;
            repeat(var->uniformCount, c) {
                if (strcmp(var->uniforms[c].name, gml->uniforms[b].name) != 0) continue;
                if (var->uniforms[c].type != gml->uniforms[b].type) same = false;
                map[b] = var->uniforms[c].location;
                if (gml->uniforms[b].type == GL_SAMPLER_2D) { // the GML program's texture units (texture_set_stage binds by them)
                    var->uniforms[c].samplerSlot = gml->uniforms[b].samplerSlot;
                    glUniform1i(var->uniforms[c].location, (GLint) gml->uniforms[b].samplerSlot);
                }
            }
            if (map[b] == -1) same = false;
        }
        if (!same) {
            logWarn("GL: %s: %d-step variant not used (its uniforms differ)\n", name, vitaPalSwapSteps[v]);
            free(map);
            freeShader(var);
            memset(var, 0, sizeof(*var));
            continue;
        }
        vitaPalSwap.map[v] = map;
        vitaPalSwap.ok[v] = true;
        ready++;
    }
    if (ready == 0) return;
    vitaPalSwap.index = index;
    vitaPalSwap.locUvs = glGetUniformLocation(gml->shaderId, "u_Uvs");
    vitaPalSwap.locPixelSize = glGetUniformLocation(gml->shaderId, "u_pixelSize");
    logInfo("GL: %s: short-loop variants ready (%d of %d: 4/8/16 steps)\n", name, ready, VITA_PALSWAP_VARIANTS);
}
#endif

// ===[ shdPalettizer fast path ]===
// The game draws the whole 512x448 frame through shdPalettizer every frame: per pixel it samples 56 palette
// entries and keeps the nearest (a distance() per entry). On the Vita's GPU that is the most expensive pass of the
// frame. Here the same search is run once per palette on the CPU over a 128-levels-per-channel colour cube into a
// table of palette indices (4 indices per RGBA texel: 1024x512, 2 MB), and the frame pass becomes: quantise the
// pixel, look its index up, sample the palette at that index exactly as the original does. Every palette colour
// maps to itself exactly (the palette's closest pair is 4/255 apart, a table cell spans 2/255); other colours get
// the result of their cell's representative, which can only differ right at a boundary between two palette colours.
// (Building the table on the GPU instead tripped over vitaGL state: vertex attributes land in the renderer's VAO.)
// Disabled by <data dir>/nofastpal (Vita) or BS_NO_FAST_PALETTIZER (env) for A/B comparisons.

#define PAL_LUT_W 1024
#define PAL_LUT_H 512
#define PAL_SAMPLES 56   // shdPalettizer's palSize

static const char* palFastFragment =
    "precision mediump float;\n"
    "uniform sampler2D gm_BaseTexture;\n"
    "uniform sampler2D spritePalette;\n"
    "uniform sampler2D bs_PaletteLut;\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "void main()\n"
    "{\n"
    "    vec3 cell = floor(floor(texture2D(gm_BaseTexture, v_vTexcoord).rgb * 255.0 + 0.5) * 0.5);\n"
    "    highp vec2 texel = vec2(floor(cell.r * 0.25) + 32.0 * mod(cell.b, 32.0), cell.g + 128.0 * floor(cell.b / 32.0));\n"
    "    vec4 indices = texture2D(bs_PaletteLut, (texel + 0.5) / vec2(1024.0, 512.0));\n"
    "    vec4 laneMask = vec4(1.0) - min(abs(vec4(mod(cell.r, 4.0)) - vec4(0.0, 1.0, 2.0, 3.0)), vec4(1.0));\n"
    "    float i = floor(dot(indices, laneMask) * 255.0 + 0.5);\n"
    "    gl_FragColor = texture2D(spritePalette, vec2(i / float(56), .5));\n"
    "}\n";

#ifdef PLATFORM_VITA
// Colour table (Vita). With the texture filter off (how the game draws Pass1) the table version above ends in a fixed
// function of the pixel's cell: index = table[cell], colour = the palette's texel floor(index / 56 * width) as the
// nearest sample returns it. The colour table holds that colour per cell (128^3 cells, 1024x2048 RGBA8, 8 MB of
// vitaGL texture memory, filled by the table job from the index table and the palette's texels), so the frame pass
// is: the same cell expression, one lookup, done: no lane select, no index decode, no dependent palette read. Each
// cell's entry is the colour the index path writes for it, so the pixels are the same. Filter on, no colour table
// (yet, or not enough texture memory), or <data dir>/nopalcolor: the index table path as before.
#define PAL_COLOR_W 1024
#define PAL_COLOR_H 2048

static const char* palColorFragment =
    "precision mediump float;\n"
    "uniform sampler2D gm_BaseTexture;\n"
    "uniform sampler2D bs_PaletteColors;\n"
    "varying vec2 v_vTexcoord;\n"
    "varying vec4 v_vColour;\n"
    "void main()\n"
    "{\n"
    "    vec3 cell = floor(floor(texture2D(gm_BaseTexture, v_vTexcoord).rgb * 255.0 + 0.5) * 0.5);\n"
    "    highp vec2 texel = vec2(cell.r + 128.0 * mod(cell.b, 8.0), cell.g + 128.0 * floor(cell.b / 8.0));\n"
    "    gl_FragColor = texture2D(bs_PaletteColors, (texel + 0.5) / vec2(1024.0, 2048.0));\n"
    "}\n";

// Texel (r + 128 * (b % 8), g + 128 * (b / 8)) = rgba[index of cell (r, g, b)], from the index table's packing
// (palettizerFillTable: row g + 128 * (b / 32), byte 32 * (b % 32) * 4 + r).
static void palettizerFillColors(const uint8_t* table, const uint8_t rgba[PAL_SAMPLES][4], uint32_t* out) {
    uint32_t entry[PAL_SAMPLES];
    repeat(PAL_SAMPLES, i) memcpy(&entry[i], rgba[i], 4);
    for (int32_t cb = 0; cb < 128; cb++) {
        for (int32_t cg = 0; cg < 128; cg++) {
            const uint8_t* row = table + (size_t) (cg + 128 * (cb / 32)) * PAL_LUT_W * 4 + (size_t) (32 * (cb % 32)) * 4;
            uint32_t* dst = out + (size_t) (cg + 128 * (cb / 8)) * PAL_COLOR_W + (size_t) (128 * (cb % 8));
            for (int32_t cr = 0; cr < 128; cr++) dst[cr] = entry[row[cr] < PAL_SAMPLES ? row[cr] : 0];
        }
    }
}

// The colour-table program: shdPalettizer's vertex shader, gm_BaseTexture on the original's unit, the colour table on
// the index table's unit (palLutSlot).
static void initColorPalettizer(GLModernRenderer* m, GMLShader* orig, Shader* shdr) {
    m->colorPalettizerOk = false;
    if (vitaMarkerFile(BS_VITA_PATH("nopalcolor"))) { logInfo("GL: shdPalettizer colour table disabled (nopalcolor)\n"); return; }
    GMLShader* p = &m->colorPalettizer;
    memset(p, 0, sizeof(*p));
    vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR); // a rejected source fails here (see the CRT rewrites)
    bool ok = compileProgram(p, "shdPalettizer (colour table)", shdr->glslES_Vertex, palColorFragment,
                             shdr->vertexAttributeCount, shdr->vertexAttributes);
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED); // vita/main.c's mode
    if (!ok) { logWarn("GL: shdPalettizer colour table program did not compile, using the index table\n"); return; }
    bool base = false, colors = false;
    glUseProgram(p->shaderId);
    repeat(p->uniformCount, b) {
        GLShaderUniform* u = &p->uniforms[b];
        if (strcmp(u->name, "gm_BaseTexture") == 0) { u->samplerSlot = orig->gmBaseTexture->samplerSlot; base = true; }
        else if (strcmp(u->name, "bs_PaletteColors") == 0) { u->samplerSlot = m->palLutSlot; colors = true; }
        else continue;
        glUniform1i(u->location, (GLint) u->samplerSlot);
    }
    if (!base || !colors || p->gmBaseTexture == nullptr) {
        logWarn("GL: shdPalettizer colour table program lacks its samplers, using the index table\n");
        freeShader(p);
        memset(p, 0, sizeof(*p));
        return;
    }
    m->colorPalettizerOk = true;
    logInfo("GL: shdPalettizer colour table program ready\n");
}
#endif

static void initFastPalettizer(GLModernRenderer* m, DataWin* dataWin) {
    m->palettizerIndex = -1;
#ifndef PLATFORM_VITA
    (void) dataWin;
    return; // reads the palette texels through vglGetTexDataPointer
#else
    FILE* off = fopen(BS_VITA_PATH("nofastpal"), "r");
    if (off != nullptr) { fclose(off); logInfo("GL: shdPalettizer fast path disabled (nofastpal)\n"); return; }
    int32_t index = -1;
    repeat(dataWin->shdr.count, i) {
        Shader* shdr = &dataWin->shdr.shaders[i];
        if (shdr->present && shdr->name != nullptr && strcmp(shdr->name, "shdPalettizer") == 0) index = (int32_t) i;
    }
    if (index < 0 || !m->gmlShaders[index].compiled) return;
    GMLShader* orig = &m->gmlShaders[index];
    // The original's own sampler slots: texture_set_stage(shader_get_sampler_index(...)) binds by them.
    int32_t paletteSlot = -1;
    repeat(orig->uniformCount, b) if (strcmp(orig->uniforms[b].name, "spritePalette") == 0) paletteSlot = (int32_t) orig->uniforms[b].samplerSlot;
    if (orig->gmBaseTexture == nullptr || paletteSlot < 0) return;
    m->palPaletteSlot = (uint32_t) paletteSlot;
    m->palLutSlot = MAX_TEXTURE_STAGES - 1;

    Shader* shdr = &dataWin->shdr.shaders[index];
    if (!compileProgram(&m->fastPalettizer, "shdPalettizer (table)", shdr->glslES_Vertex, palFastFragment,
            shdr->vertexAttributeCount, shdr->vertexAttributes)) return;
    GMLShader* fast = &m->fastPalettizer;
    glUseProgram(fast->shaderId);
    repeat(fast->uniformCount, b) {
        GLShaderUniform* u = &fast->uniforms[b];
        uint32_t slot = u->samplerSlot;
        if (strcmp(u->name, "gm_BaseTexture") == 0) slot = orig->gmBaseTexture->samplerSlot;
        else if (strcmp(u->name, "spritePalette") == 0) slot = m->palPaletteSlot;
        else if (strcmp(u->name, "bs_PaletteLut") == 0) slot = m->palLutSlot;
        else continue;
        u->samplerSlot = slot;
        glUniform1i(u->location, (GLint) slot);
    }
    m->palettizerIndex = index;
    logInfo("GL: shdPalettizer fast path ready (palette slot %u, table slot %u)\n", m->palPaletteSlot, m->palLutSlot);
    initColorPalettizer(m, orig, shdr);
#endif
}

#ifdef PLATFORM_VITA
// The 56 colours shdPalettizer's loop samples (texture2D(spritePalette, vec2(i / 56.0, .5))), in 0..255 units,
// with the texture's current filter. False if the palette texture isn't a loaded single-row RGBA page.
static bool palettizerSamples(GLModernRenderer* m, GLuint palette, bool filter, float out[PAL_SAMPLES][3], uint8_t rgba[PAL_SAMPLES][4]) {
    GLRenderer* gl = &m->base;
    int32_t page = -1;
    repeat(gl->textureCount, i) if (gl->textureLoaded[i] && gl->glTextures[i] == palette) { page = (int32_t) i; break; }
    if (page < 0 || gl->textureHeights[page] != 1 || gl->textureWidths[page] <= 0) return false;
    int32_t w = gl->textureWidths[page];
    GLint prevUnit;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0 + m->palLutSlot);
    glBindTexture(GL_TEXTURE_2D, palette);
    const uint8_t* texels = (const uint8_t*) vglGetTexDataPointer(GL_TEXTURE_2D); // linear RGBA8, row 0
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture((GLenum) prevUnit);
    if (texels == nullptr) return false;
    repeat(PAL_SAMPLES, i) {
        if (!filter) {
            int32_t x = (int32_t) ((i * w) / PAL_SAMPLES); // floor(u * w); exact: u*w is an integer only for i = 0, 28
            repeat(3, c) out[i][c] = texels[x * 4 + c];
            if (rgba != nullptr) repeat(4, c) rgba[i][c] = texels[x * 4 + c]; // the colour table's entries
        } else {
            float pos = (float) i / PAL_SAMPLES * (float) w - 0.5f;
            int32_t x0 = (int32_t) floorf(pos);
            float f = pos - (float) x0;
            int32_t xa = x0 < 0 ? 0 : (x0 >= w ? w - 1 : x0), xb = x0 + 1 >= w ? w - 1 : (x0 + 1 < 0 ? 0 : x0 + 1);
            repeat(3, c) out[i][c] = texels[xa * 4 + c] * (1.0f - f) + texels[xb * 4 + c] * f;
        }
    }
    return true;
}
#endif

#ifdef PLATFORM_VITA
// Same search as the shader (nearest by Euclidean distance, first index wins ties) for each cell's representative
// (2*cell)/255, packed 4 indices per texel. Per (g, b) row the palette is ordered by its g/b distance, so the r loop
// stops as soon as that part alone exceeds the best distance found.
static void palettizerFillTable(const float pal[PAL_SAMPLES][3], uint8_t* table) {
    int32_t order[PAL_SAMPLES];
    float gb[PAL_SAMPLES];
    for (int32_t cb = 0; cb < 128; cb++) {
        for (int32_t cg = 0; cg < 128; cg++) {
            float g = (float) (2 * cg), b = (float) (2 * cb);
            repeat(PAL_SAMPLES, i) {
                float dg = pal[i][1] - g, db = pal[i][2] - b;
                gb[i] = dg * dg + db * db;
                int32_t j = (int32_t) i;
                while (j > 0 && (gb[order[j - 1]] > gb[i] || (gb[order[j - 1]] == gb[i] && order[j - 1] > (int32_t) i))) { order[j] = order[j - 1]; j--; }
                order[j] = (int32_t) i;
            }
            int32_t y = cg + 128 * (cb / 32);
            uint8_t* row = table + (size_t) y * PAL_LUT_W * 4 + (size_t) (32 * (cb % 32)) * 4;
            for (int32_t cr = 0; cr < 128; cr++) {
                float r = (float) (2 * cr);
                float best = 1e30f;
                int32_t bestIndex = 0;
                repeat(PAL_SAMPLES, k) {
                    int32_t i = order[k];
                    if (gb[i] > best) break;
                    float dr = pal[i][0] - r;
                    float d = dr * dr + gb[i];
                    if (d < best || (d == best && i < bestIndex)) { best = d; bestIndex = i; }
                }
                row[cr] = (uint8_t) bestIndex; // texel cr/4, lane cr%4 = byte cr of this 32-texel run
            }
        }
    }
}

// One table is computed at a time, on its own thread (it takes from ~0.2 s to several seconds, too long for a
// frame); the frame pass uses the original shader until it is ready.
static struct {
    volatile int state;          // 0 idle, 1 computing, 2 computed (waiting for upload on the GL thread)
    GLuint palette;
    bool filter;
    float pal[PAL_SAMPLES][3];
    uint8_t* table;
    uint64_t start;
    SceUID thread;
    uint8_t rgba[PAL_SAMPLES][4]; // nearest palette samples (filter off): the colour table's entries
    GLuint colorTex;              // colour table texture being filled (0 = none for this job)
    uint32_t* colorData;          // its texels (vitaGL texture memory, written by the job thread)
} palJob;

static int palettizerJobMain(SceSize args, void* argp) {
    (void) args; (void) argp;
    palettizerFillTable((const float (*)[3]) palJob.pal, palJob.table);
    if (palJob.colorData != nullptr) palettizerFillColors(palJob.table, (const uint8_t (*)[4]) palJob.rgba, palJob.colorData);
    __sync_synchronize(); // the tables are complete before the GL thread can see state 2
    palJob.state = 2;
    return sceKernelExitDeleteThread(0);
}

static void palettizerCache(GLModernRenderer* m, GLuint palette, bool filter, GLuint lut, GLuint color) {
    if (m->palLutCount == (int32_t) (sizeof(m->palLuts) / sizeof(m->palLuts[0]))) {
        if (m->palLuts[0].lut != 0) glDeleteTextures(1, &m->palLuts[0].lut);
        if (m->palLuts[0].color != 0) glDeleteTextures(1, &m->palLuts[0].color);
        memmove(&m->palLuts[0], &m->palLuts[1], sizeof(m->palLuts[0]) * (size_t) (m->palLutCount - 1));
        m->palLutCount--;
    }
    m->palLuts[m->palLutCount].palette = palette;
    m->palLuts[m->palLutCount].filter = filter;
    m->palLuts[m->palLutCount].lut = lut;
    m->palLuts[m->palLutCount].color = color;
    m->palLutCount++;
}
#endif

#ifdef PLATFORM_VITA
// For a table job about to start: the colour table texture it fills as well (filter off only), allocated here on the
// GL thread from vitaGL's texture memory; its texels are only read once the job is taken in (palettizerTable).
static void palettizerColorTarget(GLModernRenderer* m, bool filter) {
    palJob.colorTex = 0;
    palJob.colorData = nullptr;
    if (!m->colorPalettizerOk || filter) return;
    GLint prevUnit;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0 + m->palLutSlot);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PAL_COLOR_W, PAL_COLOR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint32_t* data = (uint32_t*) vglGetTexDataPointer(GL_TEXTURE_2D); // linear, stride 1024 (vitaGL aligns to 8)
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture((GLenum) prevUnit);
    if (data == nullptr) {
        glDeleteTextures(1, &tex);
        logWarn("GL: no texture memory for the shdPalettizer colour table, using the index table\n");
        return;
    }
    palJob.colorTex = tex;
    palJob.colorData = data;
}
#endif

// The index table for this palette texture and texture filter (which changes what the palette samples return);
// 0 while it is being computed or if it can't be (the caller then draws with the original shader).
static GLuint palettizerTable(GLModernRenderer* m, GLuint palette) {
    bool filter = m->base.base.texFilter;
    repeat(m->palLutCount, i) if (m->palLuts[i].palette == palette && m->palLuts[i].filter == filter) return m->palLuts[i].lut;
#ifndef PLATFORM_VITA
    return 0;
#else
    if (palJob.state == 2) {
        GLuint lut = 0;
        GLint prevUnit;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
        glActiveTexture(GL_TEXTURE0 + m->palLutSlot);
        glGenTextures(1, &lut);
        glBindTexture(GL_TEXTURE_2D, lut);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PAL_LUT_W, PAL_LUT_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, palJob.table);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glActiveTexture((GLenum) prevUnit);
        free(palJob.table);
        palJob.table = nullptr;
        palettizerCache(m, palJob.palette, palJob.filter, lut, palJob.colorTex);
        logInfo("GL: built shdPalettizer table for palette texture %u (filter %d) in %.1f ms%s\n", palJob.palette, (int) palJob.filter, (double) (nowNanos() - palJob.start) / 1e6,
                palJob.colorTex != 0 ? ", with its colour table" : "");
        palJob.colorTex = 0;
        palJob.colorData = nullptr;
        palJob.state = 0;
        return palJob.palette == palette && palJob.filter == filter ? lut : 0;
    }
    if (palJob.state != 0) return 0; // computing (this palette or another one)
    if (!palettizerSamples(m, palette, filter, palJob.pal, palJob.rgba)) {
        palettizerCache(m, palette, filter, 0, 0); // never: the original shader for this palette
        logWarn("GL: no shdPalettizer table for palette texture %u: using the original shader\n", palette);
        return 0;
    }
    palJob.palette = palette;
    palJob.filter = filter;
    palJob.table = (uint8_t*) safeMalloc((size_t) PAL_LUT_W * PAL_LUT_H * 4);
    palJob.start = nowNanos();
    palettizerColorTarget(m, filter); // colour table texture for the job to fill, if it applies
    palJob.state = 1;
    palJob.thread = sceKernelCreateThread("bs_palettizer", palettizerJobMain, 0x10000100 + 16, 0x4000, 0, 0, nullptr);
    if (palJob.thread < 0 || sceKernelStartThread(palJob.thread, 0, nullptr) < 0) {
        palettizerFillTable((const float (*)[3]) palJob.pal, palJob.table); // no thread: build it here
        if (palJob.colorData != nullptr) palettizerFillColors(palJob.table, (const uint8_t (*)[4]) palJob.rgba, palJob.colorData);
        palJob.state = 2;
        return palettizerTable(m, palette);
    }
    return 0;
#endif
}

static inline bool fastPalettizerActive(GLModernRenderer* m) {
    return m->palettizerIndex >= 0 && m->base.base.currentShader == m->palettizerIndex;
}

#ifdef PLATFORM_VITA
// The colour table for this palette texture, 0 if there is none (yet) or the texture filter is on (the palette sample
// is then a blend the table doesn't hold). Call after palettizerTable, which takes finished tables in.
static GLuint palettizerColorTable(GLModernRenderer* m, GLuint palette) {
    if (!m->colorPalettizerOk || m->base.base.texFilter) return 0;
    repeat(m->palLutCount, i) if (m->palLuts[i].palette == palette && !m->palLuts[i].filter) return m->palLuts[i].color;
    return 0;
}
#endif

#ifdef PLATFORM_VITA
// ===[ CRT bake (Vita): runtime ]=== (sources and overview: "CRT bake (Vita)" above)
// Stand-in uniform handles: CRT_BAKE_STANDIN_BASE - 16 * (0 screen filter, 1 distortion) - index, so the two baked
// shaders' stand-ins never alias (vitaGL's locations are minus a uniform's address: never in this range).
#define CRT_BAKE_STANDIN_BASE (-0x100)

static bool vitaCrtBakeIsStandIn(int32_t handle) {
    return handle <= CRT_BAKE_STANDIN_BASE && handle > CRT_BAKE_STANDIN_BASE - 32;
}

// shader_get_uniform for a name the pass program doesn't have: a stand-in if a bake / plasma program uses it, else -1
static int32_t vitaCrtBakeUniformHandle(const VitaCrtBake* b, const char* name) {
    for (int32_t i = 0; i < b->uniformCount; i++)
        if (strcmp(b->uniforms[i].name, name) == 0) return b->uniforms[i].handle;
    return -1;
}

// shader_set_uniform_f: keep the value for the bake / plasma programs. True for a stand-in (no GL uniform behind it).
static bool vitaCrtBakeRecord(VitaCrtBake* b, int32_t handle, uint32_t count, const float* values) {
    for (int32_t i = 0; i < b->uniformCount; i++) {
        if (b->uniforms[i].handle != handle) continue;
        for (uint32_t k = 0; k < 4; k++) b->uniforms[i].value[k] = k < count ? values[k] : 0.0f;
        break;
    }
    return vitaCrtBakeIsStandIn(handle);
}

// shader_get_sampler_index for a sampler only the bake program reads (the screen filter's shadow mask)
static int32_t vitaCrtBakeSamplerSlot(const VitaCrtBake* b, const char* name) {
    return b->screenfilter && strcmp(name, "uTexShadowmask") == 0 ? CRT_BAKE_UNIT_MASK : -1;
}

static void vitaCrtBakeApplyUniforms(const VitaCrtBake* b, const VitaCrtBakeProgram* p) {
    for (int32_t k = 0; k < p->count; k++) {
        const VitaCrtBakeUniform* u = &b->uniforms[p->set[k].uniform];
        GLint loc = p->set[k].location;
        switch (u->type) {
            case GL_FLOAT_VEC2: glUniform2f(loc, u->value[0], u->value[1]); break;
            case GL_FLOAT_VEC3: glUniform3f(loc, u->value[0], u->value[1], u->value[2]); break;
            case GL_FLOAT_VEC4: glUniform4f(loc, u->value[0], u->value[1], u->value[2], u->value[3]); break;
            default:            glUniform1f(loc, u->value[0]); break;
        }
    }
}

// Collects an auxiliary program's uniforms into b->uniforms (shared by name with the pass program, else stand-ins).
static bool vitaCrtBakeCollectUniforms(VitaCrtBake* b, VitaCrtBakeProgram* p, GLuint passProgram) {
    p->count = 0;
    repeat(p->prog.uniformCount, i) {
        GLShaderUniform* su = &p->prog.uniforms[i];
        if (su->type == GL_SAMPLER_2D || strncmp(su->name, "gm_", 3) == 0) continue;
        int32_t idx = -1;
        for (int32_t k = 0; k < b->uniformCount; k++) if (strcmp(b->uniforms[k].name, su->name) == 0) idx = k;
        if (idx < 0) {
            if (b->uniformCount == (int32_t) (sizeof(b->uniforms) / sizeof(b->uniforms[0])) || strlen(su->name) >= sizeof(b->uniforms[0].name))
                return false;
            idx = b->uniformCount++;
            VitaCrtBakeUniform* u = &b->uniforms[idx];
            strcpy(u->name, su->name);
            u->type = su->type;
            GLint loc = glGetUniformLocation(passProgram, su->name);
            u->handle = loc != -1 ? loc : CRT_BAKE_STANDIN_BASE - (b->screenfilter ? 0 : 16) - idx;
        }
        if (p->count == (int32_t) (sizeof(p->set) / sizeof(p->set[0]))) return false;
        p->set[p->count].location = su->location;
        p->set[p->count].uniform = idx;
        p->count++;
    }
    return true;
}

// Sets a program's sampler to a texture unit (and records it); false if the program doesn't use it.
static bool vitaCrtBakeSetSamplerUnit(GMLShader* prog, const char* name, uint32_t unit) {
    repeat(prog->uniformCount, i) {
        GLShaderUniform* u = &prog->uniforms[i];
        if (u->type != GL_SAMPLER_2D || strcmp(u->name, name) != 0) continue;
        glUseProgram(prog->shaderId);
        glUniform1i(u->location, (GLint) unit);
        u->samplerSlot = unit;
        return true;
    }
    return false;
}

static void vitaCrtBakeFreeProgram(VitaCrtBakeProgram* p) {
    if (p->prog.compiled) freeShader(&p->prog);
    memset(p, 0, sizeof(*p));
}

// Compiles the bake variant of shd_retroscreen_screenfilter / _distortion: the bake (and plasma) programs, then the
// pass program into gmlShader. NULL if any of them fails (gmlShader is then left uncompiled for the rewrite above).
// Called in VGL_MODE_SHADER_PAIR, so a shader SceShaccCg rejects shows up here.
static VitaCrtBake* vitaCrtBakeCreate(int32_t index, GMLShader* gmlShader, const char* name, const char* vs, const char* fs,
                                      uint32_t attributeCount, const char** attributes) {
    static const char* fsMarker = "#define _YY_GLSLES_ 1";
    bool sf = strcmp(name, "shd_retroscreen_screenfilter") == 0;
    VitaCrtBake* b = (VitaCrtBake*) safeCalloc(1, sizeof(VitaCrtBake));
    b->screenfilter = sf;
    b->timeUniform = -1;
    char tag[96];
    bool ok = true;
    // bake program
    char* pvs = vitaCrtSplice(vs, "void main()", false, crtBakeVS);
    char* pfs = vitaCrtSplice(fs, fsMarker, false, sf ? crtBakeScreenfilterFS : crtBakeDistortionFS);
    snprintf(tag, sizeof(tag), "%s (bake)", name);
    ok = pvs != NULL && pfs != NULL && compileProgram(&b->bake.prog, tag, pvs, pfs, attributeCount, attributes);
    free(pvs);
    free(pfs);
    // plasma program
    if (ok && sf) {
        pvs = vitaCrtSplice(vs, "void main()", false, crtPlasmaVS);
        pfs = vitaCrtSplice(fs, fsMarker, false, crtPlasmaFS);
        snprintf(tag, sizeof(tag), "%s (plasma)", name);
        ok = pvs != NULL && pfs != NULL && compileProgram(&b->plasma.prog, tag, pvs, pfs, attributeCount, attributes);
        free(pvs);
        free(pfs);
    }
    // pass program (the GML shader)
    if (ok) {
        pvs = vitaCrtSplice(vs, "void main()", false, sf ? crtBakedScreenfilterVS : crtBakedDistortionVS);
        pfs = vitaCrtSplice(fs, fsMarker, false, sf ? crtBakedScreenfilterFS : crtBakedDistortionFS);
        ok = pvs != NULL && pfs != NULL && compileProgram(gmlShader, name, pvs, pfs, attributeCount, attributes);
        // the baked pass writes vec4(..., 1.0): classify it from the sources it was compiled from ("Opaque draws")
        if (ok) vitaClassifyShader(index, name, pvs, pfs);
        free(pvs);
        free(pfs);
    }
    if (!ok) {
        vitaCrtBakeFreeProgram(&b->bake);
        vitaCrtBakeFreeProgram(&b->plasma);
        free(b);
        return NULL;
    }
    // wiring: shared / stand-in uniforms, texture units, the pass program's uVitaBakeScale
    GLuint pass = gmlShader->shaderId;
    ok = vitaCrtBakeCollectUniforms(b, &b->bake, pass) && (!sf || vitaCrtBakeCollectUniforms(b, &b->plasma, pass));
    for (int32_t k = 0; k < b->uniformCount; k++) if (strcmp(b->uniforms[k].name, "uTime") == 0) b->timeUniform = k;
    ok = ok && (!sf || b->timeUniform >= 0);
    ok = ok && vitaCrtBakeSetSamplerUnit(gmlShader, "uVitaBake", CRT_BAKE_UNIT_BAKE);
    ok = ok && (!sf || vitaCrtBakeSetSamplerUnit(gmlShader, "uVitaPlasma", CRT_BAKE_UNIT_PLASMA));
    ok = ok && (!sf || vitaCrtBakeSetSamplerUnit(&b->bake.prog, "uTexShadowmask", CRT_BAKE_UNIT_MASK));
    repeat(gmlShader->uniformCount, i) { // the game's own samplers must stay clear of the bake's units
        GLShaderUniform* u = &gmlShader->uniforms[i];
        if (u->type == GL_SAMPLER_2D && strncmp(u->name, "uVita", 5) != 0 && u->samplerSlot >= CRT_BAKE_UNIT_MASK) ok = false;
    }
    b->scaleLocation = glGetUniformLocation(pass, "uVitaBakeScale");
    if (!ok || b->scaleLocation == -1) {
        logWarn("GL: %s: CRT bake programs compiled but could not be wired up\n", name);
        vitaCrtBakeFreeProgram(&b->bake);
        vitaCrtBakeFreeProgram(&b->plasma);
        freeShader(gmlShader);
        memset(gmlShader, 0, sizeof(*gmlShader));
        free(b);
        return NULL;
    }
    gmlShader->vitaBake = b;
    return b;
}

static void vitaCrtBakeRelease(VitaCrtBake* b) {
    if (b->bakeTexture != 0) glDeleteTextures(1, &b->bakeTexture);
    if (b->plasmaFbo != 0) glDeleteFramebuffers(1, &b->plasmaFbo);
    if (b->plasmaTexture != 0) glDeleteTextures(1, &b->plasmaTexture);
    b->bakeTexture = b->plasmaTexture = b->plasmaFbo = 0;
    b->bakeW = b->bakeH = 0;
    b->baked = b->plasmaValid = b->failed = false;
    b->bakeTarget = 0;
}

// glSurfaceFree: the bake textures go with the surface they were drawn for (mthRefreshScaling frees every CRT pass
// surface when the CRT type, the scaling or the window changes, so CRT off frees them all)
static void vitaCrtBakeSurfaceFreed(GLModernRenderer* m, GLuint fbo) {
    if (fbo == 0) return;
    repeat(m->gmlShaderCount, i) {
        VitaCrtBake* b = m->gmlShaders[i].vitaBake;
        if (b != NULL && b->bakeTarget == fbo && (b->bakeTexture != 0 || b->plasmaTexture != 0)) {
            vitaCrtBakeRelease(b);
            logInfo("GL: CRT bake textures freed (surface freed)\n");
        }
    }
}

// a 1x1 RGBA8 texture on the bound unit; for running out of memory: the pass then draws without the baked terms
static GLuint vitaCrtBakeNeutral(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t px[4] = { r, g, b, a };
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

// A texture on the bound unit; 0 (and nothing left behind) if vitaGL couldn't allocate it.
static GLuint vitaCrtBakeTexture(int32_t w, int32_t h, bool f16, bool linear) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, f16 ? GL_RGBA16F : GL_RGBA, w, h, 0, GL_RGBA, f16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (vglGetTexDataPointer(GL_TEXTURE_2D) == NULL) {
        glDeleteTextures(1, &t);
        return 0;
    }
    return t;
}

static uint64_t vitaCrtBakeHash(uint64_t h, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*) data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// flushBatch, just before a batch drawn with a baked CRT shader: (re)bake if the key changed, redraw the plasma if
// uTime changed, bind the textures. The batch's vertices are in the VBO and the renderer's VAO is bound; everything
// changed here is restored (framebuffer, viewport, blend / depth / cull / scissor, program, active texture unit).
static void vitaCrtBakeBeforeDraw(GLModernRenderer* m, GMLShader* shader, GlVertex* vertices, bool quads, int32_t count) {
    VitaCrtBake* b = shader->vitaBake;
    Renderer* r = &m->base.base;
    int32_t vertexCount = count * (quads ? VERTICES_PER_QUAD : VERTICES_PER_TRIANGLE);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int32_t w = vp[2], h = vp[3];

    uint64_t key = 14695981039346656037ull;
    int32_t dims[4] = { w, h, quads ? 1 : 0, vertexCount };
    key = vitaCrtBakeHash(key, dims, sizeof(dims));
    // positions and uvs only: the bake programs don't read v_vColour, so a draw colour / alpha change needs no rebake
    for (int32_t i = 0; i < vertexCount; i++) key = vitaCrtBakeHash(key, &vertices[i].x, offsetof(GlVertex, r));
    key = vitaCrtBakeHash(key, r->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION].m, sizeof(r->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION].m));
    for (int32_t k = 0; k < b->bake.count; k++)
        key = vitaCrtBakeHash(key, b->uniforms[b->bake.set[k].uniform].value, sizeof(b->uniforms[0].value));
    if (b->screenfilter) {
        uint32_t mask[2] = { (uint32_t) m->stageTexture[CRT_BAKE_UNIT_MASK], r->texFilter ? 1u : 0u };
        key = vitaCrtBakeHash(key, mask, sizeof(mask));
    }
    bool needBake = !b->failed && (!b->baked || key != b->bakeKey);
    float time = b->timeUniform >= 0 ? b->uniforms[b->timeUniform].value[0] : 0.0f;
    bool needPlasma = !b->failed && b->screenfilter && (!b->plasmaValid || time != b->plasmaTime);

    if (needBake || needPlasma) {
        GLint prevFbo = 0, prevUnit = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
        bool blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST);
        bool cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (blend) glDisable(GL_BLEND); // the bake's alpha is data
        if (depth) glDisable(GL_DEPTH_TEST);
        if (cull) glDisable(GL_CULL_FACE);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        if (needBake) {
            if (b->bakeTexture == 0 || b->bakeW != w || b->bakeH != h) {
                if (b->bakeTexture != 0) glDeleteTextures(1, &b->bakeTexture);
                glActiveTexture(GL_TEXTURE0 + CRT_BAKE_UNIT_BAKE);
                b->bakeTexture = vitaCrtBakeTexture(w, h, true, false);
                b->bakeW = w;
                b->bakeH = h;
            }
            if (b->bakeTexture != 0) {
                // the pass's own geometry, matrices and viewport size into the bake texture: same uv per pixel
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->bakeTexture, 0);
                glViewport(0, 0, w, h);
                glApplyGmlProgram(r, &b->bake.prog);
                vitaCrtBakeApplyUniforms(b, &b->bake);
                if (quads) glDrawElements(GL_TRIANGLES, count * INDICES_PER_QUAD, GL_UNSIGNED_SHORT, nullptr);
                else glDrawArrays(GL_TRIANGLES, 0, vertexCount);
                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevFbo);
                glDeleteFramebuffers(1, &fbo); // vitaGL frees its render target / depth buffer 4 frames later
                b->baked = true;
                b->bakeKey = key;
                b->bakeTarget = (GLuint) prevFbo;
                static uint32_t bakes = 0; // a key that changes every frame would show up here
                if (++bakes <= 8 || (bakes & 255) == 0)
                    logInfo("GL: %s baked (%dx%d, bake #%u)\n", b->screenfilter ? "screen filter" : "distortion", w, h, bakes);
            } else {
                b->failed = true;
                b->bakeTarget = (GLuint) prevFbo;
            }
        }
        if (needPlasma && !b->failed) {
            if (b->plasmaTexture == 0) {
                glActiveTexture(GL_TEXTURE0 + CRT_BAKE_UNIT_PLASMA);
                b->plasmaTexture = vitaCrtBakeTexture(CRT_PLASMA_W, CRT_PLASMA_H, false, true);
                if (b->plasmaTexture != 0) {
                    glGenFramebuffers(1, &b->plasmaFbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, b->plasmaFbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->plasmaTexture, 0);
                } else {
                    b->failed = true;
                    b->bakeTarget = (GLuint) prevFbo;
                }
            }
            if (b->plasmaTexture != 0) {
                // one quad over the plasma texture: texel (i, j) is evaluated at uv (i / GX, j / GY)
                GlVertex* q = (GlVertex*) vglAllocFromScratch(VERTICES_PER_QUAD * sizeof(GlVertex));
                float u0 = -0.5f / CRT_PLASMA_GX, u1 = (CRT_PLASMA_W - 0.5f) / CRT_PLASMA_GX;
                float v0 = -0.5f / CRT_PLASMA_GY, v1 = (CRT_PLASMA_H - 0.5f) / CRT_PLASMA_GY;
                GlVertex c[4] = { // x, y, z, u, v, r, g, b, a
                    { -1.0f, -1.0f, 0.0f, u0, v0, 255, 255, 255, 255 }, { 1.0f, -1.0f, 0.0f, u1, v0, 255, 255, 255, 255 },
                    { 1.0f, 1.0f, 0.0f, u1, v1, 255, 255, 255, 255 }, { -1.0f, 1.0f, 0.0f, u0, v1, 255, 255, 255, 255 },
                };
                memcpy(q, c, sizeof(c));
                glBindFramebuffer(GL_FRAMEBUFFER, b->plasmaFbo);
                glViewport(0, 0, CRT_PLASMA_W, CRT_PLASMA_H);
                glUseProgram(b->plasma.prog.shaderId);
                vitaCrtBakeApplyUniforms(b, &b->plasma);
                vglBufferData(GL_ARRAY_BUFFER, q);
                glDrawElements(GL_TRIANGLES, INDICES_PER_QUAD, GL_UNSIGNED_SHORT, nullptr);
                vglBufferData(GL_ARRAY_BUFFER, vertices); // the batch again, for the pass itself
                b->plasmaValid = true;
                b->plasmaTime = time;
            }
        }
        if (b->failed) logError("GL: CRT bake: out of GPU memory, drawing %s without the baked terms\n", b->screenfilter ? "the screen filter" : "distortion");
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevFbo);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (blend) glEnable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        glActiveTexture((GLenum) prevUnit);
        glApplyGmlProgram(r, shader);
    }

    GLint prevUnit = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    if (b->failed && b->bakeTexture == 0) { // once: neutral stand-ins (no tint / mask / scanline / vignette / warp)
        glActiveTexture(GL_TEXTURE0 + CRT_BAKE_UNIT_BAKE);
        b->bakeTexture = b->screenfilter ? vitaCrtBakeNeutral(255, 255, 255, 255) : vitaCrtBakeNeutral(0, 0, 0, 0);
        b->bakeW = b->bakeH = 1;
    }
    if (b->failed && b->screenfilter && b->plasmaTexture == 0) {
        glActiveTexture(GL_TEXTURE0 + CRT_BAKE_UNIT_PLASMA);
        b->plasmaTexture = vitaCrtBakeNeutral(128, 255, 0, 255);
    }
    glActiveTexture(GL_TEXTURE0 + CRT_BAKE_UNIT_BAKE);
    glBindTexture(GL_TEXTURE_2D, b->bakeTexture);
    if (b->screenfilter) {
        glActiveTexture(GL_TEXTURE0 + CRT_BAKE_UNIT_PLASMA);
        glBindTexture(GL_TEXTURE_2D, b->plasmaTexture);
    }
    glActiveTexture((GLenum) prevUnit);
    // window position -> bake texel: vitaGL maps clip space onto 2 * (size >> 1) pixels
    glUniform2f(b->scaleLocation, w > 0 ? (float) (2 * (w >> 1)) / (float) w : 1.0f, h > 0 ? (float) (2 * (h >> 1)) / (float) h : 1.0f);
}
#endif

#ifdef PLATFORM_VITA
// ===[ Big texture page prefetch (Vita) ]===
// Nearly every sprite is on one 4096x4096 BZ2+QOI page. It used to be decoded at its first use (the first frame's
// draw) on the main thread: ~14.5 s of black screen on hardware (hw_log_4..6: PERF draw=120 ms x 120 frames), almost
// all of it bzip2. glInit now allocates that page's texture and decodes it on a worker thread (txtr_stream.c: from
// <data dir>/texcache_<page>.bin when it matches this data.win, ~10x faster, else from the BZ2 blob, writing the cache
// for the next boot), while the main thread compiles shaders, runs the global scripts and builds the first room.
// All-in-one VPK (data.win in app0:): app0:texcache_<page>.bin is tried first, then <data dir>/texcache_<page>.bin;
// a rebuilt cache is only ever written to <data dir> (app0: is read-only).
// loop.c joins it before the first frame (GLRenderer_vitaFinishPrefetch), with the boot indicator animating.
// Markers in <data dir>: notexprefetch = decode at first use on the main thread (still through the cache),
// notexcache = never read or write the cache files (always bzip2).
#define VITA_PREFETCH_MAX 4
typedef struct {
    bool used;              // a job was started for pageId
    bool pending;           // started, not joined yet
    uint32_t pageId;
    TxtrStreamJob job;
    TxtrStreamStats stats;
    volatile int32_t rowsDone;
    volatile int32_t done;  // set by the worker when it is finished
    volatile int32_t ok;
    uint64_t startUs;
    char cachePath[128];
    char bundleCachePath[160];
} VitaPrefetch;
static VitaPrefetch vitaPrefetch[VITA_PREFETCH_MAX];
static int32_t vitaPrefetchCount = 0;
static int vitaTexCache = -1; // 1 on, 0 off (notexcache)

static bool vitaTexCacheOn(void) {
    if (vitaTexCache < 0) {
        vitaTexCache = vitaMarkerFile(BS_VITA_PATH("notexcache")) ? 0 : 1;
        if (vitaTexCache == 0) logInfo("GL: texture cache off (notexcache)\n");
    }
    return vitaTexCache == 1;
}

// A page the stream decoder takes (as GLRenderer_ensureTextureLoaded's own test, from the TXTR entry: the blob isn't
// loaded yet): stored in data.win, big, rows a multiple of 8 pixels. The job checks the blob really is 2zoq of this size.
static bool vitaStreamCandidate(GLRenderer* gl, uint32_t pageId, int32_t* w, int32_t* h) {
    DataWin* dw = gl->base.dataWin;
    if (VitaTextures_Active() || pageId >= dw->txtr.count || TxtrStream_dataWinPath == nullptr) return false;
    Texture* t = &dw->txtr.textures[pageId];
    *w = t->textureWidth;
    *h = t->textureHeight;
    return t->present && t->externalPath == nullptr && t->blobData == nullptr && t->blobOffset != 0 && t->blobSize >= 128 &&
           *w > 0 && *h > 0 && (*w % 8) == 0 && (size_t) *w * (size_t) *h >= (size_t) 1024 * 1024;
}

// The texture cache shipped next to data.win when data.win isn't in the data folder: the all-in-one VPK has
// app0:data.win + app0:texcache_N.bin (read-only). False when data.win is in the data folder (classic layout):
// the cache there is cachePath itself.
static bool vitaBundleCachePath(char* out, size_t outSize, uint32_t pageId) {
    // a cache shipped in the VPK (all-in-one or patch layout) is read-only and tried first; it is checked against
    // data.win like any other
    char app0[64];
    snprintf(app0, sizeof(app0), "app0:texcache_%u.bin", (unsigned) pageId);
    SceIoStat st;
    if (sceIoGetstat(app0, &st) >= 0) return snprintf(out, outSize, "%s", app0) < (int) outSize;
    const char* dw = TxtrStream_dataWinPath;
    const char* slash = strrchr(dw, '/');
    const char* colon = strrchr(dw, ':');
    const char* sep = slash != nullptr && (colon == nullptr || slash > colon) ? slash : colon;
    if (sep == nullptr) return false;
    size_t dirLen = (size_t) (sep + 1 - dw); // with the separator: "app0:", "ux0:data/AVGN8bit/"
    const char* dataDir = BS_VITA_DATA_DIR "/";
    if (dirLen == strlen(dataDir) && strncmp(dw, dataDir, dirLen) == 0) return false;
    return snprintf(out, outSize, "%.*stexcache_%u.bin", (int) dirLen, dw, (unsigned) pageId) < (int) outSize;
}

// Allocates the page's texture (bound on the current unit, previous binding restored) and fills in the job.
static bool vitaSetupJob(GLRenderer* gl, VitaPrefetch* p, uint32_t pageId, int32_t w, int32_t h) {
    DataWin* dw = gl->base.dataWin;
    Texture* t = &dw->txtr.textures[pageId];
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    uint8_t* texMem = (uint8_t*) vglGetTexDataPointer(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev);
    if (texMem == nullptr) return false;
    memset(p, 0, sizeof(*p));
    p->pageId = pageId;
    snprintf(p->cachePath, sizeof(p->cachePath), BS_VITA_PATH("texcache_%u.bin"), (unsigned) pageId);
    p->job.dataWinPath = TxtrStream_dataWinPath;
    p->job.dataWinSize = (uint64_t) dw->fileSize;
    p->job.pageId = pageId;
    p->job.blobOffset = t->blobOffset;
    p->job.blobSize = t->blobSize;
    p->job.gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    p->job.width = w;
    p->job.height = h;
    p->job.dst = texMem;
    p->job.dstStride = (size_t) w * 4;
    bool cache = vitaTexCacheOn();
    p->job.cachePath = cache ? p->cachePath : nullptr;
    p->job.writeCache = cache;
    // all-in-one VPK: the cache next to app0:data.win is read first; a rebuilt one is written to the data folder only
    p->job.bundleCachePath = cache && vitaBundleCachePath(p->bundleCachePath, sizeof(p->bundleCachePath), pageId) ? p->bundleCachePath : nullptr;
    return true;
}

static int vitaPrefetchMain(SceSize args, void* argp) {
    (void) args;
    VitaPrefetch* p = *(VitaPrefetch**) argp;
    p->ok = TxtrStream_decodePage(&p->job, &p->rowsDone, &p->stats) ? 1 : 0;
    __sync_synchronize();
    p->done = 1;
    return sceKernelExitDeleteThread(0);
}

static void vitaPrefetchStart(GLRenderer* gl) {
    DataWin* dw = gl->base.dataWin;
    if (!dw->lazyLoadTextures) return;
    bool threaded = !vitaMarkerFile(BS_VITA_PATH("notexprefetch"));
    if (!threaded) logInfo("GL: texture prefetch off (notexprefetch): big pages decode at first use\n");
    TxtrStream_crc32(0, nullptr, 0); // builds the CRC table here, before any worker thread uses it
    repeat(gl->textureCount, i) {
        int32_t w, h;
        if (!threaded || vitaPrefetchCount == VITA_PREFETCH_MAX || !vitaStreamCandidate(gl, (uint32_t) i, &w, &h)) continue;
        VitaPrefetch* p = &vitaPrefetch[vitaPrefetchCount];
        if (!vitaSetupJob(gl, p, (uint32_t) i, w, h)) continue;
        p->startUs = bsBootNowUs();
        // cores 1 and 2 (the main thread runs on core 0), normal user priority
        SceUID thread = sceKernelCreateThread("bs_txtr_prefetch", vitaPrefetchMain, 0x10000100, 0x20000, 0, 0x00060000, nullptr);
        VitaPrefetch* arg = p;
        if (thread < 0 || sceKernelStartThread(thread, sizeof(arg), &arg) < 0) {
            if (thread >= 0) sceKernelDeleteThread(thread);
            logWarn("GL: no worker thread for TXTR page %u: decoding it at first use\n", (unsigned) i);
            continue;
        }
        p->used = true;
        p->pending = true;
        vitaPrefetchCount++;
        logInfo("GL: decoding TXTR page %u (%dx%d) on a worker thread (cache %s%s%s)\n", (unsigned) i, w, h,
                p->job.bundleCachePath ? p->job.bundleCachePath : "", p->job.bundleCachePath ? ", then " : "", p->job.cachePath ? p->job.cachePath : "off");
    }
}

static const char* vitaCacheStateName(int s) {
    switch (s) {
        case TXTR_CACHE_USED: return "used";
        case TXTR_CACHE_MISSING: return "missing";
        case TXTR_CACHE_STALE: return "stale (other data.win)";
        case TXTR_CACHE_DAMAGED: return "damaged";
        default: return "off";
    }
}

static void vitaLogPageDecode(VitaPrefetch* p, const char* how, uint64_t waitedUs) {
    TxtrStreamStats* st = &p->stats;
    double readMs = (double) st->nsRead / 1e6;
    logInfo("GL: TXTR page %u %s from %s%s%s in %.0f ms: read %.1f MB in %.0f ms (%.1f MB/s), bzip2 %.0f ms, QOI + copy %.0f ms, CRC %.0f ms, "
            "cache write %.0f ms; cache %s%s; main thread waited %.0f ms%s%s\n",
            (unsigned) p->pageId, how, st->source == TXTR_SRC_CACHE ? "the texture cache" : st->source == TXTR_SRC_BZ2 ? "data.win (bzip2)" : "nowhere",
            st->cacheUsedPath ? " " : "", st->cacheUsedPath ? st->cacheUsedPath : "",
            (double) st->nsTotal / 1e6, (double) st->bytesRead / 1e6, readMs, readMs > 0 ? (double) st->bytesRead / 1e6 / (readMs / 1000.0) : 0.0,
            (double) st->nsBz2 / 1e6, (double) st->nsQoi / 1e6, (double) st->nsCrc / 1e6, (double) st->nsCacheWrite / 1e6,
            vitaCacheStateName(st->cacheState), st->cacheWritten ? ", rewritten" : "", (double) waitedUs / 1000.0,
            st->note[0] ? "; " : "", st->note);
}

// The page's texture is now complete: the same parameters as the stream path below.
static void vitaFinishPage(GLRenderer* gl, uint32_t pageId, int32_t w, int32_t h) {
    gl->textureLoaded[pageId] = true;
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;
    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
    bool isPOT = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    GLCommon_applyTexFilter(gl->base.texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);
}

// Waits for the page's job (animating the boot indicator, if it is still up). True if the texture is ready; false if
// the job failed (the caller then decodes the page the old way).
static bool vitaPrefetchJoin(GLRenderer* gl, VitaPrefetch* p) {
    p->pending = false;
    uint64_t waitStart = bsBootNowUs();
    while (!p->done) {
        bsBootTick();
        sceKernelDelayThread(4000);
    }
    __sync_synchronize();
    uint64_t waited = bsBootNowUs() - waitStart;
    vitaLogPageDecode(p, "decoded on a worker thread", waited);
    bsBootMark("TXTR page %u ready (worker %.0f ms from %s, main thread waited %.0f ms)", (unsigned) p->pageId,
               (double) p->stats.nsTotal / 1e6, p->stats.source == TXTR_SRC_CACHE ? "cache" : "bzip2", (double) waited / 1000.0);
    if (!p->ok) return false;
    vitaFinishPage(gl, p->pageId, p->job.width, p->job.height);
    return true;
}

static VitaPrefetch* vitaPrefetchFor(uint32_t pageId) {
    for (int32_t i = 0; i < vitaPrefetchCount; i++) if (vitaPrefetch[i].used && vitaPrefetch[i].pageId == pageId) return &vitaPrefetch[i];
    return nullptr;
}
#endif

static void glInit(Renderer* renderer, DataWin* dataWin) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer *modernGl = (GLModernRenderer*) renderer;
    renderer->dataWin = dataWin;

    GLVer ver = GLCommon_getGLVersion();
    if (ver.major < 2) {
        logError("GL: The modern-gl renderer requires OpenGL 2.0 or newer\n");
        abort();
    }
    modernGl->isGL3 = (ver.major >= 3);
    modernGl->isGLES = ver.isGLES;

    GLCommon_init(renderer);
#ifdef PLATFORM_VITA
    vitaPrefetchStart(gl); // the big texture page decodes on a worker thread while the rest of the boot runs
#endif

    if (!hasFBO()) {
        logError("GL: The modern-gl renderer requires FBO support\n");
        abort();
    }
    
    GMLShader* defaultShader = (GMLShader*)safeCalloc(1, sizeof(GMLShader));

    char vertSrc[1024];
    char fragSrc[1024];
    const char* vertHeader = "";
    const char* fragHeader = "";

    if (modernGl->isGL3) {
        if (modernGl->isGLES) {
            vertHeader = "#version 300 es\nprecision highp float;\n";
            fragHeader = "#version 300 es\nprecision highp float;\n";

            snprintf(vertSrc, sizeof(vertSrc),
                "%s"
                "layout(location = 0) in vec3 aPos;\nlayout(location = 1) in vec4 aColor;\nlayout(location = 2) in vec2 aTexCoord;\n"
                "out vec2 vTexCoord;\nout vec4 vColor;\nout float vFogFactor;\n%s",
                vertHeader, baseVertexShader);
        } else {
            vertHeader = "#version 130\n";
            fragHeader = "#version 130\n";

            snprintf(vertSrc, sizeof(vertSrc),
                "%s"
                "in vec3 aPos;\nin vec4 aColor;\nin vec2 aTexCoord;\n"
                "out vec2 vTexCoord;\nout vec4 vColor;\nout float vFogFactor;\n%s",
                vertHeader, baseVertexShader);
        }

        snprintf(fragSrc, sizeof(fragSrc),
            "%s"
            "in vec2 vTexCoord;\nin vec4 vColor;\nin float vFogFactor;\nout vec4 fragColor;\n"
            "#define TEXTURE_2D texture\n#define FRAG_COLOR fragColor\n%s",
            fragHeader, baseFragmentShader);
    } else {
        if (modernGl->isGLES) {
            vertHeader = "#version 100\nprecision highp float;\n";
            fragHeader = "#version 100\nprecision highp float;\n";
        } else {
            vertHeader = "#version 110\n";
            fragHeader = "#version 110\n";
        }

        snprintf(vertSrc, sizeof(vertSrc),
            "%s"
            "attribute vec3 aPos;\nattribute vec4 aColor;\nattribute vec2 aTexCoord;\n"
            "varying vec2 vTexCoord;\nvarying vec4 vColor;\nvarying float vFogFactor;\n%s",
            vertHeader, baseVertexShader);

        snprintf(fragSrc, sizeof(fragSrc),
            "%s"
            "varying vec2 vTexCoord;\nvarying vec4 vColor;\nvarying float vFogFactor;\n"
            "#define TEXTURE_2D texture2D\n#define FRAG_COLOR gl_FragColor\n%s",
            fragHeader, baseFragmentShader);
    }

    const char* defaultAttributes[] = { "aPos", "aColor", "aTexCoord" };
    bool success = compileProgram(defaultShader, "default", vertSrc, fragSrc, 3, defaultAttributes);
    if (!success) {
        logError("GL: Failed to compile default shaders! Bailing...\n");
        abort();
    }

    modernGl->defaultShaderProgram = defaultShader;
    modernGl->uTextureRect = glGetUniformLocation(defaultShader->shaderId, "uTextureRect");
    modernGl->uLocalUV = glGetUniformLocation(defaultShader->shaderId, "uLocalUV");
    modernGl->depthWrite = true;

    modernGl->uWorldViewProjection = getShaderUniform(defaultShader, "uWorldViewProjection", GL_FLOAT_MAT4);
    modernGl->uWorldView           = getShaderUniform(defaultShader, "uWorldView",           GL_FLOAT_MAT4);
    modernGl->uFogRange            = getShaderUniform(defaultShader, "uFogRange",            GL_FLOAT_VEC2);
    modernGl->uFogColor            = getShaderUniform(defaultShader, "uFogColor",            GL_FLOAT_VEC4);
    modernGl->uAlphaTestRef        = getShaderUniform(defaultShader, "uAlphaTestRef",        GL_FLOAT);
    modernGl->uAlphaTestEnabled    = getShaderUniform(defaultShader, "uAlphaTestEnabled",    GL_BOOL);
    modernGl->uTexture             = getShaderUniform(defaultShader, "uTexture",             GL_SAMPLER_2D);

    modernGl->fastDefaultProgram = nullptr;
#ifdef PLATFORM_VITA
    // Fast default program. The default fragment shader costs ~18 USSE instructions per pixel on the Vita (Vita3K's
    // GXP log): its texture read is dependent (the uLocalUV select feeds the coordinate), the fog mix runs always, and
    // the alpha-test discard splits it into two phases. Nearly every draw of the frame uses it (sprites, tiles, text,
    // rectangles, the border art, surface draws and copies, the final stretch), so it is a large part of the frame's
    // GPU time. With fog, alpha test and local UV all off it is texture * colour: vFogFactor is 0 (uFogColor.a == 0)
    // and mix(c, fog, 0) == c, so this program, a non-dependent read and one multiply, gives the same pixels.
    // flushBatch switches to it per batch; glShaderSettingsRefresh keeps its matrix in step with the default
    // program's. <data dir>/nofastdefault turns it off for A/B comparisons.
    FILE* noFastDefault = fopen(BS_VITA_PATH("nofastdefault"), "r");
    if (noFastDefault != nullptr) {
        fclose(noFastDefault);
        logInfo("GL: fast default program disabled (nofastdefault)\n");
    } else if (!modernGl->isGL3) {
        char fastVert[1024];
        char fastFrag[512];
        snprintf(fastVert, sizeof(fastVert),
            "%s"
            "attribute vec3 aPos;\nattribute vec4 aColor;\nattribute vec2 aTexCoord;\n"
            "varying vec2 vTexCoord;\nvarying vec4 vColor;\n"
            "uniform mat4 uWorldViewProjection;\n"
            "void main() {\n"
            "    gl_Position = uWorldViewProjection * vec4(aPos, 1.0);\n"
            "    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
            "    vTexCoord = aTexCoord;\n"
            "    vColor = aColor;\n"
            "}\n",
            vertHeader);
        snprintf(fastFrag, sizeof(fastFrag),
            "%s"
            "varying vec2 vTexCoord;\nvarying vec4 vColor;\n"
            "uniform sampler2D uTexture;\n"
            "void main() {\n"
            "    gl_FragColor = texture2D(uTexture, vTexCoord) * vColor;\n"
            "}\n",
            fragHeader);
        GMLShader* fast = (GMLShader*) safeCalloc(1, sizeof(GMLShader));
        GLint fastWVP = -1, fastTexture = -1;
        if (compileProgram(fast, "default (fast)", fastVert, fastFrag, 3, defaultAttributes)) {
            fastWVP = glGetUniformLocation(fast->shaderId, "uWorldViewProjection");
            fastTexture = glGetUniformLocation(fast->shaderId, "uTexture");
        }
        if (fastWVP >= 0 && fastTexture >= 0) {
            glUseProgram(fast->shaderId);
            glUniform1i(fastTexture, 1); // flushBatch binds default-shader textures to unit 1
            modernGl->fastDefaultProgram = fast;
            modernGl->fastDefaultWVP = fastWVP;
            logInfo("GL: fast default program ready\n");
        } else {
            logWarn("GL: fast default program unavailable, every draw uses the default program\n");
            freeShader(fast);
            free(fast);
        }
    }
#endif

    modernGl->gmlShaders = (GMLShader *)safeCalloc(dataWin->shdr.count, sizeof(GMLShader));
    logInfo("GL: %u Shaders Found\n", dataWin->shdr.count);
#ifdef PLATFORM_VITA
    vitaShaderCount = dataWin->shdr.count;
    vitaShaderAlpha = (uint8_t*) safeCalloc(vitaShaderCount + 1, 1);
    vitaShaderNoDiscard = (bool*) safeCalloc(vitaShaderCount + 1, sizeof(bool));
#endif

    repeat(dataWin->shdr.count, i) {
        Shader* shdr = &dataWin->shdr.shaders[i];
        GMLShader* gmlShader = &modernGl->gmlShaders[i];

        if (!shdr->present) {
            modernGl->gmlShaderCount++;
            logWarn("GL: Skipping shader %d because it isn't present!\n", (int)i);
            continue;
        }

        logInfo("GL: Compiling %s\n", shdr->name);

        const char* vertexShaderSource = modernGl->isGLES ? shdr->glslES_Vertex : shdr->glsl_Vertex;
        const char* fragmentShaderSource = modernGl->isGLES ? shdr->glslES_Fragment : shdr->glsl_Fragment;

        char* patchedVertexSource = nullptr;
        char* patchedFragmentSource = nullptr;

        if (!modernGl->isGLES && ver.major == 2 && ver.minor == 0) { // super opengl 2.0 fuckery go go
            if (vertexShaderSource && strstr(vertexShaderSource, "#version 120")) {
                patchedVertexSource = safeStrdup(vertexShaderSource);
                char* loc = strstr(patchedVertexSource, "#version 120");
                if (loc) loc[10] = '1';
                vertexShaderSource = patchedVertexSource;
            }
            if (fragmentShaderSource && strstr(fragmentShaderSource, "#version 120")) {
                patchedFragmentSource = safeStrdup(fragmentShaderSource);
                char* loc = strstr(patchedFragmentSource, "#version 120");
                if (loc) loc[10] = '1';
                fragmentShaderSource = patchedFragmentSource;
            }
        }

#ifdef PLATFORM_VITA
        // The palette swapper (the game's retro_pal_swapper, used by enemies, bosses, the player's bullets...) walks
        // its palette column with a float loop bounded by uniforms (`for (float i = corner.y; i < u_Uvs.w;
        // i += u_pixelSize.y)`) and returns from inside it. That hung the GPU on hardware (watchdog -> the console
        // shuts down) and crashes Vita3K's shader recompiler. Same search as a fixed-count loop with no break or
        // early return: palettes are at most 64 rows (sprGamePalette is 8x64).
        if (shdr->name != nullptr && strcmp(shdr->name, "shd_pal_swapper") == 0 && fragmentShaderSource != nullptr) {
            const char* fn = strstr(fragmentShaderSource, "vec4 findAltColor(");
            const char* mainFn = fn != nullptr ? strstr(fn, "void main()") : nullptr;
            if (mainFn != nullptr) {
                static const char* replacement =
                    "vec4 findAltColor(vec4 inCol, vec2 corner)\n"
                    "{\n"
                    "    if(inCol.a == 0.) return Transparent;\n"
                    "    vec4 result = inCol;\n"
                    "    float found = 0.0;\n"
                    "    float step = 0.0;\n"
                    "    for (int k = 0; k < 64; k++)\n"
                    "    {\n"
                    "        float i = corner.y + step * u_pixelSize.y;\n"
                    "        if (found == 0.0 && i < u_Uvs.w)\n"
                    "        {\n"
                    "            vec4 leftCol = texture2D(u_palTexture, vec2(corner.x, i));\n"
                    "            if (distance(leftCol, inCol) < Tolerance)\n"
                    "            {\n"
                    "                vec2 testPos = vec2(corner.x + u_pixelSize.x * floor(u_paletteId + 1.0), i);\n"
                    "                result = mix(texture2D(u_palTexture, vec2(testPos.x - u_pixelSize.x, testPos.y)), texture2D(u_palTexture, testPos), fract(u_paletteId));\n"
                    "                found = 1.0;\n"
                    "            }\n"
                    "        }\n"
                    "        step += 1.0;\n"
                    "    }\n"
                    "    return result;\n"
                    "}\n";
                size_t head = (size_t) (fn - fragmentShaderSource);
                size_t total = head + strlen(replacement) + strlen(mainFn) + 1;
                patchedFragmentSource = (char*) safeMalloc(total);
                memcpy(patchedFragmentSource, fragmentShaderSource, head);
                strcpy(patchedFragmentSource + head, replacement);
                strcat(patchedFragmentSource, mainFn);
                fragmentShaderSource = patchedFragmentSource;
                logInfo("GL: shd_pal_swapper: palette search rewritten as a fixed 64-step loop\n");
            }
        }
        // CRT filter rewrites ("CRT filter rewrites (Vita)" above). They replace the whole body, so the sign-bug patch
        // below is not needed on them (they only use x - 0.5); it still applies to the originals if a rewrite fails.
        // vita/main.c selects VGL_MODE_POSTPONED, where vitaGL compiles GLSL inside glLinkProgram: GL_COMPILE_STATUS
        // is always true there, and a shader SceShaccCg rejects leaves a NULL program that glLinkProgram goes on to
        // read (vitaGL is built with NO_DEBUG). A rewrite is compiled in VGL_MODE_SHADER_PAIR instead: the same
        // translation (vertex shader, then its fragment shader) done in glCompileShader, so a failure is seen before
        // anything is linked and the original sources are compiled as usual.
        // CRT bake ("CRT bake (Vita)" above) first, for the screen filter and distortion: if any of its programs fails
        // to compile, the rewrite below (or the original) is compiled instead. Off with <data dir>/nocrtbake.
        if (vitaCrtRewriteFor(shdr->name) != NULL && vertexShaderSource != NULL && fragmentShaderSource != NULL &&
            (strcmp(shdr->name, "shd_retroscreen_screenfilter") == 0 || strcmp(shdr->name, "shd_retroscreen_distortion") == 0)) {
            static int bakeOff = -1;
            if (bakeOff < 0) {
                bakeOff = vitaMarkerFile(BS_VITA_PATH("nocrtbake")) ? 1 : 0;
                if (bakeOff) logInfo("GL: CRT bake disabled (nocrtbake)\n");
            }
            // Per shader: the screen filter bake is on unless <data dir>/nocrtbakesf. The distortion bake is OFF unless
            // <data dir>/crtbakedist: in Vita3K (real SceShaccCg) CRT type 2 came out with a blue-white cast through it
            // (red down to ~0.5) while type 3 looked right; not explained yet, and its expected gain is only ~1 ms.
            bool sfShader = strcmp(shdr->name, "shd_retroscreen_screenfilter") == 0;
            bool skipOne = sfShader ? vitaMarkerFile(BS_VITA_PATH("nocrtbakesf")) : !vitaMarkerFile(BS_VITA_PATH("crtbakedist"));
            if (skipOne) logInfo("GL: %s: CRT bake off (%s)\n", shdr->name, sfShader ? "nocrtbakesf" : "no crtbakedist marker");
            if (!bakeOff && !skipOne) {
                vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
                VitaCrtBake* bake = vitaCrtBakeCreate((int32_t) i, gmlShader, shdr->name, vertexShaderSource, fragmentShaderSource,
                                                      shdr->vertexAttributeCount, shdr->vertexAttributes);
                vglSetSemanticBindingMode(VGL_MODE_POSTPONED); // vita/main.c's mode
                if (bake != NULL) {
                    logInfo("GL: %s: Vita CRT rewrite (baked terms, %d shared / stand-in uniforms)\n", shdr->name, (int) bake->uniformCount);
                    if (patchedVertexSource) free(patchedVertexSource);
                    if (patchedFragmentSource) free(patchedFragmentSource);
                    modernGl->gmlShaderCount++;
                    continue;
                }
                logWarn("GL: %s: CRT bake not used (SceShaccCg rejected a program), compiling the rewrite\n", shdr->name);
            }
        }
        {
            const VitaCrtRewrite* crt = vitaCrtRewriteFor(shdr->name);
            if (crt != NULL && vertexShaderSource != NULL && fragmentShaderSource != NULL) {
                char* crtVS = crt->vertexBody != NULL ? vitaCrtSplice(vertexShaderSource, "void main()", crt->grid, crt->vertexBody) : NULL;
                char* crtFS = vitaCrtSplice(fragmentShaderSource, "#define _YY_GLSLES_ 1", crt->grid, crt->fragmentBody);
                bool crtOK = false, spliced = crtFS != NULL && (crt->vertexBody == NULL || crtVS != NULL);
                if (spliced) {
                    vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
                    crtOK = compileProgram(gmlShader, shdr->name, crtVS != NULL ? crtVS : vertexShaderSource, crtFS,
                                           shdr->vertexAttributeCount, shdr->vertexAttributes);
                    vglSetSemanticBindingMode(VGL_MODE_POSTPONED); // vita/main.c's mode
                    if (crtOK) vitaClassifyShader((int32_t) i, shdr->name, crtVS != NULL ? crtVS : vertexShaderSource, crtFS);
                }
                free(crtVS);
                free(crtFS);
                if (crtOK) {
                    gmlShader->vitaGrid = crt->grid;
                    logInfo("GL: %s: Vita CRT rewrite%s\n", shdr->name, crt->grid ? " (64x56 grid)" : "");
                    if (patchedVertexSource) free(patchedVertexSource);
                    if (patchedFragmentSource) free(patchedFragmentSource);
                    modernGl->gmlShaderCount++;
                    continue;
                }
                logWarn("GL: %s: Vita CRT rewrite not used (%s), compiling the original shader\n", shdr->name,
                        spliced ? "SceShaccCg rejected it" : "GameMaker header not found");
            }
        }
        // CRT filter types 2/3 showed the frame rotated 180 degrees. SceShaccCg compiles getCRTUV / sampleScreen
        // (`vec2 centerDelta = vec2(0.5) - uv; ... uv -= 0.5; uv *= k; uv += 0.5;`) as (0.5 - uv) * k + 0.5, i.e.
        // 1 - getCRTUV(uv) (GXP: the VMAD2 reuses centerDelta with no negate); the sign is lost in the compiler's IR
        // when `0.5 - x` and `x -= 0.5` meet. Same maths written with one sign and no compound subtraction: every
        // `0.5 - uv` in these two shaders only feeds length() or abs(). Applied only if the shader text is exactly
        // the one this was checked against (12 / 4 replacements), otherwise the original compiles unchanged.
        if (shdr->name != nullptr && fragmentShaderSource != nullptr && patchedFragmentSource == nullptr &&
            (strcmp(shdr->name, "shd_retroscreen_distortion") == 0 || strcmp(shdr->name, "shd_retroscreen_postprocessing") == 0)) {
            static const char* const crtFix[][2] = {
                { " uv -= 0.5;",       " uv = uv - vec2(0.5);" },
                { "vec2(0.5) - uv;",    "uv - vec2(0.5);" },
                { "vec2(0.5) - crtuv;", "crtuv - vec2(0.5);" },
                { "abs(0.5 - uv.x)",    "abs(uv.x - 0.5)" },
                { "abs(0.5 - uv.y)",    "abs(uv.y - 0.5)" },
            };
            int expected = strcmp(shdr->name, "shd_retroscreen_distortion") == 0 ? 12 : 4;
            char* src = safeStrdup(fragmentShaderSource);
            int replaced = 0;
            for (size_t k = 0; k < sizeof(crtFix) / sizeof(crtFix[0]); k++) {
                char* next = vitaReplaceAll(src, crtFix[k][0], crtFix[k][1], &replaced);
                free(src);
                src = next;
            }
            if (replaced == expected) {
                patchedFragmentSource = src;
                fragmentShaderSource = src;
                logInfo("GL: %s: %d centre-offset expressions rewritten (SceShaccCg sign bug)\n", shdr->name, replaced);
            } else {
                free(src);
                logWarn("GL: %s: expected %d centre-offset expressions, found %d; compiled unchanged (CRT may show rotated)\n", shdr->name, expected, replaced);
            }
        }
#endif

        compileProgram(
            gmlShader,
            shdr->name,
            vertexShaderSource,
            fragmentShaderSource,
            shdr->vertexAttributeCount,
            shdr->vertexAttributes
        );
#ifdef PLATFORM_VITA
        if (gmlShader->compiled) vitaClassifyShader((int32_t) i, shdr->name, vertexShaderSource, fragmentShaderSource);
        if (gmlShader->compiled && shdr->name != nullptr && strcmp(shdr->name, "shd_pal_swapper") == 0 && fragmentShaderSource != nullptr)
            vitaPalSwapInit(modernGl, (int32_t) i, shdr->name, vertexShaderSource, fragmentShaderSource,
                            shdr->vertexAttributeCount, shdr->vertexAttributes);
#endif

        if (patchedVertexSource) free(patchedVertexSource);
        if (patchedFragmentSource) free(patchedFragmentSource);

        modernGl->gmlShaderCount++;
    }
    initFastPalettizer(modernGl, dataWin);
#ifdef PLATFORM_VITA
    repeat(vitaShaderCount, i) {
        if (vitaShaderAlpha[i] != VITA_ALPHA_PALETTE) continue;
        GMLShader* pal = &modernGl->gmlShaders[i];
        repeat(pal->uniformCount, b) if (strcmp(pal->uniforms[b].name, "spritePalette") == 0) vitaPaletteSlot = (int32_t) pal->uniforms[b].samplerSlot;
    }
    vitaOpaque(); // log the mode at start-up
#endif
    modernGl->fogEnable = false;
    modernGl->fogColor = 0;
    modernGl->fogEnd = 1;
    glUseProgram(modernGl->defaultShaderProgram->shaderId);
    glUniform1f(modernGl->uAlphaTestRef->location, -1.0f);
    glUniform4f(modernGl->uFogColor->location, 0.0f, 0.0f, 0.0f, 0.0f);
    glUniform2f(modernGl->uFogRange->location, 1.0f, 1.0f);

    // Create VAO/VBO/EBO
    if (hasVAO()) {
        glGenVertexArrays(1, &modernGl->vao);
        glGenVertexArrays(1, &modernGl->vertexBufferVao);
        glBindVertexArray(modernGl->vao);
    }
    glGenBuffers(1, &modernGl->vbo);
    glGenBuffers(1, &modernGl->ebo);

    // VBO: sized for max quads
    glBindBuffer(GL_ARRAY_BUFFER, modernGl->vbo);
#ifndef PLATFORM_VITA // We don't really need to warm up the buffer since we have scratch memory on VitaGL...
    int32_t vboSize = MAX_QUADS * VERTICES_PER_QUAD * sizeof(GlVertex);
    glBufferData(GL_ARRAY_BUFFER, vboSize, nullptr, GL_DYNAMIC_DRAW);
#endif

    int32_t eboSize = MAX_QUADS * INDICES_PER_QUAD * sizeof(uint16_t);
    uint16_t* indices = (uint16_t*)safeMalloc(eboSize);
    for (int32_t i = 0; MAX_QUADS > i; i++) {
        uint16_t base = i * 4;
        indices[i*6+0] = base+0; indices[i*6+1] = base+1; indices[i*6+2] = base+2;
        indices[i*6+3] = base+2; indices[i*6+4] = base+3; indices[i*6+5] = base+0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modernGl->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, eboSize, indices, GL_STATIC_DRAW);
    free(indices);

    if (hasVAO()) {
        // Vertex attributes: XYZ, normalized RGBA8, UV.
        int32_t stride = sizeof(GlVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(GlVertex, x));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*) offsetof(GlVertex, r));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(GlVertex, u));
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }

    // Allocate CPU-side vertex buffer
#if PLATFORM_VITA
    gl->vertexData = (GlVertex *)vglAllocFromScratch(MAX_QUADS * VERTICES_PER_QUAD * sizeof(GlVertex));
#else
    gl->vertexData = (GlVertex *)safeMalloc(MAX_QUADS * VERTICES_PER_QUAD * sizeof(GlVertex));
#endif

    modernGl->batchCount = 0;
    modernGl->currentTextureId = 0;

    logInfo("GL: Renderer initialized (%u texture pages)\n", gl->textureCount);
    logInfo("GL: %u shader programs compiled in %.1f ms (slowest: %s, %.1f ms)\n", (unsigned) compileCount, (double) compileNs / 1e6,
            compileSlowest, (double) compileSlowestNs / 1e6);
}

static void glApplyCullWinding(Renderer* renderer) {
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
    // The view is left-handed, and uploaded projections flip clip Y. A normal
    // perspective projection therefore makes outward-facing triangles CCW in
    // GL; the negative-height rectangle projection reverses that orientation.
    const float* projection = renderer->gmlMatrices[MATRIX_PROJECTION].m;
    float orientation = projection[0] * projection[5] - projection[1] * projection[4];
    GLenum frontFace = orientation >= 0 ? GL_CCW : GL_CW;
    if (modernGl->frontFace != frontFace) {
        glFrontFace(frontFace);
        modernGl->frontFace = frontFace;
    }
}

static void glGpuSetShader(Renderer* renderer, int32_t shaderIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
    
    flushBatch(gl);
    GMLShader* gmlShader = &modernGl->gmlShaders[shaderIndex];
    if (shaderIndex == modernGl->palettizerIndex) gmlShader = &modernGl->fastPalettizer;
    glApplyGmlProgram(renderer, gmlShader);
    renderer->currentShader = shaderIndex;
}

// Set a GML shader program's built-in uniforms from the current state (the program need not be bound).
static void glSetGmlBuiltins(Renderer* renderer, GMLShader* gmlShader) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    //Gotta set those built-ins! they ain't gonna set themselves
    GLShaderUniform* gmMatricesUniform = gmlShader->gmMatrices;
    GLShaderUniform* gmFogColourUniform = gmlShader->gmFogColour;

    //Lights are for another time

    GLShaderUniform* gmAlphaTestEnabledUniform = gmlShader->gmAlphaTestEnabled;
    GLShaderUniform* gmAlphaRefValue = gmlShader->gmAlphaRefValue;

    Matrix4f flippedClip[MATRICES_MAX];
    memcpy(flippedClip, renderer->gmlMatrices, sizeof(flippedClip));

    Matrix4f_flipClipY(&flippedClip[MATRIX_PROJECTION]);
    Matrix4f_flipClipY(&flippedClip[MATRIX_WORLD_VIEW_PROJECTION]);

    if (gmMatricesUniform != nullptr) {
        glUniformMatrix4fv(gmMatricesUniform->location, 5, GL_FALSE, flippedClip[0].m);
    }
    if (gmFogColourUniform != nullptr) {
        glUniform1i(gmFogColourUniform->location, modernGl->fogColor);
    }
    if (gmAlphaTestEnabledUniform != nullptr) {
        glUniform1i(gmAlphaTestEnabledUniform->location, gl->alphaTestEnable);
    }
    if (gmAlphaRefValue != nullptr) {
        glUniform1f(gmAlphaRefValue->location, gl->alphaTestRef);
    }
}

// Bind a GML shader's program and set its built-in uniforms (no batch flush).
static void glApplyGmlProgram(Renderer* renderer, GMLShader* gmlShader) {
    glUseProgram(gmlShader->shaderId);
    glApplyCullWinding(renderer);
    glSetGmlBuiltins(renderer, gmlShader);
#ifdef PLATFORM_VITA
    // "Palette swapper loop variants": the same built-in values for the short-loop programs
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
    if (vitaPalSwap.index >= 0 && gmlShader == &modernGl->gmlShaders[vitaPalSwap.index])
        repeat(VITA_PALSWAP_VARIANTS, v) if (vitaPalSwap.ok[v]) glSetGmlBuiltins(renderer, &vitaPalSwap.variant[v]);
#endif
}

static void glShaderSettingsRefresh(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    flushBatch(gl);
    glApplyCullWinding(renderer);
    if (renderer->currentShader != -1) {
        glGpuSetShader(renderer, (int32_t) renderer->currentShader);
    } else {
        float fogR = (float) BGR_R(modernGl->fogColor) / 255.0f;
        float fogG = (float) BGR_G(modernGl->fogColor) / 255.0f;
        float fogB = (float) BGR_B(modernGl->fogColor) / 255.0f;

        glUseProgram(modernGl->defaultShaderProgram->shaderId);

        Matrix4f flippedClip[MATRICES_MAX];
        memcpy(flippedClip, renderer->gmlMatrices, sizeof(flippedClip));
        //I was making the Legacy OpenGL renderer work with the projections, then I realized I think I only need to flip the Projection(s) and not the other ones
        Matrix4f_flipClipY(&flippedClip[MATRIX_PROJECTION]);
        Matrix4f_flipClipY(&flippedClip[MATRIX_WORLD_VIEW_PROJECTION]);

        glUniformMatrix4fv(modernGl->uWorldViewProjection->location, 1, GL_FALSE, flippedClip[MATRIX_WORLD_VIEW_PROJECTION].m);
        glUniformMatrix4fv(modernGl->uWorldView->location, 1, GL_FALSE, renderer->gmlMatrices[MATRIX_WORLD_VIEW].m);
        float range = modernGl->fogEnd - modernGl->fogStart;
        glUniform2f(modernGl->uFogRange->location, range == 0 ? 0 : 1.0f / range, modernGl->fogEnd);
        glUniform4f(modernGl->uFogColor->location, fogR, fogG, fogB, modernGl->fogEnable ? 1.0f : 0.0f);
        glUniform1f(modernGl->uAlphaTestRef->location, gl->alphaTestRef);
        glUniform1i(modernGl->uAlphaTestEnabled->location, gl->alphaTestEnable);
        glUniform1i(modernGl->uTexture->location, 1);
        if (modernGl->fastDefaultProgram != nullptr) {
            glUseProgram(modernGl->fastDefaultProgram->shaderId);
            glUniformMatrix4fv(modernGl->fastDefaultWVP, 1, GL_FALSE, flippedClip[MATRIX_WORLD_VIEW_PROJECTION].m);
            glUseProgram(modernGl->defaultShaderProgram->shaderId);
        }
    }
}

// camera_apply: swap the active world->clip projection on the current target without touching its viewport.
static void glApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,const Matrix4f* projectionMatrix) {
    GLRenderer* gl = (GLRenderer*) renderer;

    // Flush first so pending quads draw under the projection they were issued with.
    flushBatch(gl);
    
    Renderer_applyProjection(renderer, viewMatrix, projectionMatrix);

    glShaderSettingsRefresh(renderer);
}

static void glGpuResetShader(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    flushBatch(gl);
    glUseProgram(modernGl->defaultShaderProgram->shaderId);
    renderer->currentShader = -1;
    glShaderSettingsRefresh(renderer);
}

static void freeShader(GMLShader* shader) {
#ifdef PLATFORM_VITA
    if (shader->vitaBake != nullptr) { // CRT bake: its textures and programs
        VitaCrtBake* b = shader->vitaBake;
        shader->vitaBake = nullptr;
        vitaCrtBakeRelease(b);
        vitaCrtBakeFreeProgram(&b->bake);
        vitaCrtBakeFreeProgram(&b->plasma);
        free(b);
    }
#endif
    if (shader->compiled)
        glDeleteProgram(shader->shaderId);

    repeat(shader->uniformCount, i) {
        free(shader->uniforms[i].name);
    }
    free(shader->uniforms);
}

static void glDestroy(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    repeat(modernGl->gmlShaderCount, i) {
        freeShader(&modernGl->gmlShaders[i]);
    }

    free(modernGl->gmlShaders);
#ifdef PLATFORM_VITA
    // Vita: the palette swapper loop variants, the colour-table program and the colour tables
    vitaPalSwapFree();
    if (modernGl->colorPalettizerOk) freeShader(&modernGl->colorPalettizer);
    modernGl->colorPalettizerOk = false;
    repeat(modernGl->palLutCount, i) if (modernGl->palLuts[i].color != 0) glDeleteTextures(1, &modernGl->palLuts[i].color);
#endif
    freeShader(modernGl->defaultShaderProgram);
    free(modernGl->defaultShaderProgram);
    if (modernGl->fastDefaultProgram != nullptr) {
        freeShader(modernGl->fastDefaultProgram);
        free(modernGl->fastDefaultProgram);
    }
    if (hasVAO()) {
        glDeleteVertexArrays(1, &modernGl->vao);
        glDeleteVertexArrays(1, &modernGl->vertexBufferVao);
    }
    glDeleteBuffers(1, &modernGl->vbo);
    glDeleteBuffers(1, &modernGl->ebo);

    free(modernGl->uWorldViewProjection);
    free(modernGl->uFogColor);
    free(modernGl->uWorldView);
    free(modernGl->uFogRange);
    free(modernGl->uAlphaTestRef);
    free(modernGl->uAlphaTestEnabled);
    free(modernGl->uTexture);

    for (uint32_t i = 0; i < gl->surfaceCount; i++) {
        if (modernGl->surfaceDepth[i]) glDeleteRenderbuffers(1, &modernGl->surfaceDepth[i]);
    }
    free(modernGl->surfaceDepth);
    free(modernGl->primitiveVertices);
#ifdef PLATFORM_VITA
    // A prefetch worker may still be writing into a page's texture memory: wait for it before the textures go.
    GLRenderer_vitaFinishPrefetch(gl);
    vitaPrefetchCount = 0;
#endif
    GLCommon_destroy(renderer);
}

static void glSetDepthState(Renderer* renderer, bool test, bool write, bool cull) {
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
    if (modernGl->depthTest == test && modernGl->depthWrite == write && modernGl->cull == cull) return;
    flushBatch((GLRenderer*)renderer);
    modernGl->depthTest = test;
    modernGl->depthWrite = write;
    modernGl->cull = cull;
    if (test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(write ? GL_TRUE : GL_FALSE);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glApplyCullWinding(renderer);
}

static void glClearDepthBuffer(Renderer* renderer, float depth) {
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
    flushBatch((GLRenderer*)renderer);
#ifdef PLATFORM_VITA
    vitaBeforeGpuWork();
#endif
    glDepthMask(GL_TRUE);
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__SWITCH__)
    glClearDepthf(depth);
#else
    if (modernGl->isGLES) {
        glClearDepthf(depth);
    } else {
        glClearDepth(depth);
    }
#endif
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(modernGl->depthWrite ? GL_TRUE : GL_FALSE);
}

static void glBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    modernGl->batchCount = 0;
    modernGl->currentTextureId = 0;
    
    GLCommon_beginFrame(gl, gameW, gameH, windowW, windowH);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearDepthBuffer(renderer, 1.0f);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

static void glBeginView(Renderer* renderer, MAYBE_UNUSED int32_t viewX, MAYBE_UNUSED int32_t viewY, MAYBE_UNUSED int32_t viewW, MAYBE_UNUSED int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, MAYBE_UNUSED float viewAngle) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
    modernGl->batchCount = 0;
    modernGl->currentTextureId = 0;

    GLCommon_beginView(renderer, portX, portY, portW, portH, GL_TEXTURE1, glApplyProjection);
    VMBuiltins_applyD3DDefaultView(renderer, viewX, viewY, viewW, viewH, viewAngle);
    glClearDepthBuffer(renderer, 1.0f);

    if (hasVAO()) glBindVertexArray(modernGl->vao);
}

static void glEndView(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    GLCommon_endView();
}

static void glBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
    // surface_reset_target rebinds the GUI target through this same callback.
    // Save world state only on entry, not again while already inside the pass.
    if (!modernGl->guiActive) {
        modernGl->savedDepthTest = modernGl->depthTest;
        modernGl->savedDepthWrite = modernGl->depthWrite;
        modernGl->savedCull = modernGl->cull;
        modernGl->savedGUIWorld = renderer->gmlMatrices[MATRIX_WORLD];
        modernGl->guiActive = true;
    }
    glSetDepthState(renderer, false, false, false);
    Matrix4f identity;
    Matrix4f_identity(&identity);
    renderer->vtable->setMatrix(renderer, MATRIX_WORLD, identity);

    modernGl->batchCount = 0;
    modernGl->currentTextureId = 0;

    GLCommon_beginGUI(
        renderer, targetSurfaceId, modernGl->hostFramebuffer, GL_TEXTURE1, glApplyProjection,
        guiW, guiH, portX, portY, portW, portH
    );

    if (hasVAO()) glBindVertexArray(modernGl->vao);
}

static void glSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED bool renderingToUserSurface) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    GLCommon_setGuiProjection(renderer, renderingToUserSurface, glApplyProjection, guiW, guiH);
}

static void glEndGUI(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    glDisable(GL_SCISSOR_TEST);
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
    renderer->vtable->setMatrix(renderer, MATRIX_WORLD, modernGl->savedGUIWorld);
    glSetDepthState(renderer, modernGl->savedDepthTest, modernGl->savedDepthWrite, modernGl->savedCull);
    modernGl->guiActive = false;
}

static void glEndFrameInit(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    if (hasVAO()) glBindVertexArray(0);

    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        glBindFramebuffer(GL_FRAMEBUFFER, modernGl->hostFramebuffer);
        return;
    }
}

static void glEndFrameEnd(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        return;
    }
    int32_t appId = gl->base.runner->applicationSurfaceId;

    if (modernGl->isGL3) {
        GLCommon_beginLetterboxBlit(gl->surfaces[appId], modernGl->hostFramebuffer);
        GLCommon_endLetterboxBlit(gl->surfaceWidth[appId], gl->surfaceHeight[appId], gl->gameW, gl->gameH, gl->windowW, gl->windowH, modernGl->hostFramebuffer);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, modernGl->hostFramebuffer);
        GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
        if (scissorWasEnabled) glDisable(GL_SCISSOR_TEST);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
#ifdef PLATFORM_VITA
        vitaBeforeGpuWork();
#endif
        glClear(GL_COLOR_BUFFER_BIT);

        glViewport(0, 0, gl->windowW, gl->windowH);

        renderer->vtable->setGuiProjection(renderer, gl->windowW, gl->windowH, gl->windowW, gl->windowH, false);
        glDisable(GL_BLEND);

        int32_t sx, sy, ex, ey;
        GLCommon_computeLetterbox(gl->gameW, gl->gameH, gl->windowW, gl->windowH, &sx, &sy, &ex, &ey);
        float scaleX = (float)(ex - sx) / (float)gl->gameW;
        float scaleY = (float)(ey - sy) / (float)gl->gameH;

        renderer->vtable->drawSurface(renderer, appId, 0, 0, gl->gameW, gl->gameH, (float)sx, (float)sy, scaleX, scaleY, 0.0f, 0xFFFFFF, 1.0f);
        flushBatch(gl);

        glEnable(GL_BLEND);
        if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST);
    }
}

static void glRendererFlush(Renderer* renderer) {
    flushBatch((GLRenderer*) renderer);
}

static void glClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
#ifdef PLATFORM_VITA
    vitaBeforeGpuWork();
    vitaNoteUnknownWrite(gl); // vitaGL's clear ignores the colour mask
#endif

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // GML draw_clear ignores the active scissor and clears the whole target. Disable scissor for the clear and restore it after.
    //No it doesn't?
    glClearColor(r, g, b, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
    // HTML5 draw_clear/draw_clear_alpha also clear depth on the current target.
    glClearDepthBuffer(renderer, 1.0f);
}

// Lazily decodes and uploads a TXTR page on first access.
// Returns true if the texture is ready, false if it failed to decode.
bool GLRenderer_ensureTextureLoaded(GLRenderer* gl, uint32_t pageId) {
    if (gl->textureLoaded[pageId]) return (gl->textureWidths[pageId] != 0);

#if defined(PLATFORM_VITA)
    {
        // Big page: the worker thread's job (normally joined before the first frame by GLRenderer_vitaFinishPrefetch),
        // or, without a worker (notexprefetch), the same decode here with the texture cache. On failure: the old path.
        VitaPrefetch* p = vitaPrefetchFor(pageId);
        int32_t w, h;
        if (p != nullptr) {
            if (p->pending && vitaPrefetchJoin(gl, p)) return true;
        } else if (vitaPrefetchCount < VITA_PREFETCH_MAX && vitaStreamCandidate(gl, pageId, &w, &h)) {
            p = &vitaPrefetch[vitaPrefetchCount];
            if (vitaSetupJob(gl, p, pageId, w, h)) {
                p->used = true;
                vitaPrefetchCount++;
                glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
                uint64_t t0 = bsBootNowUs();
                p->ok = TxtrStream_decodePage(&p->job, &p->rowsDone, &p->stats) ? 1 : 0;
                vitaLogPageDecode(p, "decoded at first use", bsBootNowUs() - t0);
                if (p->ok) { vitaFinishPage(gl, pageId, w, h); return true; }
            }
        }
    }
#endif

    gl->textureLoaded[pageId] = true;

#if defined(PLATFORM_VITA)
    if (VitaTextures_Active()) {
        glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
        if (!VitaTextures_LoadPage(pageId, &gl->textureWidths[pageId], &gl->textureHeights[pageId])) {
            logError("GL: Failed to load Vita TXTR page %u", pageId);
            return false;
        }
        GLCommon_applyTexFilter(gl->base.texFilter);
        logInfo("GL: Loaded TXTR page %u (%dx%d)\n", pageId, gl->textureWidths[pageId], gl->textureHeights[pageId]);
        return true;
    }
#endif

    DataWin* dw = gl->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    DataWin_loadTxtrIfNeeded(dw, pageId);

    int w, h;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
#if defined(PLATFORM_VITA)
    // Big BZ2+QOI pages (e.g. 4096x4096) can't be decoded through malloc'd intermediates on the Vita
    // (~144 MB peak): stream-decode straight into the texture's own memory instead.
    if (txtr->externalPath == nullptr && txtr->blobData != nullptr &&
            ImageDecoder_isBz2Qoi(txtr->blobData, txtr->blobSize, &w, &h) &&
            (w % 8) == 0 && (size_t) w * (size_t) h >= (size_t) 1024 * 1024) {
        glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        uint8_t* texMem = (uint8_t*) vglGetTexDataPointer(GL_TEXTURE_2D);
        if (texMem == nullptr || !ImageDecoder_streamBz2QoiInto(txtr->blobData, txtr->blobSize, gm2022_5, texMem, (size_t) w * 4)) {
            logWarn("GL: Failed to stream-decode TXTR page %u (%dx%d)\n", pageId, w, h);
            return false;
        }
        if (!txtr->mapped) { free(txtr->blobData); txtr->blobData = nullptr; }
        gl->textureWidths[pageId] = w;
        gl->textureHeights[pageId] = h;
        bool isPOT = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
        GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        GLCommon_applyTexFilter(gl->base.texFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);
        logInfo("GL: Stream-loaded TXTR page %u (%dx%d)\n", pageId, w, h);
        return true;
    }
#endif
    uint8_t* externalData = nullptr;
    int32_t externalSize = 0;
    if (txtr->externalPath != nullptr && gl->base.runner != nullptr)
        gl->base.runner->fileSystem->vtable->readFileBinary(gl->base.runner->fileSystem,
            txtr->externalPath, &externalData, &externalSize);
    uint8_t* pixels = ImageDecoder_decodeToRgba(externalData ? externalData : txtr->blobData,
        externalData ? (size_t)externalSize : (size_t)txtr->blobSize, gm2022_5, &w, &h);
    free(externalData);
    if (pixels == nullptr) {
        logWarn("GL: Failed to decode TXTR page %u\n", pageId);
        return false;
    }
    if (!txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    } else if (txtr->blobData && txtr->blobSize) {
        dropMappedRange(txtr->blobData, 0, txtr->blobSize);
    }

    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;
#if defined(PLATFORM_VITA)
    // "Opaque draws (Vita)": a page whose texels are all opaque (the palettes, the CRT noise / mask / rainbow pages)
    if (pageId < gl->originalTexturePageCount && w > 0 && h > 0) {
        if (vitaPageCount < gl->originalTexturePageCount) {
            vitaPageOpaque = (uint8_t*) safeRealloc(vitaPageOpaque, gl->originalTexturePageCount);
            memset(vitaPageOpaque + vitaPageCount, 0, gl->originalTexturePageCount - vitaPageCount);
            vitaPageCount = gl->originalTexturePageCount;
        }
        bool opaque = true;
        for (size_t i = 3, n = (size_t) w * (size_t) h * 4; i < n && opaque; i += 4) opaque = pixels[i] == 255;
        vitaPageOpaque[pageId] = opaque ? 1 : 0;
    }
#endif

    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);
    glTexImage2D(GL_TEXTURE_2D, 0, gl->textureFormat, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    free(pixels);

    bool isPOT = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;

    GLCommon_applyTexFilter(gl->base.texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);

    logInfo("GL: Loaded TXTR page %u (%dx%d)\n", pageId, gl->textureWidths[pageId], gl->textureHeights[pageId]);
    return true;
}

// Resolves a TPAG index to a loaded GL texture. Returns false if drawing should be skipped.
static bool resolveSpriteTexture(GLRenderer* gl, int32_t tpagIndex, TexturePageItem** outTpag, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH) {
    DataWin* dw = gl->base.dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return false;
    if (!GLRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return false;
    *outTpag = tpag;
    *outTexId = gl->glTextures[pageId];
    *outTexW = gl->textureWidths[pageId];
    *outTexH = gl->textureHeights[pageId];
    return true;
}

// Emits a single textured quad into the batch given 4 final screen-space corners (TL, TR, BR, BL), 4 UVs forming a rect (u0,v0)-(u1,v1), and a flat color/alpha.
// Handles texture rebinding and batch flushing.
static void emitTexturedQuad(
    GLRenderer* gl, GLuint texId,
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r0, uint8_t g0, uint8_t b0,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    uint8_t r3, uint8_t g3, uint8_t b3,
    float alpha
) {
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    flushIfNeededAndSetActiveState(gl, BATCHTYPE_QUAD, texId);

    GlVertex* verts = gl->vertexData + modernGl->batchCount * VERTICES_PER_QUAD;
    uint8_t ca = floatToUnormByte(alpha);
#ifdef PLATFORM_VITA
    if (modernGl->batchCount == 0) { vitaBatchAlpha255 = true; vitaBatchCovers = false; } // a new batch
    if (ca != 255) vitaBatchAlpha255 = false;
#endif

    verts[0].x = x0; verts[0].y = y0; verts[0].u = u0; verts[0].v = v0; verts[0].r = r0; verts[0].g = g0; verts[0].b = b0; verts[0].a = ca;
    for (int i = 0; i < 4; i++) verts[i].z = gl->base.drawDepth;
    verts[1].x = x1; verts[1].y = y1; verts[1].u = u1; verts[1].v = v0; verts[1].r = r1; verts[1].g = g1; verts[1].b = b1; verts[1].a = ca;
    verts[2].x = x2; verts[2].y = y2; verts[2].u = u1; verts[2].v = v1; verts[2].r = r2; verts[2].g = g2; verts[2].b = b2; verts[2].a = ca;
    verts[3].x = x3; verts[3].y = y3; verts[3].u = u0; verts[3].v = v1; verts[3].r = r3; verts[3].g = g3; verts[3].b = b3; verts[3].a = ca;

    modernGl->batchCount++;
}

static void drawMultiColoredTextureWithTransform(
    GLRenderer* renderer, GLuint textureId, Matrix4f transform,
    float localX0, float localY0, float localX1, float localY1,
    float u0, float v0, float u1, float v1,
    uint8_t r0, uint8_t g0, uint8_t b0,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    uint8_t r3, uint8_t g3, uint8_t b3,
    float alpha
) {
    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0, &y0);
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1, &y1);
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2, &y2);
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3, &y3);

    emitTexturedQuad(
        renderer, textureId,
        x0, y0, x1, y1, x2, y2, x3, y3,
        u0, v0, u1, v1,
        r0, g0, b0,
        r1, g1, b1,
        r2, g2, b2,
        r3, g3, b3,
        alpha
    );
}

static void drawTextureWithTransform(
    GLRenderer* renderer, GLuint textureId, Matrix4f transform,
    float localX0, float localY0, float localX1, float localY1,
    float u0, float v0, float u1, float v1,
    uint32_t color, float alpha
) {
    uint8_t r = (uint8_t) BGR_R(color);
    uint8_t g = (uint8_t) BGR_G(color);
    uint8_t b = (uint8_t) BGR_B(color);

    drawMultiColoredTextureWithTransform(
        renderer, textureId, transform,
        localX0, localY0, localX1, localY1,
        u0, v0, u1, v1,
        r, g, b,
        r, g, b,
        r, g, b,
        r, g, b,
        alpha
    );
}

static void drawTexture(
    GLRenderer* renderer,
    GLuint textureId,
    float x,
    float y,
    // Locals = the coordinate in relation to the sprite "frame" itself, includes originX/originY and trimmed transparency
    float localX0,
    float localY0,
    float localX1,
    float localY1,
    float u0,
    float v0,
    float u1,
    float v1,
    float xscale,
    float yscale,
    float angleDeg,
    uint32_t color,
    float alpha
) {
    // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
    // GML rotation is counter-clockwise, OpenGL rotation is counter-clockwise, but
    // since we have Y-down, we negate the angle to get the correct visual rotation
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    drawTextureWithTransform(
        renderer,
        textureId,
        transform,
        localX0,
        localY0,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        color,
        alpha
    );
}

static void glDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH)) return;

    // Compute normalized UVs from TPAG source rect
    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->targetWidth;
    float localY1 = localY0 + (float) tpag->targetHeight;

    drawTexture(
        gl,
        texId,
        x,
        y,
        localX0,
        localY0,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        xscale,
        yscale,
        angleDeg,
        color,
        alpha
    );
}

static void drawTiled(
    GLRenderer* gl,
    GLuint texId,
    float gridX,
    float gridY,
    float tileW,
    float tileH,
    bool tileX,
    bool tileY,
    float roomW,
    float roomH,
    float quadOffsetX0,
    float quadW,
    float quadOffsetY0,
    float quadH,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t color,
    float alpha
) {
    if (0 >= tileW || 0 >= tileH) return;
    bsPhase = "draw tiled";

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(gridX, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = gridX;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(gridY, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = gridY;
        endY = startY + tileH;
    }

    // Optimization for 2D affine projects: Clip the tiled extent to the world-space AABB of what the active projection can see.
    const Matrix4f* projection = &gl->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    Matrix4f invProjection;
    if (Matrix4f_isAffine2D(projection) && Matrix4f_inverse(&invProjection, projection)) {
        // The borders of the screen
        static const float ndcCorners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
        float visMinX = 0.0f, visMinY = 0.0f, visMaxX = 0.0f, visMaxY = 0.0f;
        // For each point, get the borders of it in world-space.
        repeat(4, i) {
            float wx, wy;
            Matrix4f_transformPoint(&invProjection, ndcCorners[i][0], ndcCorners[i][1], &wx, &wy);
            if (i == 0 || wx < visMinX) visMinX = wx;
            if (i == 0 || wx > visMaxX) visMaxX = wx;
            if (i == 0 || wy < visMinY) visMinY = wy;
            if (i == 0 || wy > visMaxY) visMaxY = wy;
        }
        if (tileX) {
            // Snap start forward by whole tiles so the tile containing visMinX is kept
            if (visMinX > startX) startX += floorf((visMinX - startX) / tileW) * tileW;
            if (visMaxX < endX) endX = visMaxX;
        }
        if (tileY) {
            if (visMinY > startY) startY += floorf((visMinY - startY) / tileH) * tileH;
            if (visMaxY < endY) endY = visMaxY;
        }
    }
    if (startX >= endX || startY >= endY) return;

    uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);

    // Integer tile counts avoid FP-comparison drift; the inner break handles overshoot at the boundary
    int32_t tilesX = (int32_t) ((endX - startX) / tileW) + 1;
    int32_t tilesY = (int32_t) ((endY - startY) / tileH) + 1;
    if (0 >= tilesX || 0 >= tilesY) return;

    repeat(tilesY, iy) {
        float dy = startY + (float) iy * tileH;
        if (dy >= endY) break;
        float vy0 = dy + quadOffsetY0;
        float vy1 = vy0 + quadH;
        repeat(tilesX, ix) {
            float dx = startX + (float) ix * tileW;
            if (dx >= endX) break;
            float vx0 = dx + quadOffsetX0;
            float vx1 = vx0 + quadW;
            emitTexturedQuad(gl, texId, vx0, vy0, vx1, vy0, vx1, vy1, vx0, vy1, u0, v0, u1, v1, r, g, b, r, g, b, r, g, b, r, g, b, alpha);
        }
    }
    bsPhase = "draw";
}

static void glDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH)) return;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    // Per-tile quad origin = grid cell (dx) + originX*axScale (cancels the grid anchor) + xscale*localX0
    float quadOffX0 = originX * axScale + xscale * localX0;
    float quadOffY0 = originY * ayScale + yscale * localY0;
    float quadW = xscale * (float) tpag->targetWidth;
    float quadH = yscale * (float) tpag->targetHeight;

    drawTiled(
        gl,
        texId,
        x - originX * axScale,
        y - originY * ayScale,
        tileW,
        tileH,
        tileX,
        tileY,
        roomW,
        roomH,
        quadOffX0,
        quadW,
        quadOffY0,
        quadH,
        u0,
        v0,
        u1,
        v1,
        color,
        alpha
    );
}

static void glDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!GLRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

    // Compute UVs for the sub-region within the atlas
    float u0 = (float) (tpag->sourceX + srcOffX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / (float) texH;

    // Convert BGR colors to RGB bytes
    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);

    // Quad corners (no origin offset - draw_sprite_part ignores sprite origin)
    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    if (angleDeg == 0.0f) {
        cx0 = x;                         cy0 = y;
        cx1 = x + (float) srcW * xscale; cy1 = y;
        cx2 = x + (float) srcW * xscale; cy2 = y + (float) srcH * yscale;
        cx3 = x;                         cy3 = y + (float) srcH * yscale;
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float qx0 = x,                         qy0 = y;
        float qx1 = x + (float) srcW * xscale, qy1 = y;
        float qx2 = x + (float) srcW * xscale, qy2 = y + (float) srcH * yscale;
        float qx3 = x,                         qy3 = y + (float) srcH * yscale;
        float dx, dy;
        dx = qx0 - pivotX; dy = qy0 - pivotY; cx0 = cosA * dx - sinA * dy + pivotX; cy0 = sinA * dx + cosA * dy + pivotY;
        dx = qx1 - pivotX; dy = qy1 - pivotY; cx1 = cosA * dx - sinA * dy + pivotX; cy1 = sinA * dx + cosA * dy + pivotY;
        dx = qx2 - pivotX; dy = qy2 - pivotY; cx2 = cosA * dx - sinA * dy + pivotX; cy2 = sinA * dx + cosA * dy + pivotY;
        dx = qx3 - pivotX; dy = qy3 - pivotY; cx3 = cosA * dx - sinA * dy + pivotX; cy3 = sinA * dx + cosA * dy + pivotY;
    }

    emitTexturedQuad(gl, texId, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3, u0, v0, u1, v1, r1, g1, b1, r2, g2, b2, r3, g3, b3, r4, g4, b4, alpha);
}

static void glDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    glDrawSpritePartColor(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, color, color, color, alpha);
}

static void glDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH)) return;

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    emitTexturedQuad(gl, texId, x1, y1, x2, y2, x3, y3, x4, y4, u0, v0, u1, v1, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, alpha);
}

// Emits a single colored quad into the batch using the white pixel texture
static void emitMultiColoredQuad(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    float x3,
    float y3,
    uint8_t r0,
    uint8_t g0,
    uint8_t b0,
    uint8_t r1,
    uint8_t g1,
    uint8_t b1,
    uint8_t r2,
    uint8_t g2,
    uint8_t b2,
    uint8_t r3,
    uint8_t g3,
    uint8_t b3,
    float a
) {
    emitTexturedQuad(
        gl,
        gl->whiteTexture,
        x0,
        y0,
        x1,
        y1,
        x2,
        y2,
        x3,
        y3,
        // Points to the middle of the whiteTexture
        0.5f,
        0.5f,
        0.5f,
        0.5f,
        r0,
        g0,
        b0,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r3,
        g3,
        b3,
        a
    );
}

static void emitColoredQuad(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    float x3,
    float y3,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    float a
) {
    emitTexturedQuad(
        gl,
        gl->whiteTexture,
        x0,
        y0,
        x1,
        y1,
        x2,
        y2,
        x3,
        y3,
        // Points to the middle of the whiteTexture
        0.5f,
        0.5f,
        0.5f,
        0.5f,
        r,
        g,
        b,
        r,
        g,
        b,
        r,
        g,
        b,
        r,
        g,
        b,
        a
    );
}

// Helper method that emits a colored rectangle
static void emitMultiColoredRectangle(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    uint8_t r0,
    uint8_t g0,
    uint8_t b0,
    uint8_t r1,
    uint8_t g1,
    uint8_t b1,
    uint8_t r2,
    uint8_t g2,
    uint8_t b2,
    uint8_t r3,
    uint8_t g3,
    uint8_t b3,
    float a
) {
    emitMultiColoredQuad(
        gl,
        // top-left
        x0,
        y0,
        // top-right
        x1,
        y0,
        // bottom-right
        x1,
        y1,
        // bottom-left
        x0,
        y1,
        r0,
        g0,
        b0,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r3,
        g3,
        b3,
        a
    );
}

// Helper method that emits a colored rectangle
static void emitColoredRectangle(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    float a
) {
    emitMultiColoredRectangle(gl, x0, y0, x1, y1, r, g, b, r, g, b, r, g, b, r, g, b, a);
}

static void glDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        emitColoredRectangle(gl, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha); // top
        emitColoredRectangle(gl, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha); // bottom
        emitColoredRectangle(gl, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha); // left
        emitColoredRectangle(gl, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha); // right
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitColoredRectangle(gl, x1, y1, x2 + 1,y2 + 1, r, g, b, alpha);
    }
}

// ===[ Line Drawing ]===

static void glDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    emitColoredQuad(gl, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, r, g, b, alpha);
}

static void glDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    emitMultiColoredQuad(
        gl,
        x1 + px,
        y1 + py,
        x1 - px,
        y1 - py,
        x2 - px,
        y2 - py,
        x2 + px,
        y2 + py,
        r1,
        g1,
        b1,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r2,
        g2,
        b2,
        alpha
    );
}

static void glDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        glDrawLineColor(renderer, x1, y1, x2, y1, 1.0, color1, color2, alpha);
        glDrawLineColor(renderer, x2, y1, x2, y2, 1.0, color2, color3, alpha);
        glDrawLineColor(renderer, x2, y2, x1, y2, 1.0, color3, color4, alpha);
        glDrawLineColor(renderer, x1, y2, x1, y1, 1.0, color4, color1, alpha);
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitMultiColoredRectangle(
            gl,
            x1,
            y1,
            x2 + 1,
            y2 + 1,
            r1,
            g1,
            b1,
            r2,
            g2,
            b2,
            r3,
            g3,
            b3,
            r4,
            g4,
            b4,
            alpha
        );
    }
}

static void glDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    if (outline) {
        glDrawLineColor(renderer, x1, y1, x2, y2, 1, color1, color2, alpha);
        glDrawLineColor(renderer, x2, y2, x3, y3, 1, color2, color3, alpha);
        glDrawLineColor(renderer, x3, y3, x1, y1, 1, color3, color1, alpha);
    } else {
        flushIfNeededAndSetActiveState(gl, BATCHTYPE_TRIANGLE, gl->whiteTexture);
#ifdef PLATFORM_VITA
        if (modernGl->batchCount == 0) vitaBatchCovers = false;
        vitaBatchAlpha255 = false; // triangle batches aren't analysed
#endif

        // Woo, pointers!
        // This gets the vertex data for the new triangle batch
        GlVertex* verts = gl->vertexData + modernGl->batchCount * VERTICES_PER_TRIANGLE;
        uint8_t ca = floatToUnormByte(alpha);
        for (int i = 0; i < 3; i++) verts[i].z = renderer->drawDepth;

        verts[0].x = x1; verts[0].y = y1; verts[0].u = 0.0f; verts[0].v = 0.0f; verts[0].r = (uint8_t) BGR_R(color1); verts[0].g = (uint8_t) BGR_G(color1); verts[0].b = (uint8_t) BGR_B(color1); verts[0].a = ca;
        verts[1].x = x2; verts[1].y = y2; verts[1].u = 0.0f; verts[1].v = 0.0f; verts[1].r = (uint8_t) BGR_R(color2); verts[1].g = (uint8_t) BGR_G(color2); verts[1].b = (uint8_t) BGR_B(color2); verts[1].a = ca;
        verts[2].x = x3; verts[2].y = y3; verts[2].u = 0.0f; verts[2].v = 0.0f; verts[2].r = (uint8_t) BGR_R(color3); verts[2].g = (uint8_t) BGR_G(color3); verts[2].b = (uint8_t) BGR_B(color3); verts[2].a = ca;

        modernGl->batchCount++;
    }
}

static int vertex_type_components(int type)
{
    switch (type) {
        case VERTEX_TYPE_FLOAT1:
            return 1;
        case VERTEX_TYPE_FLOAT2:
            return 2;
        case VERTEX_TYPE_FLOAT3:
            return 3;
        case VERTEX_TYPE_FLOAT4:
            return 4;
        case VERTEX_TYPE_UBYTE4:
            return 4;
        case VERTEX_TYPE_COLOR:
            return 4;
    }

    return 4;
}

static bool glResolveTextureHandle(GLRenderer* gl, uint32_t texHandle, TexturePageItem** outTpag, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH);

static void glDrawVertexBuffer(MAYBE_UNUSED Renderer* renderer, VertexBuffer* buffer, int32_t primitive, int32_t texture, int32_t offset, int32_t number) {
    if (!buffer || !buffer->format)
        return;

    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;
    flushBatch(gl);
#ifdef PLATFORM_VITA
    vitaBeforeGpuWork();
#endif

    typedef struct {
        GLuint vbo;
    } GLVertexBuffer;

    GLVertexBuffer *glBuffer = (GLVertexBuffer *)buffer->rendererData;

    if (!glBuffer) {
        glBuffer = (GLVertexBuffer *)malloc(sizeof(*glBuffer));
        glGenBuffers(1, &glBuffer->vbo);
        buffer->rendererData = (void *) glBuffer;
    }

    GLenum mode = primitiveTypeToGL(primitive);

    if (hasVAO()) {
        glBindVertexArray(modernGl->vertexBufferVao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, glBuffer->vbo);
    glBufferData(GL_ARRAY_BUFFER, buffer->size, buffer->data, GL_DYNAMIC_DRAW);

    for (int i = 0; i < 4; i++) glDisableVertexAttribArray(i);

    bool hasColor = false;
    bool hasTexcoord = false;

    for (int i = 0; i < buffer->format->numElements; i++) {
        VertexElement *e = &buffer->format->elements[i];

        switch (e->usage) {
            case VERTEX_USAGE_POSITION:
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(
                    0,
                    vertex_type_components(e->type),
                    GL_FLOAT,
                    GL_FALSE,
                    buffer->format->stride,
                    (void*)(intptr_t)e->offset
                );
                break;

            case VERTEX_USAGE_COLOR:
                hasColor = true;
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(
                    1,
                    4,
                    GL_UNSIGNED_BYTE,
                    GL_TRUE,
                    buffer->format->stride,
                    (void*)(intptr_t)e->offset
                );
                break;

            case VERTEX_USAGE_NORMAL:
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(
                    3,
                    3,
                    GL_FLOAT,
                    GL_FALSE,
                    buffer->format->stride,
                    (void*)(intptr_t)e->offset
                );
                break;

            case VERTEX_USAGE_TEXCOORD:
                hasTexcoord = true;
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(
                    2,
                    2,
                    GL_FLOAT,
                    GL_FALSE,
                    buffer->format->stride,
                    (void*)(intptr_t)e->offset
                );
                break;
        }
    }

    if (!hasColor) {
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    if (!hasTexcoord) {
        glDisableVertexAttribArray(2);
        glVertexAttrib2f(2, 0.5f, 0.5f);
    }

    GLuint drawTexture = gl->whiteTexture;
    if (texture != -1) {
        TexturePageItem* textureTpag = nullptr;
        GLuint resolvedTexId = 0;
        int32_t resolvedTexW = 0;
        int32_t resolvedTexH = 0;

        if (glResolveTextureHandle(gl, (uint32_t) texture, &textureTpag, &resolvedTexId, &resolvedTexW, &resolvedTexH) && resolvedTexId != 0) {
            drawTexture = resolvedTexId;
        } else if (glIsTexture((GLuint) texture)) {
            // Backward compatibility with callers that already pass raw GL ids.
            drawTexture = (GLuint) texture;
        }
    }

    // Position/color-only vertex formats should render untextured.
    if (!hasTexcoord) {
        drawTexture = gl->whiteTexture;
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, drawTexture);

    int vertexCount = buffer->size / buffer->format->stride;
    if (vertexCount <= 0) {
        if (hasVAO()) glBindVertexArray(modernGl->vao);
        else for (int i = 0; i < 4; i++) glDisableVertexAttribArray(i);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    if (offset < 0) offset = 0;
    if (offset > vertexCount) offset = vertexCount;

    if (number == -1) {
        number = vertexCount - offset;
    } else {
        if (number < 0) number = 0;
        if (offset + number > vertexCount) {
            number = vertexCount - offset;
        }
    }

    if (number <= 0) {
        if (hasVAO()) glBindVertexArray(modernGl->vao);
        else for (int i = 0; i < 4; i++) glDisableVertexAttribArray(i);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    glDrawArrays(
        mode,
        offset,
        number
    );
#ifdef PLATFORM_VITA
    vitaNoteUnknownWrite(gl);
#endif

    if (hasVAO()) glBindVertexArray(modernGl->vao);
    else for (int i = 0; i < 4; i++) glDisableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ===[ Text Drawing ]===

// Resolved font state shared between glDrawText and glDrawTextColor
typedef struct {
    Font* font;
    TexturePageItem* fontTpag; // single TPAG for regular fonts (nullptr for sprite fonts)
    GLuint texId;
    int32_t texW, texH;
    Sprite* spriteFontSprite; // source sprite for sprite fonts (nullptr for regular fonts)
} GlFontState;

// ===[ Debug UI font (drawTextUI) ]===
// drawTextUI must not depend on game fonts (data.win may ship none), so it uses
// the embedded debug font from gl_common.h, uploaded as its own GL texture.
// A synthetic Font + GlFontState pair is built per renderer instance and passed
// as real pointers into drawText().

// Resolves font texture state
// Returns false if the font can't be drawn
static bool glResolveFontState(GLRenderer* gl, DataWin* dw, Font* font, GlFontState* state) {
    state->font = font;
    state->fontTpag = nullptr;
    state->texId = 0;
    state->texW = 0;
    state->texH = 0;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (0 > fontTpagIndex) return false;

        state->fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= gl->textureCount) return false;
        if (!GLRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return false;

        state->texId = gl->glTextures[pageId];
        state->texW = gl->textureWidths[pageId];
        state->texH = gl->textureHeights[pageId];
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    return true;
}

// Resolves UV coordinates, texture ID, and local position for a single glyph
// Returns false if the glyph can't be drawn
static bool glResolveGlyph(GLRenderer* gl, DataWin* dw, GlFontState* state, FontGlyph* glyph, float cursorX, float cursorY, GLuint* outTexId, float* outU0, float* outV0, float* outU1, float* outV1, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= gl->textureCount) return false;
        if (!GLRenderer_ensureTextureLoaded(gl, (uint32_t) pid)) return false;

        *outTexId = gl->glTextures[pid];
        int32_t tw = gl->textureWidths[pid];
        int32_t th = gl->textureHeights[pid];

        *outU0 = (float) glyphTpag->sourceX / (float) tw;
        *outV0 = (float) glyphTpag->sourceY / (float) th;
        *outU1 = (float) (glyphTpag->sourceX + glyphTpag->sourceWidth) / (float) tw;
        *outV1 = (float) (glyphTpag->sourceY + glyphTpag->sourceHeight) / (float) th;

        // Sprite-font glyphs sit at the cell offset. GM 2023.2+ subtracts the sprite origin, pre-2023.2 it cancels.
        // (See GameMaker-HTML5's commit a7c5b909209d5a28602fedfe2031965386a99921)
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) (int32_t) glyphTpag->targetY - (float) font->spriteOriginYAdjust;
    } else {
        *outTexId = state->texId;
        *outU0 = (float) (state->fontTpag->sourceX + glyph->sourceX) / (float) state->texW;
        *outV0 = (float) (state->fontTpag->sourceY + glyph->sourceY) / (float) state->texH;
        *outU1 = (float) (state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) state->texW;
        *outV1 = (float) (state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) state->texH;

        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY + GLCommon_debugUIFontYOffset(&gl->debugUI, font, glyph);
    }
    return true;
}

static void drawText(
    Renderer* renderer,
    const char* text,
    float x,
    float y,
    float xscale,
    float yscale,
    float angleDeg,
    float lineSeparation,
    uint32_t _c1,
    uint32_t _c2,
    uint32_t _c3,
    uint32_t _c4,
    float alpha,
    Font *font,
    GlFontState *fs
) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (!font) {
        int32_t fontIndex = renderer->drawFont;
        if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex)
            return;

        font = &dw->font.fonts[fontIndex];
    }

    GlFontState fontState;
    if (!fs) {
        if (!glResolveFontState(gl, dw, font, &fontState))
            return;
    } else
        fontState = *fs;

    int32_t textLen = (int32_t) strlen(text);
    if (textLen == 0)
        return;

    // Count lines, treating \r\n and \n\r as single breaks
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    // Build transform matrix
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    // Iterate through lines. HTML5 subtracts ascenderOffset from per-line y offset.
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    int32_t c1 = _c1;
    int32_t c2 = _c2;
    int32_t c3 = _c3;
    int32_t c4 = _c4;
    bool needsLerpingOnTheFly = c1 != c2 || c2 != c3 || c3 != c4;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        // Pixel-position cursor for the gradient
        float gradientX = 0.0f;

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                float advance = (float) glyph->shift;
                float leftFrac = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                if (needsLerpingOnTheFly) {
                    c1 = Color_lerp(_c1, _c2, leftFrac);
                    c2 = Color_lerp(_c1, _c2, rightFrac);
                    c3 = Color_lerp(_c4, _c3, rightFrac);
                    c4 = Color_lerp(_c4, _c3, leftFrac);
                }

                uint8_t r0 = (uint8_t) BGR_R(c1), g0 = (uint8_t) BGR_G(c1), b0 = (uint8_t) BGR_B(c1);
                uint8_t r1 = (uint8_t) BGR_R(c2), g1 = (uint8_t) BGR_G(c2), b1 = (uint8_t) BGR_B(c2);
                uint8_t r2 = (uint8_t) BGR_R(c3), g2 = (uint8_t) BGR_G(c3), b2 = (uint8_t) BGR_B(c3);
                uint8_t r3 = (uint8_t) BGR_R(c4), g3 = (uint8_t) BGR_G(c4), b3 = (uint8_t) BGR_B(c4);

                bool drewSuccessfully = false;
				if (ch == ' ') {
					drewSuccessfully = true;
                } else if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1;
                    float localX0, localY0;
                    GLuint glyphTexId;

                    if (glResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &glyphTexId, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        float localX1 = localX0 + (float) glyph->sourceWidth;
                        float localY1 = localY0 + (float) glyph->sourceHeight;

                        drawMultiColoredTextureWithTransform(
                            gl,
                            glyphTexId,
                            transform,
                            localX0,
                            localY0,
                            localX1,
                            localY1,
                            u0,
                            v0,
                            u1,
                            v1,
                            r0,
                            g0,
                            b0,
                            r1,
                            g1,
                            b1,
                            r2,
                            g2,
                            b2,
                            r3,
                            g3,
                            b3,
                            alpha
                        );
                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                gradientX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static void glDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    drawText(
        renderer,
        text,
        x,
        y,
        xscale,
        yscale,
        angleDeg,
        lineSeparation,
        renderer->drawColor,
        renderer->drawColor,
        renderer->drawColor,
        renderer->drawColor,
        renderer->drawAlpha,
        nullptr,
        nullptr
    );
}

static void glDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha, float lineSeparation) {
    drawText(
        renderer,
        text,
        x,
        y,
        xscale,
        yscale,
        angleDeg,
        lineSeparation,
        _c1,
        _c2,
        _c3,
        _c4,
        alpha,
        nullptr,
        nullptr
    );
}

static void glDrawTextUI(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha, float lineSeparation) {
    if (text == nullptr) return;
    GLRenderer* gl = (GLRenderer*) renderer;
    GLCommon_initDebugUIFont(&gl->debugUI);
    if (!GLCommon_ensureDebugFontTexture(gl, &gl->debugUI)) return;

    GlFontState fs;
    fs.font = &gl->debugUI.font;
    fs.fontTpag = &gl->debugUI.tpag;
    fs.texId = gl->debugUI.texture;
    fs.texW = DEBUGFONT_ATLAS_W;
    fs.texH = DEBUGFONT_ATLAS_H;
    fs.spriteFontSprite = nullptr;

    drawText(
        renderer,
        text,
        x,
        y,
        xscale,
        yscale,
        angleDeg,
        lineSeparation,
        _c1,
        _c2,
        _c3,
        _c4,
        alpha,
        fs.font,
        &fs
    );
}

// ===[ Dynamic Sprite Creation/Deletion ]===

// Finds a free dynamic texture page slot (glTextures[i] == 0), or appends a new one.
static uint32_t findOrAllocTexturePageSlot(GLRenderer* gl) {
    // Scan dynamic range for a reusable slot
    for (uint32_t i = gl->originalTexturePageCount; gl->textureCount > i; i++) {
        if (gl->glTextures[i] == 0) return i;
    }
    // No free slot found, grow the arrays
    uint32_t newPageId = gl->textureCount;
    gl->textureCount++;
    gl->glTextures = (GLuint *)safeRealloc(gl->glTextures, gl->textureCount * sizeof(GLuint));
    gl->textureWidths = (int32_t *)safeRealloc(gl->textureWidths, gl->textureCount * sizeof(int32_t));
    gl->textureHeights = (int32_t *)safeRealloc(gl->textureHeights, gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = (bool *)safeRealloc(gl->textureLoaded, gl->textureCount * sizeof(bool));
    gl->glTextures[newPageId] = 0;
    gl->textureWidths[newPageId] = 0;
    gl->textureHeights[newPageId] = 0;
    gl->textureLoaded[newPageId] = false;
    return newPageId;
}

// Finds a free dynamic TPAG slot (texturePageId == -1), or appends a new one.
static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = (TexturePageItem *)safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t glCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    // Save the current FBO binding so creating a surface doesn't change the active render target.
    GLint prevBinding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);

    uint32_t surfaceIndex = GLCommon_allocateSurfaceSlot(&gl->surfaces, &gl->surfaceTexture, &gl->surfaceWidth, &gl->surfaceHeight, &gl->surfaceCount);
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
    modernGl->surfaceDepth = (GLuint*)safeRealloc(modernGl->surfaceDepth, gl->surfaceCount * sizeof(GLuint));
    modernGl->surfaceDepth[surfaceIndex] = 0;

    glGenFramebuffers(1, &gl->surfaces[surfaceIndex]);

    glGenTextures(1, &gl->surfaceTexture[surfaceIndex]);
    glBindTexture(GL_TEXTURE_2D, gl->surfaceTexture[surfaceIndex]);
    glTexImage2D(GL_TEXTURE_2D, 0, GLCommon_surfaceInternalFormat(gl), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    bool isPOT = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
    GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;

    GLCommon_applyTexFilter(renderer->texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);


    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceIndex]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->surfaceTexture[surfaceIndex], 0);
    glGenRenderbuffers(1, &modernGl->surfaceDepth[surfaceIndex]);
    glBindRenderbuffer(GL_RENDERBUFFER, modernGl->surfaceDepth[surfaceIndex]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, modernGl->surfaceDepth[surfaceIndex]);
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearDepthBuffer(renderer, 1.0f);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        logError("GL: Incomplete depth-capable surface %u\n", surfaceIndex);

    gl->surfaceWidth[surfaceIndex] = width;
    gl->surfaceHeight[surfaceIndex] = height;
#ifdef PLATFORM_VITA
    vitaSetSurfaceOpaque((int32_t) surfaceIndex, false);
#endif

    logInfo("GL: Created surface %u with size (%dx%d)\n", surfaceIndex, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);

    return (int32_t) surfaceIndex;
}

static int32_t glEnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    GLRenderer* gl = (GLRenderer*) renderer;
    int32_t id = renderer->runner->applicationSurfaceId;

    bool needsCreate = (id < 0) || ((uint32_t) id >= gl->surfaceCount) || (gl->surfaces[id] == 0);
    if (needsCreate) {
        id = glCreateSurface(renderer, width, height);
        // Publish immediately so anything that re-queries the runner during this frame sees the new ID.
        renderer->runner->applicationSurfaceId = id;
        return id;
    }

    if (gl->surfaceWidth[id] != width || gl->surfaceHeight[id] != height) {
        renderer->vtable->surfaceResize(renderer, id, width, height);
    }
    return id;
}

static void glSurfaceFree(Renderer* renderer, int32_t surfaceID) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;

    // Freeing the application_surface is a no-op from GML; the runner manages its lifecycle via application_surface_enable.
    if (surfaceID == renderer->runner->applicationSurfaceId) return;
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
#ifdef PLATFORM_VITA
    vitaCrtBakeSurfaceFreed(modernGl, gl->surfaces[surfaceID]); // CRT bake textures drawn for this surface
#endif
    if (modernGl->surfaceDepth[surfaceID]) glDeleteRenderbuffers(1, &modernGl->surfaceDepth[surfaceID]);
    modernGl->surfaceDepth[surfaceID] = 0;

    if (gl->surfaceTexture[surfaceID] != 0) glDeleteTextures(1, &gl->surfaceTexture[surfaceID]);
    if (gl->surfaces[surfaceID] != 0) glDeleteFramebuffers(1, &gl->surfaces[surfaceID]);
    gl->surfaces[surfaceID] = 0;
    gl->surfaceTexture[surfaceID] = 0;
    gl->surfaceWidth[surfaceID] = 0;
    gl->surfaceHeight[surfaceID] = 0;
#ifdef PLATFORM_VITA
    vitaSetSurfaceOpaque(surfaceID, false);
#endif
    logInfo("GL: Freed Surface %u\n", surfaceID);
}

static void glSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaces[surfaceID] == 0) return;
    if (gl->surfaceWidth[surfaceID] == width && gl->surfaceHeight[surfaceID] == height) return;

    GLint prevBinding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);

    if (gl->surfaceTexture[surfaceID] != 0) glDeleteTextures(1, &gl->surfaceTexture[surfaceID]);

    glGenTextures(1, &gl->surfaceTexture[surfaceID]);
    glBindTexture(GL_TEXTURE_2D, gl->surfaceTexture[surfaceID]);
    glTexImage2D(GL_TEXTURE_2D, 0, GLCommon_surfaceInternalFormat(gl), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceID]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->surfaceTexture[surfaceID], 0);
    GLModernRenderer* modernGl = (GLModernRenderer*)renderer;
    glBindRenderbuffer(GL_RENDERBUFFER, modernGl->surfaceDepth[surfaceID]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearDepthBuffer(renderer, 1.0f);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST);

    gl->surfaceWidth[surfaceID] = width;
    gl->surfaceHeight[surfaceID] = height;
#ifdef PLATFORM_VITA
    vitaSetSurfaceOpaque(surfaceID, false);
#endif

    logInfo("GL: Resized Surface %u Size (%dx%d)\n", surfaceID, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);
}

static bool glSurfaceExists(Renderer* renderer, int32_t surfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    return gl->surfaces[surfaceId] != 0;
}

#ifdef PLATFORM_VITA
// Pixel uploads into a surface make its alpha unknown ("Opaque draws (Vita)").
static bool vitaSurfaceSetPixels(Renderer* renderer, int32_t surfaceId, const uint8_t* rgba) {
    vitaSetSurfaceOpaque(surfaceId, false);
    return GLCommon_surfaceSetPixels(renderer, surfaceId, rgba);
}

static void vitaSurfaceUploadPixels(Renderer* renderer, int32_t surfaceId, int32_t w, int32_t h, const uint8_t* rgba) {
    vitaSetSurfaceOpaque(surfaceId, false);
    GLCommon_surfaceUploadPixels(renderer, surfaceId, w, h, rgba);
}
#endif

static bool glSurfaceGetPixels(Renderer* renderer, int32_t surfaceId, uint8_t* outRGBA) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    return GLCommon_surfaceGetPixels(gl->surfaces, gl->surfaceWidth, gl->surfaceHeight, gl->surfaceCount, surfaceId, outRGBA);
}

static bool glSetRenderTarget(Renderer* renderer, int32_t surfaceId, bool implicitApplicationSurface) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
    viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = &renderer->runner->views[viewCurrent];
    gl->base.cameraCurrent = view->cameraId;
    GMLCamera* camera = Runner_getCameraById(renderer->runner, gl->base.cameraCurrent);

    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    if (gl->surfaces[surfaceId] == 0) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceId]);

    if (surfaceId == renderer->runner->applicationSurfaceId && implicitApplicationSurface) {
        gl->base.CPortX = 0;
        gl->base.CPortY = 0;
        gl->base.CPortW = gl->gameW;
        gl->base.CPortH = gl->gameH;

        glViewport(gl->base.CPortX, gl->base.CPortY, gl->base.CPortW, gl->base.CPortH);
        glEnable(GL_SCISSOR_TEST);
        glScissor(gl->base.CPortX, gl->base.CPortY, gl->base.CPortW, gl->base.CPortH);

        glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);

        return true;
    }


    if (surfaceId == view->surfaceId) {
    //the surface belongs to the view we are rending, we use the view's camera.
    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
    return true;
    } else {
    //camera will use full surface.
    gl->base.cameraCurrent = SURFACE_CAMERA;
    GMLCamera* camera =  &renderer->runner->surfaceCamera;

    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = gl->surfaceWidth[surfaceId];
    camera->viewHeight = gl->surfaceHeight[surfaceId];
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;
    Runner_updateCameraViewSimple(camera);

    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);
    glApplyProjection(renderer, &camera->viewMatrix,&camera->projectionMatrix);
    return true;
    }
}

static void glSurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    flushBatch(gl);

    if (0 > srcSurfaceID || (uint32_t) srcSurfaceID >= gl->surfaceCount || gl->surfaces[srcSurfaceID] == 0) return;
    if (0 > destSurfaceID || (uint32_t) destSurfaceID >= gl->surfaceCount || gl->surfaces[destSurfaceID] == 0) return;

    if (modernGl->isGL3) {
        GLCommon_surfaceBlit(gl->surfaces, gl->surfaceWidth, gl->surfaceHeight, gl->surfaceCount, destSurfaceID, destX, destY, srcSurfaceID, srcX, srcY, srcW, srcH, part);
    } else {
        GLint prevBinding = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);
        Matrix4f prevProj = renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
        bool prevBlend = glIsEnabled(GL_BLEND);
        GLint prevViewport[4];
        glGetIntegerv(GL_VIEWPORT, prevViewport);

        glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[destSurfaceID]);
        glViewport(0, 0, gl->surfaceWidth[destSurfaceID], gl->surfaceHeight[destSurfaceID]);
        glDisable(GL_BLEND);

        renderer->vtable->setGuiProjection(renderer, gl->surfaceWidth[destSurfaceID], gl->surfaceHeight[destSurfaceID], gl->surfaceWidth[destSurfaceID], gl->surfaceHeight[destSurfaceID], true);

        int32_t sX = part ? srcX : 0;
        int32_t sY = part ? srcY : 0;
        int32_t sW = part ? srcW : gl->surfaceWidth[srcSurfaceID];
        int32_t sH = part ? srcH : gl->surfaceHeight[srcSurfaceID];

        renderer->vtable->drawSurface(renderer, srcSurfaceID, sX, sY, sW, sH, (float)destX, (float)destY, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
        flushBatch(gl);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);
        renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = prevProj;
        glShaderSettingsRefresh(renderer);
        if (prevBlend) glEnable(GL_BLEND);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
}

static float glGetSurfaceWidth(Renderer* renderer, int32_t surfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return 0.0f;
    if (gl->surfaces[surfaceId] == 0) return 0.0f;
    return (float) gl->surfaceWidth[surfaceId];
}

static float glGetSurfaceHeight(Renderer* renderer, int32_t surfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return 0.0f;
    if (gl->surfaces[surfaceId] == 0) return 0.0f;
    return (float) gl->surfaceHeight[surfaceId];
}

static void glDrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaceTexture[surfaceID] == 0) return;

    GLuint texId = gl->surfaceTexture[surfaceID];
    int32_t texW = gl->surfaceWidth[surfaceID];
    int32_t texH = gl->surfaceHeight[surfaceID];

    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float) srcLeft / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float localX1 = (float) srcWidth;
    float localY1 = (float) srcHeight;

#ifdef PLATFORM_VITA
    // A draw covering all of the bound surface gets a batch of its own, marked as covering it, so that an alpha-1 draw
    // makes the surface known-opaque (see "Opaque draws (Vita)"). The grid below stays within one batch (3584 quads).
    bool vitaCover = false;
    if (angleDeg == 0.0f && vitaOpaque() != 0) {
        vitaCover = vitaDrawCoversTarget((GLModernRenderer*) gl, vitaBoundSurface(gl), x, y, x + localX1 * xscale, y + localY1 * yscale);
        if (vitaCover) flushBatch(gl);
    }
    // CRT rewrites with per-vertex terms (GMLShader.vitaGrid): draw the surface as a 64x56 grid of quads (8x8 px
    // cells on the 512x448 CRT surfaces, 3584 quads <= MAX_QUADS = one batch). Shared edges use the same
    // expressions, so neighbouring cells' vertices match exactly.
    if (renderer->currentShader >= 0 && angleDeg == 0.0f &&
            ((GLModernRenderer*) gl)->gmlShaders[renderer->currentShader].vitaGrid) {
        enum { CRT_GX = 64, CRT_GY = 56 };
        uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);
        float w = localX1 * xscale, h = localY1 * yscale;
        for (int32_t j = 0; j < CRT_GY; j++) {
            float fy0 = (float) j / CRT_GY, fy1 = (float) (j + 1) / CRT_GY;
            float py0 = y + h * fy0, py1 = y + h * fy1;
            float tv0 = v0 + (v1 - v0) * fy0, tv1 = v0 + (v1 - v0) * fy1;
            for (int32_t i = 0; i < CRT_GX; i++) {
                float fx0 = (float) i / CRT_GX, fx1 = (float) (i + 1) / CRT_GX;
                float px0 = x + w * fx0, px1 = x + w * fx1;
                float tu0 = u0 + (u1 - u0) * fx0, tu1 = u0 + (u1 - u0) * fx1;
                emitTexturedQuad(gl, texId, px0, py0, px1, py0, px1, py1, px0, py1, tu0, tv0, tu1, tv1,
                                 r, g, b, r, g, b, r, g, b, r, g, b, alpha);
            }
        }
        if (vitaCover) vitaBatchCovers = true;
        return;
    }
#endif

    drawTexture(
        gl,
        texId,
        x,
        y,
        0.0f,
        0.0f,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        xscale,
        yscale,
        angleDeg,
        color,
        alpha
    );
#ifdef PLATFORM_VITA
    if (vitaCover) vitaBatchCovers = true;
#endif
}

static void glDrawSurfaceColor(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaceTexture[surfaceID] == 0) return;

    GLuint texId = gl->surfaceTexture[surfaceID];
    int32_t texW = gl->surfaceWidth[surfaceID];
    int32_t texH = gl->surfaceHeight[surfaceID];

    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float) srcLeft / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float localX1 = (float) srcWidth;
    float localY1 = (float) srcHeight;

    uint8_t r1 = (uint8_t) BGR_R(color1);
    uint8_t g1 = (uint8_t) BGR_G(color1);
    uint8_t b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2);
    uint8_t g2 = (uint8_t) BGR_G(color2);
    uint8_t b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3);
    uint8_t g3 = (uint8_t) BGR_G(color3);
    uint8_t b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4);
    uint8_t g4 = (uint8_t) BGR_G(color4);
    uint8_t b4 = (uint8_t) BGR_B(color4);

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);

    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    drawMultiColoredTextureWithTransform(
        gl,
        texId,
        transform,
        0.0f,
        0.0f,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r3,
        g3,
        b3,
        r4,
        g4,
        b4,
        alpha
    );
}

static void glDrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaceTexture[surfaceID] == 0) return;

    GLuint texId = gl->surfaceTexture[surfaceID];
    int32_t texW = gl->surfaceWidth[surfaceID];
    int32_t texH = gl->surfaceHeight[surfaceID];

    float tileW = (float) texW * fabsf(xscale);
    float tileH = (float) texH * fabsf(yscale);

    drawTiled(
        gl,
        texId,
        x,
        y,
        tileW,
        tileH,
        true,
        true,
        roomW,
        roomH,
        0.0f,
        xscale * (float) texW,
        0.0f,
        yscale * (float) texH,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color,
        alpha
    );
}

static int32_t glCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    // TODO: implement these
    (void)smooth;
    (void)removeback;
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 >= w || 0 >= h) return -1;
    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return -1;
    if (gl->surfaces[surfaceID] == 0) return -1;

    // Flush any pending draws before reading pixels
    flushBatch(gl);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceID]);

    uint8_t* pixels = (uint8_t *)safeMalloc((size_t) w * (size_t) h * 4);
    if (pixels == nullptr) return -1;

    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Create a new GL texture from the captured pixels
    GLuint newTexId;
    glGenTextures(1, &newTexId);
    glBindTexture(GL_TEXTURE_2D, newTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, gl->textureFormat, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLCommon_applyTexFilter(renderer->texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    free(pixels);

    // Find or allocate slots for texture page, TPAG, and sprite
    uint32_t pageId = findOrAllocTexturePageSlot(gl);
    gl->glTextures[pageId] = newTexId;
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;
    gl->textureLoaded[pageId] = true;

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, gl->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = (int16_t) pageId;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, gl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t *)safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    logInfo("GL: Created dynamic sprite %u (%dx%d) from surface %d at (%d,%d)\n", spriteIndex, w, h, surfaceID, x, y);
    return (int32_t) spriteIndex;
}

static int32_t glCreateTpagFromPixels(Renderer* renderer, const uint8_t* rgba, int32_t w, int32_t h) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 >= w || 0 >= h) return -1;
    flushBatch(gl);

    GLuint newTexId;
    glGenTextures(1, &newTexId);
    glBindTexture(GL_TEXTURE_2D, newTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, gl->textureFormat, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    GLCommon_applyTexFilter(renderer->texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    uint32_t pageId = findOrAllocTexturePageSlot(gl);
    gl->glTextures[pageId] = newTexId;
    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;
    gl->textureLoaded[pageId] = true;

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, gl->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = (int16_t) pageId;
    return (int32_t) tpagIndex;
}

static void glDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (gl->originalSpriteCount > (uint32_t) spriteIndex) {
        logWarn("GL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Clean up GL texture and TPAG entries owned by this sprite.
    // Slots with index >= originalTpagCount are dynamically allocated and ours to free.
    repeat(sprite->textureCount, i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= gl->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && gl->textureCount > (uint32_t) pageId) {
                glDeleteTextures(1, &gl->glTextures[pageId]);
                gl->glTextures[pageId] = 0;
            }
            // Mark TPAG slot as free for reuse
            tpag->texturePageId = -1;
        }
    }

    // Clear the sprite entry so it won't be drawn and can be reused. Preserve `name` across the memset: the slot is still in sprt.count and must keep a valid string for asset_get_index / name lookups.
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    logInfo("GL: Deleted sprite %d\n", spriteIndex);
}

static BlendFactors glGpuGetBlendFactors(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*)renderer;
    BlendFactors ret;
    ret.src = gl->currentSFactor;
    ret.dst = gl->currentDFactor;
    ret.srcAlpha = gl->currentSFactorAlpha;
    ret.dstAlpha = gl->currentDFactorAlpha;
    return ret;
}

static int32_t glGpuGetBlendMode(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    return gl->currentBlendMode;
}

static void glGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (mode == gl->currentBlendMode) return;
    flushBatch(gl);

    gl->currentBlendMode = mode;
    gl->currentSFactor = GLCommon_blendModeToSFactor(mode);
    gl->currentDFactor = GLCommon_blendModeToDFactor(mode);
    gl->currentSFactorAlpha = gl->currentSFactor;
    gl->currentDFactorAlpha = gl->currentDFactor;

    glBlendEquation(GLCommon_blendModeToEquation(mode));
    glBlendFunc(gl->currentSFactor, gl->currentDFactor);
}

static void glGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (sfactor == gl->currentSFactor && dfactor == gl->currentDFactor && \
            sfactor_alpha == gl->currentSFactorAlpha && dfactor_alpha == gl->currentDFactorAlpha) return;
    flushBatch(gl);
    gl->currentBlendMode = bm_complex;
    gl->currentSFactor = sfactor;
    gl->currentDFactor = dfactor;
    gl->currentSFactorAlpha = sfactor_alpha;
    gl->currentDFactorAlpha = dfactor_alpha;

    glBlendFuncSeparate(
        GLCommon_blendFactorToGL(sfactor),
        GLCommon_blendFactorToGL(dfactor),
        GLCommon_blendFactorToGL(sfactor_alpha),
        GLCommon_blendFactorToGL(dfactor_alpha)
    );
}

static void glGpuSetBlendEnable(Renderer* renderer, bool enable) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->blendEnable == enable) return;
    flushBatch(gl);
    enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    gl->blendEnable = enable;
}

static void glGpuSetTexFilter(Renderer* renderer, bool enable) {
    if (renderer->texFilter == enable) return;
    flushBatch((GLRenderer*) renderer);
    GLCommon_setTexFilter(renderer, enable);
}

static bool glGpuGetBlendEnable(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    return gl->blendEnable;
}

static void glGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->alphaTestEnable == enable) return;
    flushBatch(gl);
    gl->alphaTestEnable = enable;
    glShaderSettingsRefresh(renderer);
}

static bool glGpuGetAlphaTestEnable(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    return gl->alphaTestEnable;
}

static void glGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    GLRenderer* gl = (GLRenderer*) renderer;
    float refF = ref / 255.0f;
    if (gl->alphaTestRef == refF) return;
    flushBatch(gl);
    gl->alphaTestRef = refF;
    glShaderSettingsRefresh(renderer);
}

static void glGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->colorWriteR == red && gl->colorWriteG == green && gl->colorWriteB == blue && gl->colorWriteA == alpha) return;
    flushBatch(gl);
    gl->colorWriteR = red;
    gl->colorWriteG = green;
    gl->colorWriteB = blue;
    gl->colorWriteA = alpha;
    glColorMask(red, green, blue, alpha);
}

static void glGpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    *red = gl->colorWriteR;
    *green = gl->colorWriteG;
    *blue = gl->colorWriteB;
    *alpha = gl->colorWriteA;
}

static void glGpuSetFog(Renderer* renderer, bool enable, uint32_t color, float start, float end) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    if (modernGl->fogEnable == enable && modernGl->fogColor == color && modernGl->fogStart == start && modernGl->fogEnd == end) return;
    flushBatch(gl);
    modernGl->fogEnable = enable;
    modernGl->fogColor = color;
    modernGl->fogStart = start;
    modernGl->fogEnd = end;
    glShaderSettingsRefresh(renderer);
}

static int32_t glShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
#ifdef PLATFORM_VITA
    // the gml edit "direct final" asks whether the frame may skip Pass1 / Pass2 (see "Direct final output")
    if (uniform != nullptr && strcmp(uniform, VITA_DIRECT_PROBE_NAME) == 0)
        return vitaDirectFinalOk(modernGl) ? VITA_DIRECT_PROBE_HANDLE : -1;
    // the gml edit "opaque border" asks whether to draw the (opaque, bilinear-filtered) border art with blending off:
    // not a GL uniform, the setters below ignore this handle ("Opaque draws (Vita)")
    if (uniform != nullptr && strcmp(uniform, VITA_OPAQUE_PROBE_NAME) == 0)
        return vitaOpaque() == 1 ? VITA_OPAQUE_PROBE_HANDLE : -1;
#endif

    int32_t targetShader = (shaderIndex != -1) ? shaderIndex : renderer->currentShader;
    if (targetShader == -1) return -1;

    GMLShader* shader = &modernGl->gmlShaders[targetShader];
    if (!shader->compiled || shader->shaderId == 0) return -1;

    GLint loc = glGetUniformLocation(shader->shaderId, uniform);
    if (loc != -1) return loc;

    char arrayName[256];
    snprintf(arrayName, sizeof(arrayName), "%s[0]", uniform);
    loc = glGetUniformLocation(shader->shaderId, arrayName);
#ifdef PLATFORM_VITA
    if (loc == -1 && shader->vitaBake != nullptr) loc = vitaCrtBakeUniformHandle(shader->vitaBake, uniform); // CRT bake
#endif
    return loc;
}

static GLenum glShaderGetUniformTypeByLocation(GMLShader* shader, int32_t location) {
    if (!shader || location == -1) return GL_NONE;
    for (uint32_t i = 0; i < shader->uniformCount; i++) {
        if (shader->uniforms[i].location == location) {
            return shader->uniforms[i].type;
        }
    }
    return GL_NONE;
}

// The GL call shader_set_uniform_f makes for a uniform of this type (GL_NONE: by value count).
static void glUniformFOfType(GLenum type, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) {
    switch (type) {
        case GL_FLOAT:      glUniform1f(handle, value1); break;
        case GL_FLOAT_VEC2: glUniform2f(handle, value1, value2); break;
        case GL_FLOAT_VEC3: glUniform3f(handle, value1, value2, value3); break;
        case GL_FLOAT_VEC4: glUniform4f(handle, value1, value2, value3, value4); break;
        case GL_INT:        glUniform1i(handle, (GLint)value1); break;
        case GL_INT_VEC2:   glUniform2i(handle, (GLint)value1, (GLint)value2); break;
        case GL_INT_VEC3:   glUniform3i(handle, (GLint)value1, (GLint)value2, (GLint)value3); break;
        case GL_INT_VEC4:   glUniform4i(handle, (GLint)value1, (GLint)value2, (GLint)value3, (GLint)value4); break;
        default:
            if (count == 1)      glUniform1f(handle, value1);
            else if (count == 2) glUniform2f(handle, value1, value2);
            else if (count == 3) glUniform3f(handle, value1, value2, value3);
            else if (count >= 4) glUniform4f(handle, value1, value2, value3, value4);
            break;
    }
}

static void glShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    if (handle == -1 || renderer->currentShader == -1) return;
#ifdef PLATFORM_VITA
    if (handle == VITA_OPAQUE_PROBE_HANDLE || handle == VITA_DIRECT_PROBE_HANDLE) return;
#endif

    GMLShader* shader = &modernGl->gmlShaders[renderer->currentShader];
#ifdef PLATFORM_VITA
    // CRT bake: the bake / plasma programs' copy of the value; a stand-in handle has no GL uniform behind it
    if (shader->vitaBake != nullptr) {
        float values[4] = { value1, value2, value3, value4 };
        if (vitaCrtBakeRecord(shader->vitaBake, handle, count > 0 ? (uint32_t) count : 0, values)) return;
    }
    if (vitaCrtBakeIsStandIn(handle)) return;
#endif
    GLenum type = glShaderGetUniformTypeByLocation(shader, handle);

    glUniformFOfType(type, handle, count, value1, value2, value3, value4);
#ifdef PLATFORM_VITA
    int32_t palSwapUniform = vitaPalSwapUniform(modernGl, handle); // "Palette swapper loop variants": same write
    if (palSwapUniform >= 0) {
        float values[4] = { value1, value2, value3, value4 };
        vitaPalSwapRecord(handle, values, 4);
        repeat(VITA_PALSWAP_VARIANTS, v) if (vitaPalSwap.ok[v])
            glUniformFOfType(type, vitaPalSwap.map[v][palSwapUniform], count, value1, value2, value3, value4);
    }
#endif
}

// The GL call shader_set_uniform_f_array makes for a uniform of this type (values padded to whole vectors).
static void glUniformFArrayOfType(GLenum type, int32_t handle, const float* values, uint32_t count) {
    switch (type) {
        case GL_FLOAT:      glUniform1fv(handle, count, values); break;
        case GL_FLOAT_VEC2: glUniform2fv(handle, count / 2, values); break;
        case GL_FLOAT_VEC3: glUniform3fv(handle, count / 3, values); break;
        case GL_FLOAT_VEC4: glUniform4fv(handle, count / 4, values); break;
        case GL_FLOAT_MAT2: glUniformMatrix2fv(handle, count / 4, GL_FALSE, values); break;
        case GL_FLOAT_MAT3: glUniformMatrix3fv(handle, count / 9, GL_FALSE, values); break;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(handle, count / 16, GL_FALSE, values); break;
        default:            glUniform1fv(handle, count, values); break;
    }
}

static void glShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    if (handle == -1 || renderer->currentShader == -1 || values == NULL || count == 0) return;
#ifdef PLATFORM_VITA
    if (handle == VITA_OPAQUE_PROBE_HANDLE || handle == VITA_DIRECT_PROBE_HANDLE) return;
#endif

    GMLShader* shader = &modernGl->gmlShaders[renderer->currentShader];
#ifdef PLATFORM_VITA
    if (shader->vitaBake != nullptr && vitaCrtBakeRecord(shader->vitaBake, handle, count, values)) return; // CRT bake
    if (vitaCrtBakeIsStandIn(handle)) return;
#endif
    GLenum type = glShaderGetUniformTypeByLocation(shader, handle);

    uint32_t vectorWidth = 1;
    switch (type) {
        case GL_FLOAT_VEC2: vectorWidth = 2; break;
        case GL_FLOAT_VEC3: vectorWidth = 3; break;
        case GL_FLOAT_VEC4: vectorWidth = 4; break;
        default: break;
    }
    float* paddedValues = nullptr;
    if (count % vectorWidth != 0) {
        uint32_t paddedCount = count + vectorWidth - count % vectorWidth;
        paddedValues = (float*) safeCalloc(paddedCount, sizeof(float));
        memcpy(paddedValues, values, count * sizeof(float));
        values = paddedValues;
        count = paddedCount;
    }

    glUniformFArrayOfType(type, handle, values, count);
#ifdef PLATFORM_VITA
    int32_t palSwapUniform = vitaPalSwapUniform(modernGl, handle); // "Palette swapper loop variants": same write
    if (palSwapUniform >= 0) {
        vitaPalSwapRecord(handle, values, count);
        repeat(VITA_PALSWAP_VARIANTS, v) if (vitaPalSwap.ok[v])
            glUniformFArrayOfType(type, vitaPalSwap.map[v][palSwapUniform], values, count);
    }
#endif
    free(paddedValues);
}

// The GL call shader_set_uniform_i makes for a uniform of this type (GL_NONE: by value count).
static void glUniformIOfType(GLenum type, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) {
    switch (type) {
        case GL_INT: glUniform1i(handle, value1); break;
        case GL_INT_VEC2: glUniform2i(handle, value1, value2); break;
        case GL_INT_VEC3: glUniform3i(handle, value1, value2, value3); break;
        case GL_INT_VEC4: glUniform4i(handle, value1, value2, value3, value4); break;
        default:
            if (count == 1)      glUniform1i(handle, value1);
            else if (count == 2) glUniform2i(handle, value1, value2);
            else if (count == 3) glUniform3i(handle, value1, value2, value3);
            else if (count >= 4) glUniform4i(handle, value1, value2, value3, value4);
            break;
    }
}

static void glShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;

    if (handle == -1 || renderer->currentShader == -1) return;
#ifdef PLATFORM_VITA
    if (handle == VITA_OPAQUE_PROBE_HANDLE || handle == VITA_DIRECT_PROBE_HANDLE) return;
    if (vitaCrtBakeIsStandIn(handle)) return; // CRT bake stand-in: only float uniforms are recorded
#endif

    GMLShader* shader = &modernGl->gmlShaders[renderer->currentShader];
    GLenum type = glShaderGetUniformTypeByLocation(shader, handle);

    glUniformIOfType(type, handle, count, value1, value2, value3, value4);
#ifdef PLATFORM_VITA
    int32_t palSwapUniform = vitaPalSwapUniform(modernGl, handle); // "Palette swapper loop variants": same write
    if (palSwapUniform >= 0) {
        // an integer write: the loop-length test doesn't model what the program then sees, so the 64-step one is used
        if (handle == vitaPalSwap.locUvs) vitaPalSwap.haveUvs = false;
        if (handle == vitaPalSwap.locPixelSize) vitaPalSwap.havePixelSize = false;
        repeat(VITA_PALSWAP_VARIANTS, v) if (vitaPalSwap.ok[v])
            glUniformIOfType(type, vitaPalSwap.map[v][palSwapUniform], count, value1, value2, value3, value4);
    }
#endif
}

static int32_t glShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
    GMLShader* shader = &modernGl->gmlShaders[shaderIndex];

    repeat(shader->uniformCount, b) {
        if (strcmp(shader->uniforms[b].name, uniform) == 0) {
            return shader->uniforms[b].samplerSlot;
        }
    }
#ifdef PLATFORM_VITA
    if (shader->vitaBake != nullptr && vitaCrtBakeSamplerSlot(shader->vitaBake, uniform) >= 0)
        return vitaCrtBakeSamplerSlot(shader->vitaBake, uniform); // CRT bake: a sampler only the bake program reads
#endif

    fprintf(stderr, "GL: Sampler Index %s not found for shader %d!\n", uniform, shaderIndex);
    return -1;
}

static uint32_t glSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH)) return 0;
    return (uint32_t) (tpagIndex + 1);
}

// Decode a texture handle produced by glSpriteGetTexture (sprite/tpag) or glSurfaceGetTexture (surface)
// back into its GL id and pixel size. *outTpag is set to NULL for surface handles (no tpag sub-region).
// Returns false for the 0 ("no texture") handle or an unresolvable one.
static bool glResolveTextureHandle(GLRenderer* gl, uint32_t texHandle, TexturePageItem** outTpag, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH) {
    if (texHandle == 0) return false;
    if (texHandle & GL_SURFACE_TEXTURE_FLAG) {
        uint32_t sid = texHandle & ~GL_SURFACE_TEXTURE_FLAG;
        if (sid >= gl->surfaceCount || gl->surfaceTexture[sid] == 0) return false;
        if (outTpag) *outTpag = nullptr;
        *outTexId = gl->surfaceTexture[sid];
        *outTexW = gl->surfaceWidth[sid];
        *outTexH = gl->surfaceHeight[sid];
        return true;
    }
    return resolveSpriteTexture(gl, (int32_t) texHandle - 1, outTpag, outTexId, outTexW, outTexH);
}

// surface_get_texture: returns a handle that texture_get_texel_*/texture_get_uvs/texture_set_stage resolve.
static uint32_t glSurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (surfaceID < 0 || (uint32_t) surfaceID >= gl->surfaceCount) return 0;
    if (gl->surfaceTexture[surfaceID] == 0) return 0;
    return GL_SURFACE_TEXTURE_FLAG | (uint32_t) surfaceID;
}

static void glTextureSetStage(Renderer* renderer, int32_t slot, uint32_t texHandle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;

    if (slot < 0) {
        logWarn("GL: Invalid Texture Stage\n");
        return;
    }
    TexturePageItem* tpag;
    GLuint texID = 0;
    int32_t texW, texH;
    glResolveTextureHandle(gl, texHandle, &tpag, &texID, &texW, &texH);
    if (slot == 0) {
        modernGl->currentTextureId = texID;
    }
    if (slot > MAX_TEXTURE_STAGES) {
        logWarn("GL: Texture Stage Higher Than Max\n");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, texID);
    glActiveTexture(GL_TEXTURE1);
    modernGl->stageTexture[slot] = texID;

}

// Look up a texture's pixel size from the renderer's own tables.
MAYBE_UNUSED static bool lookupTextureSize(GLRenderer* gl, uint32_t texID, int32_t* outW, int32_t* outH) {
    repeat(gl->textureCount, i) {
        if (gl->textureLoaded[i] && gl->glTextures[i] == (GLuint) texID) {
            *outW = gl->textureWidths[i];
            *outH = gl->textureHeights[i];
            return true;
        }
    }

    repeat(gl->textureCount, i) {
        if (gl->surfaceTexture[i] == (GLuint) texID) {
            *outW = gl->surfaceWidth[i];
            *outH = gl->surfaceHeight[i];
            return true;
        }
    }

    return false;
}

static float glTextureGetTexelWidth(Renderer* renderer, uint32_t texHandle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t width = 0, height = 0;
    if (!glResolveTextureHandle(gl, texHandle, &tpag, &texId, &width, &height) || 0 >= width) return 1.0f;
    return 1.0f / (float) width;
}

static float glTextureGetTexelHeight(Renderer* renderer, uint32_t texHandle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t width = 0, height = 0;
    if (!glResolveTextureHandle(gl, texHandle, &tpag, &texId, &width, &height) || 0 >= height) return 1.0f;
    return 1.0f / (float) height;
}

static bool glTextureGetUVs(Renderer* renderer, uint32_t texHandle, float* outUVs) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t width = 0, height = 0;
    if (!glResolveTextureHandle(gl, texHandle, &tpag, &texId, &width, &height) || 0 >= width || 0 >= height) return false;
    // Surface handles cover the whole texture (no tpag sub-region).
    if (tpag == nullptr) {
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    float divW = 1.0f / (float) width;
    float divH = 1.0f / (float) height;
    outUVs[0] = (float) tpag->sourceX * divW;                              // left
    outUVs[1] = (float) tpag->sourceY * divH;                             // top
    outUVs[2] = outUVs[0] + (float) tpag->sourceWidth * divW;            // right
    outUVs[3] = outUVs[1] + (float) tpag->sourceHeight * divH;           // bottom
    return true;
}

static bool glShaderIsCompiled(Renderer* renderer, int32_t shaderID) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GLModernRenderer* modernGl = (GLModernRenderer*) renderer;
    
    DataWin* dw = gl->base.dataWin;
    if (0 > shaderID || (uint32_t) shaderID >= dw->shdr.count) return false;
    return modernGl->gmlShaders[shaderID].compiled;
}

static bool glShadersSupported(void) {
    return true;
}

static void glSetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    if (memcmp(&renderer->gmlMatrices[matrixType], &matrix, sizeof(Matrix4f)) == 0) return;
    flushBatch((GLRenderer*)renderer);

    renderer->gmlMatrices[matrixType] = matrix;
    //yeah just recalculate everything when we change a matrix
    //TODO LATR: only allow these 3 to be changed directly, other ones should only be allowed to be calculated by the rest of the function
    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = renderer->gmlMatrices[MATRIX_VIEW];
    Matrix4f projection = renderer->gmlMatrices[MATRIX_PROJECTION];

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);

    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;


    glShaderSettingsRefresh(renderer);
}

#ifdef PLATFORM_VITA
// ===[ GPU profiler (Vita) ]===
// Where a frame's GPU time goes, on hardware. On when <data dir>/debuglog or <data dir>/gpuprof exists, off with
// <data dir>/nogpuprof. Every GPUPROF_PERIOD presented frames the loop drains the GPU after a swap (sceGxmFinish)
// and samples the next frame: at its first draw or clear on a different render target, vitaGL's own scene_reset()
// (what that draw is about to call) ends the previous target's GXM scene and begins this one's, then
// sceGxmFinish waits for the GPU, so every segment is one scene:
//   c = CPU time from the segment's start to its sync (the draws were issued while the GPU was idle),
//   g = time inside the sync: the GPU running that scene (vertices, tiling, fragments, tile loads/stores) plus the
//       completion wake-up (GPUCAL sync/empty below).
// The first segment, "start", opens when the frame is armed (after the drain): its c is the CPU time up to the
// frame's first counted draw (the game step, ...) and its g the scene that unwrapped GL work began before that
// draw: loop.c's frame-start screen clear, a 960x544 scene of its own (g near GPUCAL sync when there is none,
// e.g. with that clear deferred into the final screen scene).
// The GPU runs the same scenes, in the same order, with the same contents as in any other frame: scene_reset only
// switches scenes when the bound target differs from vitaGL's in-use one, so it never splits a scene, even when GL
// work outside these macros (a raw glClear, a vgl* draw) already began the new target's scene (glFinish would:
// it forces a reset). Such unwrapped work is not counted (d/C), its CPU time falls in the previous segment's c,
// and work of that kind on a third target between two counted ones is timed in the earlier segment's g.
// What goes is the overlap (normally the CPU issues pass N+1 while the GPU runs pass N, and GXM can tile scene
// N+1 while it shades scene N), so sum(g) is the frame's GPU work done back to back, each scene on its own; what
// a drained frame cannot show is cross-frame contention (frames in flight sharing the parameter buffer, render
// targets made with 1 scene per frame, the display queue): if sum(g) stays well below the PERF frame time of a
// GPU-bound room, the rest is that waiting, not shading. total = sum(c) + sum(g) is longer than an unsampled
// frame: one frame in GPUPROF_PERIOD hitches (with the drain before it, the log lines and GPUCAL after it,
// ~20-50 ms once; the PERF line after a GPUPROF line contains it and reads ~0.2-0.4 ms high, and its draw=/swap=
// split moves, since waits that are normally in the swap happen in the draw: gpuprof_summary.py prints PERF with
// and without those lines; nogpuprof for clean PERF runs). The last segment ends at the swap: vglSwapBuffers ends
// the screen scene and queues the flip, then sceGxmFinish; its g counts from the start of vglSwapBuffers (swap= is
// the CPU time inside it; if swap= is close to that g, the display queue was full and g is an upper bound).
// After the sampled frame, with the GPU idle, GPUCAL times scenes on a private 512x448 target: sync =
// sceGxmFinish with nothing pending, empty = a scene with no draws, tiny = one 1x1 quad, clear = one glClear,
// copy = one 512x448 textured quad (a plain surface copy), copy2 = two of them in one scene (each includes the CPU
// time to issue it, ~0.1 ms). clear ~ the fixed cost of a one-quad pass (scene begin/end, tile loads/stores, sync;
// if empty comes out near sync, GXM skipped the tile work of a scene without draws); copy2 - copy ~ the shading of
// one full quad.
#include <stdarg.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
extern SceGxmContext* gxm_context; // vitaGL's GXM context (a global of libvitaGL.a)
void scene_reset(void);            // vitaGL (shared.h): ends/begins the GXM scene if the bound target changed
extern bool bsPerfLog;             // loop.c: <data dir>/debuglog exists

#define GPUPROF_PERIOD 1800 // one sampled frame every ~30 s: each costs a visible 20-50 ms hitch
#define GPUPROF_MAX_SEGS 48
#define GPUPROF_MAX_PROGS 3
#define GPUPROF_CAL_W 512
#define GPUPROF_CAL_H 448

enum { GPUPROF_DEFAULT = -1, GPUPROF_FASTDEFAULT = -2, GPUPROF_PALLUT = -3, GPUPROF_UNKNOWN = -4, GPUPROF_PALCOLOR = -5,
       GPUPROF_PALSWAP4 = -6 }; // -6 - v: palette swapper variant v

typedef struct {
    GLint fbo;
    int32_t surface;                    // GML surface id; -1 = screen, -2 = a framebuffer that isn't a surface,
                                        // -3 = "start" (fbo -1, opened when the sampled frame is armed)
    int32_t w, h;
    uint32_t draws, clears, depthClears, quads, verts;
    int32_t progs[GPUPROF_MAX_PROGS];   // GML shader index or GPUPROF_*
    uint8_t progN;
    bool progMore, merged;
    uint64_t tOpen, tSync, tDone;       // us
} GpuProfSeg;

bool bsGpuProfOn;
static GLModernRenderer* gpuProfR;
static int gpuProfState;                // 0 = not checked yet, 1 = on, -1 = off
static uint32_t gpuProfCountdown = GPUPROF_PERIOD;
static GpuProfSeg gpuProfSegs[GPUPROF_MAX_SEGS];
static int gpuProfSegN;
static uint64_t gpuProfArmT, gpuProfSwapT;
static int32_t gpuProfArmFrame;
static GLuint gpuProfCalFbo, gpuProfCalTex;

static inline uint64_t gpuProfNow(void) { return sceKernelGetProcessTimeWide(); }

static int32_t gpuProfProgCode(GLuint prog) {
    GLModernRenderer* m = gpuProfR;
    if (m->fastDefaultProgram != nullptr && prog == m->fastDefaultProgram->shaderId) return GPUPROF_FASTDEFAULT;
    if (m->defaultShaderProgram != nullptr && prog == m->defaultShaderProgram->shaderId) return GPUPROF_DEFAULT;
    if (m->palettizerIndex >= 0 && prog == m->fastPalettizer.shaderId) return GPUPROF_PALLUT;
    if (m->colorPalettizerOk && prog == m->colorPalettizer.shaderId) return GPUPROF_PALCOLOR;
    repeat(VITA_PALSWAP_VARIANTS, v) if (vitaPalSwap.ok[v] && prog == vitaPalSwap.variant[v].shaderId) return GPUPROF_PALSWAP4 - (int32_t) v;
    repeat(m->gmlShaderCount, i) if (m->gmlShaders[i].compiled && m->gmlShaders[i].shaderId == prog) return (int32_t) i;
    return GPUPROF_UNKNOWN;
}

// A draw (kind 0: glDrawElements, count = indices; 1: glDrawArrays, count = vertices) or a clear (2, count = mask)
// is about to be issued on the bound framebuffer.
void bsGpuProfWork(int kind, uint32_t count) {
    if (gpuProfR == nullptr) return;
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    GpuProfSeg* s = gpuProfSegN > 0 ? &gpuProfSegs[gpuProfSegN - 1] : nullptr;
    if (s == nullptr || s->fbo != fb) {
        if (gpuProfSegN == GPUPROF_MAX_SEGS) {
            s->merged = true; // out of segments: the rest of the frame is counted in the last one, without syncs
        } else {
            uint64_t t = gpuProfNow();
            if (s != nullptr) {
                s->tSync = t;
                scene_reset();              // ends the previous scene and begins this target's, as the draw would
                sceGxmFinish(gxm_context);  // waits for the GPU (the new scene has nothing in it yet)
                t = gpuProfNow();
                s->tDone = t;
            }
            s = &gpuProfSegs[gpuProfSegN++];
            memset(s, 0, sizeof(*s));
            s->fbo = fb;
            s->surface = -2;
            s->tOpen = t;
            GLRenderer* gl = &gpuProfR->base;
            if ((GLuint) fb == gpuProfR->hostFramebuffer) {
                s->surface = -1; s->w = gl->windowW; s->h = gl->windowH;
            } else {
                repeat(gl->surfaceCount, i) if (gl->surfaces[i] == (GLuint) fb) {
                    s->surface = (int32_t) i; s->w = gl->surfaceWidth[i]; s->h = gl->surfaceHeight[i];
                    break;
                }
            }
        }
    }
    if (kind == 2) {
        if (count & GL_COLOR_BUFFER_BIT) s->clears++; else s->depthClears++;
        return;
    }
    s->draws++;
    if (kind == 0) s->quads += count / 6; else s->verts += count;
    int32_t code = gpuProfProgCode(gpuProfProgram);
    int k = 0;
    while (k < s->progN && s->progs[k] != code) k++;
    if (k == s->progN) {
        if (s->progN < GPUPROF_MAX_PROGS) s->progs[s->progN++] = code; else s->progMore = true;
    }
}

// snprintf at out + *n, never past size (*n stays < size)
static void gpuProfCat(char* out, size_t size, size_t* n, const char* fmt, ...) {
    if (*n + 1 >= size) return;
    va_list va;
    va_start(va, fmt);
    int k = vsnprintf(out + *n, size - *n, fmt, va);
    va_end(va);
    if (k > 0) *n = (*n + (size_t) k < size) ? *n + (size_t) k : size - 1;
}

static void gpuProfSegText(char* out, size_t size, const GpuProfSeg* s) {
    GLModernRenderer* m = gpuProfR;
    DataWin* dw = m->base.base.dataWin;
    size_t n = 0;
    out[0] = '\0';
    if (s->surface == -3) {
        gpuProfCat(out, size, &n, "start: g%.2f c%.2f", (double) (s->tDone - s->tSync) / 1000.0,
                   (double) (s->tSync - s->tOpen) / 1000.0);
        return;
    }
    if (s->surface == -1) gpuProfCat(out, size, &n, "screen %dx%d", (int) s->w, (int) s->h);
    else if (s->surface >= 0) gpuProfCat(out, size, &n, "s%d%s %dx%d", (int) s->surface,
                                         s->surface == m->base.base.runner->applicationSurfaceId ? "(app)" : "", (int) s->w, (int) s->h);
    else gpuProfCat(out, size, &n, "fb%08x", (unsigned) s->fbo);
    if (s->clears) gpuProfCat(out, size, &n, " C%u", (unsigned) s->clears);
    if (s->depthClears) gpuProfCat(out, size, &n, " Z%u", (unsigned) s->depthClears);
    repeat((int) s->progN, k) {
        int32_t c = s->progs[k];
        static const char* const palSwapNames[VITA_PALSWAP_VARIANTS] = { "shd_pal_swapper/4", "shd_pal_swapper/8", "shd_pal_swapper/16" };
        const char* name = c == GPUPROF_FASTDEFAULT ? "fastdefault" : c == GPUPROF_DEFAULT ? "default" :
                           c == GPUPROF_PALLUT ? "palettizer(lut)" : c == GPUPROF_PALCOLOR ? "palettizer(colour)" :
                           c <= GPUPROF_PALSWAP4 && c > GPUPROF_PALSWAP4 - VITA_PALSWAP_VARIANTS ? palSwapNames[GPUPROF_PALSWAP4 - c] :
                           c == GPUPROF_UNKNOWN ? "?" :
                           ((uint32_t) c < dw->shdr.count && dw->shdr.shaders[c].name != nullptr) ? dw->shdr.shaders[c].name : "gml?";
        gpuProfCat(out, size, &n, "%s%s%s", k == 0 ? " " : "+", name, c >= 0 && m->gmlShaders[c].vitaGrid ? "#" : "");
    }
    if (s->progMore) gpuProfCat(out, size, &n, "+...");
    gpuProfCat(out, size, &n, " d%u", (unsigned) s->draws);
    if (s->quads) gpuProfCat(out, size, &n, " q%u", (unsigned) s->quads);
    if (s->verts) gpuProfCat(out, size, &n, " v%u", (unsigned) s->verts);
    if (s->merged) gpuProfCat(out, size, &n, " +more targets");
    gpuProfCat(out, size, &n, ": g%.2f c%.2f", (double) (s->tDone - s->tSync) / 1000.0, (double) (s->tSync - s->tOpen) / 1000.0);
}

static void gpuProfReport(int32_t frame, uint64_t swapUs, uint64_t tEnd) {
    uint64_t gpu = 0, cpu = 0;
    repeat(gpuProfSegN, i) { gpu += gpuProfSegs[i].tDone - gpuProfSegs[i].tSync; cpu += gpuProfSegs[i].tSync - gpuProfSegs[i].tOpen; }
    char line[1000], seg[256]; // platformLog formats up to 1024 bytes a line
    size_t n = 0;
    gpuProfCat(line, sizeof(line), &n, "GPUPROF frame=%d total=%.2f gpu=%.2f cpu=%.2f swap=%.2f segs=%d",
               (int) frame, (double) (tEnd - gpuProfArmT) / 1000.0, (double) gpu / 1000.0, (double) cpu / 1000.0,
               (double) swapUs / 1000.0, gpuProfSegN);
    if (frame - gpuProfArmFrame != 1) gpuProfCat(line, sizeof(line), &n, " (%d frames)", (int) (frame - gpuProfArmFrame));
    repeat(gpuProfSegN, i) {
        gpuProfSegText(seg, sizeof(seg), &gpuProfSegs[i]);
        if (n + strlen(seg) + 3 >= sizeof(line) - 1) {
            logInfo("%s\n", line);
            n = 0;
            gpuProfCat(line, sizeof(line), &n, "GPUPROF+ frame=%d", (int) frame);
        }
        gpuProfCat(line, sizeof(line), &n, " | %s", seg);
    }
    logInfo("%s\n", line);
}

// Fixed per-scene costs, timed on a private 512x448 target with the GPU idle (see above).
static void gpuProfCalibrate(void) {
    GLModernRenderer* m = gpuProfR;
    GLRenderer* gl = &m->base;
    Renderer* renderer = &gl->base;
    const int W = GPUPROF_CAL_W, H = GPUPROF_CAL_H;
    if (renderer->currentShader != -1 || m->batchCount != 0 || m->depthTest) { logInfo("GPUCAL skipped\n"); return; }

    GLint prevFbo = 0, prevViewport[4], prevUnit = GL_TEXTURE0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    Matrix4f savedMatrices[MATRICES_MAX];
    memcpy(savedMatrices, renderer->gmlMatrices, sizeof(savedMatrices));
    int32_t savedCamera = renderer->cameraCurrent;
    GMLCamera savedGuiCamera = renderer->runner->guiCamera;

    if (gpuProfCalFbo == 0) {
        glActiveTexture(GL_TEXTURE1); // the unit flushBatch rebinds for every default-program draw
        glGenTextures(1, &gpuProfCalTex);
        glBindTexture(GL_TEXTURE_2D, gpuProfCalTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &gpuProfCalFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, gpuProfCalFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpuProfCalTex, 0);
    }
    // copy source: a surface of the same size (the CRT passes'), else the application surface stretched
    int32_t src = -1;
    repeat(gl->surfaceCount, i) if (gl->surfaceTexture[i] != 0 && gl->surfaceWidth[i] == W && gl->surfaceHeight[i] == H) { src = (int32_t) i; break; }
    if (src < 0) {
        int32_t app = renderer->runner->applicationSurfaceId;
        if (app >= 0 && (uint32_t) app < gl->surfaceCount && gl->surfaceTexture[app] != 0) src = app;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, gpuProfCalFbo);
    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    renderer->vtable->setGuiProjection(renderer, W, H, W, H, true);

    uint64_t t, us[6] = {0};
    t = gpuProfNow(); sceGxmFinish(gxm_context); us[0] = gpuProfNow() - t;
    glFinish(); // begins a scene on the calibration target; nothing to wait for
    t = gpuProfNow(); glFinish(); us[1] = gpuProfNow() - t; // ends that empty scene
    t = gpuProfNow();
    emitColoredRectangle(gl, 0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 1.0f);
    flushBatch(gl);
    glFinish();
    us[2] = gpuProfNow() - t;
    t = gpuProfNow(); glClear(GL_COLOR_BUFFER_BIT); glFinish(); us[3] = gpuProfNow() - t;
    if (src >= 0) {
        GLuint tex = gl->surfaceTexture[src];
        repeat(2, pass) {
            t = gpuProfNow();
            repeat(pass + 1, q) {
                emitTexturedQuad(gl, tex, 0.0f, 0.0f, (float) W, 0.0f, (float) W, (float) H, 0.0f, (float) H, 0.0f, 0.0f, 1.0f, 1.0f,
                                 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 1.0f);
            }
            flushBatch(gl);
            glFinish();
            us[4 + pass] = gpuProfNow() - t;
        }
    }
    bool fast = m->fastDefaultProgram != nullptr && !gl->alphaTestEnable && !m->fogEnable;
    bool blend = glIsEnabled(GL_BLEND); // left as the game set it (its passes draw with blending on)

    // The calibration target keeps an empty scene open; the next frame's first draw ends it.
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glActiveTexture((GLenum) prevUnit);
    memcpy(renderer->gmlMatrices, savedMatrices, sizeof(savedMatrices));
    renderer->cameraCurrent = savedCamera;
    renderer->runner->guiCamera = savedGuiCamera;
    glShaderSettingsRefresh(renderer);

    logInfo("GPUCAL %dx%d ms: sync=%.3f empty=%.3f tiny=%.3f clear=%.3f copy=%.3f copy2=%.3f (copy from s%d %dx%d, %s, blend %s)\n",
            W, H, us[0] / 1000.0, us[1] / 1000.0, us[2] / 1000.0, us[3] / 1000.0, us[4] / 1000.0, us[5] / 1000.0,
            (int) src, src >= 0 ? (int) gl->surfaceWidth[src] : 0, src >= 0 ? (int) gl->surfaceHeight[src] : 0,
            fast ? "fastdefault" : "default", blend ? "on" : "off");
}

// Called by the loop around platformSwapBuffers (after = false just before it, true just after it).
void bsGpuProfSwap(bool after, int32_t frame) {
    if (gpuProfR == nullptr) return;
    if (!after) {
        if (bsGpuProfOn) gpuProfSwapT = gpuProfNow();
        return;
    }
    if (bsGpuProfOn) {
        uint64_t swapUs = gpuProfNow() - gpuProfSwapT;
        sceGxmFinish(gxm_context); // the screen scene vglSwapBuffers ended
        uint64_t tEnd = gpuProfNow();
        bsGpuProfOn = false;
        if (gpuProfSegN > 0) {
            gpuProfSegs[gpuProfSegN - 1].tSync = gpuProfSwapT;
            gpuProfSegs[gpuProfSegN - 1].tDone = tEnd;
        }
        gpuProfReport(frame, swapUs, tEnd);
        gpuProfCalibrate();
        gpuProfCountdown = GPUPROF_PERIOD;
        return;
    }
    if (gpuProfState == 0) {
        gpuProfState = (bsPerfLog || vitaMarkerFile(BS_VITA_PATH("gpuprof"))) && !vitaMarkerFile(BS_VITA_PATH("nogpuprof")) ? 1 : -1;
        if (gpuProfState > 0) logInfo("GL: GPU profiler on: 1 frame in %d sampled (GPUPROF/GPUCAL lines; nogpuprof turns it off)\n", GPUPROF_PERIOD);
    }
    if (gpuProfState < 0 || --gpuProfCountdown != 0) return;
    sceGxmFinish(gxm_context); // drain the frames in flight: the sampled frame starts with the GPU idle
    gpuProfArmFrame = frame;
    gpuProfArmT = gpuProfNow();
    // "start": whatever GL work outside the wrapped calls begins a scene before the frame's first counted draw or
    // clear (loop.c's frame-start screen clear) is ended and timed by that first draw's sync
    memset(&gpuProfSegs[0], 0, sizeof(gpuProfSegs[0]));
    gpuProfSegs[0].fbo = -1;
    gpuProfSegs[0].surface = -3;
    gpuProfSegs[0].tOpen = gpuProfArmT;
    gpuProfSegN = 1;
    bsGpuProfOn = true;
}
#endif

// ===[ Vtable ]===

static RendererVtable glVtable;

// ===[ Public API ]===

#ifdef PLATFORM_VITA
void GLRenderer_vitaFinishPrefetch(GLRenderer* gl) {
    for (int32_t i = 0; i < vitaPrefetchCount; i++) {
        VitaPrefetch* p = &vitaPrefetch[i];
        if (!p->pending) continue;
        GLint prevUnit = 0, prevTex = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        if (!vitaPrefetchJoin(gl, p))
            logWarn("GL: TXTR page %u: worker decode failed, decoding it at first use\n", (unsigned) p->pageId);
        glActiveTexture((GLenum) prevUnit);
        glBindTexture(GL_TEXTURE_2D, (GLuint) prevTex);
    }
}

// The boot indicator swapped frames since glInit took the batch buffer from vitaGL's per-frame scratch pool: take a
// fresh one, so the first frame's batches can't land on memory the pool has since handed out again.
void GLRenderer_vitaAfterBootIndicator(GLRenderer* gl) {
    GLModernRenderer* modernGl = (GLModernRenderer*) gl;
    if (modernGl->batchCount == 0)
        gl->vertexData = (GlVertex*) vglAllocFromScratch(MAX_QUADS * VERTICES_PER_QUAD * sizeof(GlVertex));
}
#endif

Renderer* GLRenderer_create(void) {
    GLModernRenderer* modernGl = (GLModernRenderer *)safeCalloc(1, sizeof(GLModernRenderer));
    GLRenderer* gl = &modernGl->base;
    gl->glMode = GL_MODE_MODERN;
    
    Renderer* base = &gl->base;
    base->vtable = &glVtable;
#ifdef PLATFORM_VITA
    gpuProfR = modernGl;
#endif
    
    glVtable.init = glInit;
    glVtable.destroy = glDestroy;
    glVtable.beginFrame = glBeginFrame;
    glVtable.endFrameInit = glEndFrameInit;
    glVtable.endFrameEnd = glEndFrameEnd;
    glVtable.beginView = glBeginView;
    glVtable.endView = glEndView;
    glVtable.applyProjection = glApplyProjection;
    glVtable.beginGUI = glBeginGUI;
    glVtable.setGuiProjection = glSetGuiProjection;
    glVtable.endGUI = glEndGUI;
    glVtable.drawSprite = glDrawSprite;
    glVtable.drawSpritePos = glDrawSpritePos;
    glVtable.drawSpritePart = glDrawSpritePart;
    glVtable.drawSpritePartColor = glDrawSpritePartColor;
    glVtable.drawRectangle = glDrawRectangle;
    glVtable.drawRectangleColor = glDrawRectangleColor;
    glVtable.drawLine = glDrawLine;
    glVtable.drawLineColor = glDrawLineColor;
    glVtable.drawTriangle = glDrawTriangle;
    glVtable.drawVertexBuffer = glDrawVertexBuffer;
    glVtable.drawText = glDrawText;
    glVtable.drawTextColor = glDrawTextColor;
    glVtable.drawTextUI = glDrawTextUI;
    glVtable.primitiveBegin = glPrimitiveBegin;
    glVtable.primitiveBeginTexture = glPrimitiveBeginTexture;
    glVtable.primitiveEnd = glPrimitiveEnd;
    glVtable.drawVertex = glDrawVertex;
    glVtable.flush = glRendererFlush;
    glVtable.clearScreen = glClearScreen;
    glVtable.createSpriteFromSurface = glCreateSpriteFromSurface;
    glVtable.createTpagFromPixels = glCreateTpagFromPixels;
    glVtable.deleteSprite = glDeleteSprite;
    glVtable.gpuGetBlendFactors = glGpuGetBlendFactors;
    glVtable.gpuGetBlendMode = glGpuGetBlendMode;
    glVtable.gpuSetBlendMode = glGpuSetBlendMode;
    glVtable.gpuSetBlendModeExt = glGpuSetBlendModeExt;
    glVtable.gpuSetBlendEnable = glGpuSetBlendEnable;
    glVtable.gpuSetTexFilter = glGpuSetTexFilter;
    glVtable.gpuSetAlphaTestEnable = glGpuSetAlphaTestEnable;
    glVtable.gpuGetAlphaTestEnable = glGpuGetAlphaTestEnable;
    glVtable.gpuSetAlphaTestRef = glGpuSetAlphaTestRef;
    glVtable.gpuSetColorWriteEnable = glGpuSetColorWriteEnable;
    glVtable.gpuGetColorWriteEnable = glGpuGetColorWriteEnable;
    glVtable.gpuSetFog = glGpuSetFog;
    glVtable.gpuGetBlendEnable = glGpuGetBlendEnable;
    glVtable.drawTile = nullptr;
    glVtable.drawSpriteTiled = glDrawSpriteTiled;
    glVtable.createSurface = glCreateSurface;
    glVtable.surfaceExists = glSurfaceExists;
    glVtable.setRenderTarget = glSetRenderTarget;
    glVtable.ensureApplicationSurface = glEnsureApplicationSurface;
    glVtable.surfaceCopy = glSurfaceCopy;
    glVtable.surfaceGetPixels = glSurfaceGetPixels;
#ifdef PLATFORM_VITA
    glVtable.surfaceSetPixels = vitaSurfaceSetPixels;
    glVtable.surfaceUploadPixels = vitaSurfaceUploadPixels;
#else
    glVtable.surfaceSetPixels = GLCommon_surfaceSetPixels;
    glVtable.surfaceUploadPixels = GLCommon_surfaceUploadPixels;
#endif
    glVtable.getSurfaceWidth = glGetSurfaceWidth;
    glVtable.getSurfaceHeight = glGetSurfaceHeight;
    glVtable.drawSurface = glDrawSurface;
    glVtable.drawSurfaceColor = glDrawSurfaceColor;
    glVtable.drawSurfaceTiled = glDrawSurfaceTiled;
    glVtable.surfaceResize = glSurfaceResize;
    glVtable.surfaceFree = glSurfaceFree;
    glVtable.gpuSetShader = glGpuSetShader,
    glVtable.gpuResetShader = glGpuResetShader,
    glVtable.shaderGetUniform = glShaderGetUniform,
    glVtable.shaderSetUniformF = glShaderSetUniformF,
    glVtable.shaderSetUniformFArray = glShaderSetUniformFArray,
    glVtable.shaderSetUniformI = glShaderSetUniformI,
    glVtable.spriteGetTexture = glSpriteGetTexture,
    glVtable.surfaceGetTexture = glSurfaceGetTexture,
    glVtable.textureGetTexelWidth = glTextureGetTexelWidth,
    glVtable.textureGetTexelHeight = glTextureGetTexelHeight,
    glVtable.textureGetUVs = glTextureGetUVs,
    glVtable.shaderGetSamplerIndex = glShaderGetSamplerIndex,
    glVtable.textureSetStage = glTextureSetStage,
    glVtable.shaderIsCompiled = glShaderIsCompiled,
    glVtable.shadersSupported = glShadersSupported,
    glVtable.setMatrix = glSetMatrix,
    glVtable.setDepthState = glSetDepthState;
    glVtable.clearDepth = glClearDepthBuffer;

    base->drawColor = 0xFFFFFF; // white (BGR)
    base->drawAlpha = 1.0f;
    base->drawFont = -1;
    base->drawHalign = 0;
    base->drawValign = 0;
    base->circlePrecision = 24;
    base->currentShader = -1;
    base->cameraCurrent = 0;

    return (Renderer*) modernGl;
}
