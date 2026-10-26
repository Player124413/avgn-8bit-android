// Android video backend: android.media.MediaPlayer decoded into a SurfaceTexture.
//
// MediaPlayer does everything the Vita build relied on SceAvPlayer for: demuxing, hardware decode of the H.264 the
// game ships, the audio track, and the clock that keeps picture and sound together. The decoder renders into the
// BufferQueue behind a SurfaceTexture, which hands every frame to the GL context of the thread that created it as an
// external OES texture. That frame is drawn into the GML video surface (a plain FBO) with a two-triangle pass, so
// the game's draw_surface / sprite_create_from_surface see an ordinary RGBA surface, exactly like on the Vita.
//
// Everything here runs on the render thread: video_open / video_draw / video_close are GML built-ins, so the GL
// calls and updateTexImage() happen where the GL context is current. Audio is not extracted (extractAudio is null):
// MediaPlayer plays it itself, which also means the video needs no master clock from the runner.
//
// The game's cutscenes are Cutscenes/HD/*_1080.mp4 from the Steam release. Android decodes them at full resolution;
// the Vita patcher used to re-encode them to 960x544, which is only needed for SceAvPlayer.
#include "video.h"
#include "renderer.h"
#include "gl_common.h"
#include "log.h"
#include "utils.h"
#include "android_jni.h"

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

#define POSITION_ATTRIB 0
#define TEXCOORD_ATTRIB 1

static const char* videoVertexShader =
    "#extension GL_OES_EGL_image_external : require\n"
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "uniform mat4 uSTMatrix;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    vTexCoord = (uSTMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "}\n";

static const char* videoFragmentShader =
    "#extension GL_OES_EGL_image_external : require\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform samplerExternalOES uTexture;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
    "}\n";

// Two triangles covering the whole surface. The uv of a corner is its position scaled into 0..1, so uv (0,0) is the
// top-left texel of the frame: the video surface, like every surface the runtime renders into, keeps the game's y-down
// orientation in its texture (v = 0 is the top row, which is what draw_surface assumes when it samples it).
static const float videoQuad[] = {
    // x,     y,     u,    v
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

// ===[ Java classes and methods, resolved once ]===

typedef struct {
    bool ready;
    jclass surfaceTextureClass, surfaceClass, mediaPlayerClass;
    jmethodID surfaceTextureCtor, surfaceTextureUpdate, surfaceTextureTimestamp, surfaceTextureTransform, surfaceTextureBufferSize, surfaceTextureRelease;
    jmethodID surfaceCtor, surfaceRelease;
    jmethodID playerCtor, playerSetDataSource, playerSetSurface, playerSetVolume, playerPrep, playerStart, playerPause,
              playerRelease, playerSeekTo, playerDuration, playerPosition, playerIsPlaying, playerVideoWidth,
              playerVideoHeight, playerSetLooping;
} JavaClasses;

static JavaClasses java;

static JNIEnv* videoEnv(void) {
    static bool attached = false;
    JavaVM* vm = Android_getJavaVM();
    if (vm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    jint r = (*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6);
    if (r == JNI_OK) return env;
    if (r != JNI_EDETACHED) return nullptr;
    if (attached) return nullptr; // attached by us already but GetEnv still says no: something is wrong
    if ((*vm)->AttachCurrentThread(vm, (void**) &env, nullptr) != JNI_OK) return nullptr;
    attached = true;
    return env;
}

// Any pending Java exception would kill the next JNI call; clear it and say which call failed.
static bool clearException(JNIEnv* env, const char* what) {
    if (!(*env)->ExceptionCheck(env)) return false;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    logError("Video: %s threw a Java exception\n", what);
    return true;
}

static jclass globalClass(JNIEnv* env, const char* name) {
    jclass local = (*env)->FindClass(env, name);
    if (local == nullptr || clearException(env, name)) return nullptr;
    jclass global = (jclass) (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return global;
}

/** Resolves everything the backend needs once. False when Java does not have the classes (never on Android). */
static bool resolveJava(void) {
    if (java.ready) return true;
    JNIEnv* env = videoEnv();
    if (env == nullptr) {
        logError("Video: no Java VM, video playback is unavailable\n");
        return false;
    }
    java.surfaceTextureClass = globalClass(env, "android/graphics/SurfaceTexture");
    java.surfaceClass = globalClass(env, "android/view/Surface");
    java.mediaPlayerClass = globalClass(env, "android/media/MediaPlayer");
    if (java.surfaceTextureClass == nullptr || java.surfaceClass == nullptr || java.mediaPlayerClass == nullptr) {
        logError("Video: android.media classes are missing, video playback is unavailable\n");
        return false;
    }

    java.surfaceTextureCtor = (*env)->GetMethodID(env, java.surfaceTextureClass, "<init>", "(I)V");
    java.surfaceTextureUpdate = (*env)->GetMethodID(env, java.surfaceTextureClass, "updateTexImage", "()V");
    java.surfaceTextureTimestamp = (*env)->GetMethodID(env, java.surfaceTextureClass, "getTimestamp", "()J");
    java.surfaceTextureTransform = (*env)->GetMethodID(env, java.surfaceTextureClass, "getTransformMatrix", "([F)V");
    java.surfaceTextureBufferSize = (*env)->GetMethodID(env, java.surfaceTextureClass, "setDefaultBufferSize", "(II)V");
    java.surfaceTextureRelease = (*env)->GetMethodID(env, java.surfaceTextureClass, "release", "()V");

    java.surfaceCtor = (*env)->GetMethodID(env, java.surfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    java.surfaceRelease = (*env)->GetMethodID(env, java.surfaceClass, "release", "()V");

    java.playerCtor = (*env)->GetMethodID(env, java.mediaPlayerClass, "<init>", "()V");
    java.playerSetDataSource = (*env)->GetMethodID(env, java.mediaPlayerClass, "setDataSource", "(Ljava/lang/String;)V");
    java.playerSetSurface = (*env)->GetMethodID(env, java.mediaPlayerClass, "setSurface", "(Landroid/view/Surface;)V");
    java.playerSetVolume = (*env)->GetMethodID(env, java.mediaPlayerClass, "setVolume", "(FF)V");
    java.playerPrep = (*env)->GetMethodID(env, java.mediaPlayerClass, "prepare", "()V");
    java.playerStart = (*env)->GetMethodID(env, java.mediaPlayerClass, "start", "()V");
    java.playerPause = (*env)->GetMethodID(env, java.mediaPlayerClass, "pause", "()V");
    java.playerRelease = (*env)->GetMethodID(env, java.mediaPlayerClass, "release", "()V");
    java.playerSeekTo = (*env)->GetMethodID(env, java.mediaPlayerClass, "seekTo", "(I)V");
    java.playerDuration = (*env)->GetMethodID(env, java.mediaPlayerClass, "getDuration", "()I");
    java.playerPosition = (*env)->GetMethodID(env, java.mediaPlayerClass, "getCurrentPosition", "()I");
    java.playerIsPlaying = (*env)->GetMethodID(env, java.mediaPlayerClass, "isPlaying", "()Z");
    java.playerVideoWidth = (*env)->GetMethodID(env, java.mediaPlayerClass, "getVideoWidth", "()I");
    java.playerVideoHeight = (*env)->GetMethodID(env, java.mediaPlayerClass, "getVideoHeight", "()I");
    java.playerSetLooping = (*env)->GetMethodID(env, java.mediaPlayerClass, "setLooping", "(Z)V");

    bool ok = java.surfaceTextureCtor != nullptr && java.surfaceTextureUpdate != nullptr
        && java.surfaceTextureTimestamp != nullptr && java.surfaceTextureTransform != nullptr
        && java.surfaceTextureBufferSize != nullptr && java.surfaceTextureRelease != nullptr
        && java.surfaceCtor != nullptr && java.surfaceRelease != nullptr
        && java.playerCtor != nullptr && java.playerSetDataSource != nullptr && java.playerSetSurface != nullptr
        && java.playerSetVolume != nullptr && java.playerPrep != nullptr && java.playerStart != nullptr
        && java.playerPause != nullptr && java.playerRelease != nullptr
        && java.playerSeekTo != nullptr && java.playerDuration != nullptr && java.playerPosition != nullptr
        && java.playerIsPlaying != nullptr && java.playerVideoWidth != nullptr && java.playerVideoHeight != nullptr
        && java.playerSetLooping != nullptr;
    if (!ok || clearException(env, "GetMethodID")) {
        logError("Video: the media API of this Android version is missing pieces, video playback is unavailable\n");
        return false;
    }
    java.ready = true;
    return true;
}

// ===[ Backend state ]===

typedef struct {
    jobject player;           // android.media.MediaPlayer, global ref
    jobject surface;          // android.view.Surface around the SurfaceTexture
    jobject surfaceTexture;   // android.graphics.SurfaceTexture the player renders into
    jfloatArray transform;    // reused float[16] for getTransformMatrix

    GLuint texture;           // GL_TEXTURE_EXTERNAL_OES fed by the SurfaceTexture
    GLuint program, quad, vao;
    GLint uSTMatrix, uTexture;

    bool opened, started, paused, ended, looping, hasFrame;
    int32_t idlePolls;        // consecutive polls of a player that is no longer playing (end detection)
    jlong lastTimestamp;
    int32_t width, height;
    double durationSec, positionSec;
    float stMatrix[16];
    uint32_t frames, heldPolls;
} AndroidVideo;

/** Compiles one shader stage, logging the driver's message when it fails. */
static GLuint compileStage(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512] = "";
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        logError("Video: %s shader failed to compile: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/** Compiles and links the external-texture program; attribute slots are fixed so the quad's layout is known. */
static GLuint buildProgram(void) {
    GLuint vert = compileStage(GL_VERTEX_SHADER, videoVertexShader);
    if (vert == 0) return 0;
    GLuint frag = compileStage(GL_FRAGMENT_SHADER, videoFragmentShader);
    if (frag == 0) {
        glDeleteShader(vert);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glBindAttribLocation(program, POSITION_ATTRIB, "aPosition");
    glBindAttribLocation(program, TEXCOORD_ATTRIB, "aTexCoord");
    glLinkProgram(program);
    glDeleteShader(vert);
    glDeleteShader(frag);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char info[512] = "";
        glGetProgramInfoLog(program, sizeof(info), nullptr, info);
        logError("Video: the external-texture program did not link: %s\n", info);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static bool ensureRenderObjects(AndroidVideo* v) {
    if (v->program != 0 && v->quad != 0) return true;

    v->program = buildProgram();
    if (v->program == 0) {
        logError("Video: cannot build the external-texture shader (GL_OES_EGL_image_external)\n");
        return false;
    }
    v->uSTMatrix = glGetUniformLocation(v->program, "uSTMatrix");
    v->uTexture = glGetUniformLocation(v->program, "uTexture");

    GLint prevBuffer = 0, prevVao = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);

    glGenBuffers(1, &v->quad);
    glBindBuffer(GL_ARRAY_BUFFER, v->quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(videoQuad), videoQuad, GL_STATIC_DRAW);

    if (v->vao == 0) {
        glGenVertexArrays(1, &v->vao);
        glBindVertexArray(v->vao);
        glBindBuffer(GL_ARRAY_BUFFER, v->quad);
        intptr_t stride = (intptr_t) (4 * sizeof(float));
        glEnableVertexAttribArray(POSITION_ATTRIB);
        glVertexAttribPointer(POSITION_ATTRIB, 2, GL_FLOAT, GL_FALSE, (GLsizei) stride, (const void*) 0);
        glEnableVertexAttribArray(TEXCOORD_ATTRIB);
        glVertexAttribPointer(TEXCOORD_ATTRIB, 2, GL_FLOAT, GL_FALSE, (GLsizei) stride, (const void*) (intptr_t) (2 * sizeof(float)));
    }

    glBindVertexArray((GLuint) prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) prevBuffer);
    return true;
}

/** Creates the external texture once; it is where the SurfaceTexture writes every frame. */
static bool ensureTexture(AndroidVideo* v) {
    if (v->texture != 0) return true;
    GLint prevUnit = GL_TEXTURE0, prev2d = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2d);

    glGenTextures(1, &v->texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, v->texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint) prev2d);
    glActiveTexture((GLenum) prevUnit);
    return v->texture != 0;
}

static void androidVideoClose(VideoDecoder* decoder);

static bool androidVideoOpen(VideoDecoder* decoder, const char* url) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    androidVideoClose(decoder);
    if (!resolveJava()) return false;

    JNIEnv* env = videoEnv();
    if (env == nullptr || !ensureTexture(v) || !ensureRenderObjects(v)) return false;

    // One local frame for every reference this function creates; the objects that outlive it are turned into globals.
    if ((*env)->PushLocalFrame(env, 8) != 0) {
        logError("Video: out of Java local references, cannot open %s\n", url);
        return false;
    }

    jstring path = (*env)->NewStringUTF(env, url);
    jobject st = (*env)->NewObject(env, java.surfaceTextureClass, java.surfaceTextureCtor, (jint) v->texture);
    jobject surface = st != nullptr ? (*env)->NewObject(env, java.surfaceClass, java.surfaceCtor, st) : nullptr;
    jobject player = surface != nullptr ? (*env)->NewObject(env, java.mediaPlayerClass, java.playerCtor) : nullptr;
    bool ok = path != nullptr && st != nullptr && surface != nullptr && player != nullptr
        && !clearException(env, "creating the media player");

    if (ok) {
        (*env)->CallVoidMethod(env, player, java.playerSetDataSource, path);
        ok = !clearException(env, "MediaPlayer.setDataSource");
    }
    if (ok) {
        (*env)->CallVoidMethod(env, player, java.playerSetSurface, surface);
        ok = !clearException(env, "MediaPlayer.setSurface");
    }
    if (ok) {
        (*env)->CallVoidMethod(env, player, java.playerSetVolume, 1.0f, 1.0f);
        (*env)->CallVoidMethod(env, player, java.playerSetLooping, (jboolean) (v->looping ? JNI_TRUE : JNI_FALSE));
        // Synchronous: the video surface the game creates right after video_open is sized from what this reports.
        (*env)->CallVoidMethod(env, player, java.playerPrep);
        ok = !clearException(env, "MediaPlayer.prepare");
    }

    int32_t w = 0, h = 0, durationMs = 0;
    if (ok) {
        w = (int32_t) (*env)->CallIntMethod(env, player, java.playerVideoWidth);
        h = (int32_t) (*env)->CallIntMethod(env, player, java.playerVideoHeight);
        durationMs = (int32_t) (*env)->CallIntMethod(env, player, java.playerDuration);
        ok = !clearException(env, "reading the video track") && w > 0 && h > 0;
        if (!ok) logError("Video: %s has no decodable video track (%dx%d)\n", url, w, h);
    }

    if (ok) {
        // The size the producer should use; the decoder sets its own when it disagrees.
        (*env)->CallVoidMethod(env, st, java.surfaceTextureBufferSize, (jint) w, (jint) h);
        (*env)->CallVoidMethod(env, player, java.playerStart);
        ok = !clearException(env, "MediaPlayer.start");
    }

    if (ok) {
        v->player = (*env)->NewGlobalRef(env, player);
        v->surface = (*env)->NewGlobalRef(env, surface);
        v->surfaceTexture = (*env)->NewGlobalRef(env, st);
        v->transform = (jfloatArray) (*env)->NewGlobalRef(env, (*env)->NewFloatArray(env, 16));
        // SurfaceTexture keeps the identity until the first frame arrives.
        for (int i = 0; i < 16; i++) v->stMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        v->width = w;
        v->height = h;
        v->durationSec = durationMs > 0 ? durationMs / 1000.0 : 0.0;
        v->opened = true;
        v->started = true;
        v->ended = false;
        v->hasFrame = false;
        v->lastTimestamp = -1;
    } else if (player != nullptr) {
        (*env)->CallVoidMethod(env, player, java.playerRelease);
        clearException(env, "MediaPlayer.release");
    }

    (*env)->PopLocalFrame(env, nullptr);
    if (!ok) {
        logError("Video: cannot play %s\n", url);
        return false;
    }
    logInfo("Video: opened %s (%dx%d, %.1f s) through MediaPlayer\n", url, w, h, v->durationSec);
    return true;
}

static void androidVideoClose(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    JNIEnv* env = java.ready ? videoEnv() : nullptr;
    if (env != nullptr && (v->player != nullptr || v->surfaceTexture != nullptr)) {
        if ((*env)->PushLocalFrame(env, 8) == 0) {
            if (v->player != nullptr) {
                // release() on its own is enough, and unlike stop() it is legal in every player state.
                (*env)->CallVoidMethod(env, v->player, java.playerRelease);
                clearException(env, "MediaPlayer.release");
            }
            if (v->surface != nullptr) {
                (*env)->CallVoidMethod(env, v->surface, java.surfaceRelease);
                clearException(env, "Surface.release");
            }
            if (v->surfaceTexture != nullptr) {
                // Only after the player let go of the buffer queue: the frames belong to it until then.
                (*env)->CallVoidMethod(env, v->surfaceTexture, java.surfaceTextureRelease);
                clearException(env, "SurfaceTexture.release");
            }
            (*env)->PopLocalFrame(env, nullptr);
        }
        if (v->player != nullptr) (*env)->DeleteGlobalRef(env, v->player);
        if (v->surface != nullptr) (*env)->DeleteGlobalRef(env, v->surface);
        if (v->surfaceTexture != nullptr) (*env)->DeleteGlobalRef(env, v->surfaceTexture);
        if (v->transform != nullptr) (*env)->DeleteGlobalRef(env, v->transform);
        v->player = nullptr;
        v->surface = nullptr;
        v->surfaceTexture = nullptr;
        v->transform = nullptr;
    }
    if (v->opened) logInfo("Video: closed after %u frames (%u polls without a new one)\n", v->frames, v->heldPolls);
    v->opened = v->started = v->paused = v->ended = v->hasFrame = false;
    v->lastTimestamp = -1;
    v->width = v->height = 0;
    v->durationSec = v->positionSec = 0.0;
    v->frames = v->heldPolls = 0;
    v->idlePolls = 0;
}

static void androidVideoQuit(VideoDecoder* decoder) {
    androidVideoClose(decoder);
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (v->vao != 0) glDeleteVertexArrays(1, &v->vao);
    if (v->quad != 0) glDeleteBuffers(1, &v->quad);
    if (v->program != 0) glDeleteProgram(v->program);
    if (v->texture != 0) glDeleteTextures(1, &v->texture);
    v->vao = v->quad = v->program = v->texture = 0;
}

static bool androidVideoInit(void) {
    return resolveJava();
}

// ===[ Playback ]===

static jint playerPositionMs(AndroidVideo* v) {
    JNIEnv* env = videoEnv();
    if (env == nullptr || v->player == nullptr) return -1;
    jint ms = (*env)->CallIntMethod(env, v->player, java.playerPosition);
    return clearException(env, "MediaPlayer.getCurrentPosition") ? -1 : ms;
}

static bool androidVideoIsRunning(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (!v->opened || v->ended) return false;
    if (v->paused) return true;
    jint ms = playerPositionMs(v);
    if (ms < 0) return true; // a failed query must not end the video
    if (ms / 1000.0 > v->positionSec) v->positionSec = ms / 1000.0;

    JNIEnv* env = videoEnv();
    if (env == nullptr) return true;
    bool playing = (*env)->CallBooleanMethod(env, v->player, java.playerIsPlaying) == JNI_TRUE;
    if (clearException(env, "MediaPlayer.isPlaying")) return true;

    // A looped video never ends on its own (the game stops it), and a player that has not produced a frame yet may
    // still be starting up. Otherwise "no longer playing" at the end of the track is the end: the last fraction of a
    // second is already reported by getCurrentPosition, so the stop is what says the final frame has played.
    bool atEnd = v->durationSec <= 0 || v->positionSec + 0.05 >= v->durationSec;
    if (!v->looping && !playing && atEnd) {
        // A player that never delivered a frame would otherwise never end the cutscene: stop waiting after a while.
        int32_t threshold = v->hasFrame ? 3 : 90;
        if (++v->idlePolls >= threshold) {
            v->ended = true;
            logInfo("Video: finished at %.2f of %.2f s after %u frames\n", v->positionSec, v->durationSec, v->frames);
            return false;
        }
    } else {
        v->idlePolls = 0;
    }
    return true;
}

static bool androidVideoIsPaused(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    return v->opened && !v->ended && v->paused;
}

static void androidVideoPause(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (!v->opened || v->paused) return;
    JNIEnv* env = videoEnv();
    if (env != nullptr && v->player != nullptr) {
        (*env)->CallVoidMethod(env, v->player, java.playerPause);
        clearException(env, "MediaPlayer.pause");
    }
    v->paused = true;
}

static void androidVideoResume(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (!v->opened || !v->paused) return;
    JNIEnv* env = videoEnv();
    if (env != nullptr && v->player != nullptr) {
        (*env)->CallVoidMethod(env, v->player, java.playerStart);
        clearException(env, "MediaPlayer.start");
    }
    v->paused = false;
}

static void androidVideoSetLoop(VideoDecoder* decoder, bool loop) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    v->looping = loop;
    if (!v->opened) return;
    JNIEnv* env = videoEnv();
    if (env != nullptr && v->player != nullptr) {
        (*env)->CallVoidMethod(env, v->player, java.playerSetLooping, (jboolean) (loop ? JNI_TRUE : JNI_FALSE));
        clearException(env, "MediaPlayer.setLooping");
    }
}

/** Reads the SurfaceTexture crop/flip matrix, so the frame lands in the surface the right way up. */
static void latchTransform(AndroidVideo* v) {
    JNIEnv* env = videoEnv();
    if (env == nullptr || v->transform == nullptr) return;
    (*env)->CallVoidMethod(env, v->surfaceTexture, java.surfaceTextureTransform, v->transform);
    if (clearException(env, "SurfaceTexture.getTransformMatrix")) return;
    (*env)->GetFloatArrayRegion(env, v->transform, 0, 16, v->stMatrix);
}

static int32_t androidVideoUpdate(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (!v->opened || v->ended || v->paused) return VIDEO_FRAME_HELD;
    JNIEnv* env = videoEnv();
    if (env == nullptr || v->surfaceTexture == nullptr) return VIDEO_FRAME_HELD;

    // Latch the newest decoded frame into the external texture. Cheap and non-blocking: it returns at once when the
    // decoder has not produced anything since the last call, leaving the previous frame in the texture.
    (*env)->CallVoidMethod(env, v->surfaceTexture, java.surfaceTextureUpdate);
    if (clearException(env, "SurfaceTexture.updateTexImage")) return VIDEO_FRAME_HELD;

    jlong timestamp = (*env)->CallLongMethod(env, v->surfaceTexture, java.surfaceTextureTimestamp);
    if (clearException(env, "SurfaceTexture.getTimestamp")) return VIDEO_FRAME_HELD;
    if (timestamp == v->lastTimestamp) {
        v->heldPolls++;
        // Frames carry the decoder's timestamp, so an unchanged one means "nothing new". A decoder that stamps every
        // frame with the same value would never look new, though: after a short wait take whatever the texture holds.
        if (!v->hasFrame && v->heldPolls >= 8) {
            logWarn("Video: the decoder does not report frame times, drawing on the first available frame\n");
            v->hasFrame = true;
            latchTransform(v);
            return VIDEO_FRAME_READY;
        }
        return VIDEO_FRAME_HELD;
    }
    bool first = !v->hasFrame;
    bool looped = v->hasFrame && timestamp < v->lastTimestamp; // the stream went back to its beginning
    v->lastTimestamp = timestamp;
    v->hasFrame = true;
    v->frames++;
    latchTransform(v);
    if (first) logInfo("Video: first frame after %u polls (%dx%d)\n", v->heldPolls, v->width, v->height);
    return looped ? VIDEO_FRAME_LOOPED : VIDEO_FRAME_READY;
}

/** Draws the latched frame into the game's video surface (an FBO), leaving the renderer's GL state as it was. */
static void androidVideoDraw(VideoDecoder* decoder, Runner* runner, int32_t surfaceId) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (!v->opened || !v->hasFrame || runner == nullptr || runner->renderer == nullptr) return;
    Renderer* renderer = runner->renderer;
    GLRenderer* gl = (GLRenderer*) renderer;
    if (surfaceId < 0 || (uint32_t) surfaceId >= gl->surfaceCount || gl->surfaces[surfaceId] == 0) return;
    if (!ensureRenderObjects(v)) return;

    // Queued GML geometry belongs to the target that is bound right now: submit it before switching targets.
    if (renderer->vtable->flush != nullptr) renderer->vtable->flush(renderer);

    int32_t w = gl->surfaceWidth[surfaceId], h = gl->surfaceHeight[surfaceId];
    if (w <= 0 || h <= 0) return;

    // Everything below is captured after the flush, so what is restored is the state the renderer's cache describes.

    GLint prevFbo = 0, prevProgram = 0, prevActive = GL_TEXTURE0, prevVao = 0, prevArrayBuffer = 0;
    GLint prevViewport[4];
    GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevColorMask[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
    GLint prevTex2d = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2d);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceId]);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(v->program);
    glBindVertexArray(v->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, v->texture);
    if (v->uTexture >= 0) glUniform1i(v->uTexture, 0);
    if (v->uSTMatrix >= 0) glUniformMatrix4fv(v->uSTMatrix, 1, GL_FALSE, v->stMatrix);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Put everything back: the renderer caches its own view of the state (blend in particular), so it must not be
    // left guessing what the GL context looks like.
    glBindVertexArray((GLuint) prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) prevArrayBuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prevTex2d);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevFbo);
    glUseProgram((GLuint) prevProgram);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
    if (gl->blendEnable) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glActiveTexture((GLenum) prevActive);
}

static double androidVideoDuration(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    return v->durationSec;
}

static double androidVideoPosition(VideoDecoder* decoder) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    // Monotonic, like the Vita backend: the game compares successive positions (and the credits stop on a jump back).
    if (v->opened && !v->ended) {
        jint ms = playerPositionMs(v);
        if (ms >= 0 && ms / 1000.0 > v->positionSec) v->positionSec = ms / 1000.0;
    }
    return v->positionSec;
}

static int32_t androidVideoWidth(VideoDecoder* decoder) { return ((AndroidVideo*) decoder->impl)->width; }
static int32_t androidVideoHeight(VideoDecoder* decoder) { return ((AndroidVideo*) decoder->impl)->height; }

static void androidVideoSeek(VideoDecoder* decoder, double seconds) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (!v->opened || v->ended || seconds < 0) return;
    JNIEnv* env = videoEnv();
    if (env != nullptr && v->player != nullptr) {
        (*env)->CallVoidMethod(env, v->player, java.playerSeekTo, (jint) (seconds * 1000.0));
        clearException(env, "MediaPlayer.seekTo");
    }
    v->positionSec = seconds;
}

static void androidVideoSetVolume(VideoDecoder* decoder, double volume) {
    AndroidVideo* v = (AndroidVideo*) decoder->impl;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    if (!v->opened) return;
    JNIEnv* env = videoEnv();
    if (env != nullptr && v->player != nullptr) {
        (*env)->CallVoidMethod(env, v->player, java.playerSetVolume, (jfloat) volume, (jfloat) volume);
        clearException(env, "MediaPlayer.setVolume");
    }
}

static VideoDecoderVtable androidVideoVtable;

VideoDecoder* VideoDecoder_createBackend(void) {
    if (!resolveJava()) return nullptr;
    VideoDecoder* decoder = (VideoDecoder*) safeCalloc(1, sizeof(VideoDecoder));
    AndroidVideo* impl = (AndroidVideo*) safeCalloc(1, sizeof(AndroidVideo));
    impl->lastTimestamp = -1;
    androidVideoVtable.init = androidVideoInit;
    androidVideoVtable.quit = androidVideoQuit;
    androidVideoVtable.open = androidVideoOpen;
    androidVideoVtable.close = androidVideoClose;
    androidVideoVtable.extractAudio = nullptr; // MediaPlayer plays the audio track itself
    androidVideoVtable.isRunning = androidVideoIsRunning;
    androidVideoVtable.isPaused = androidVideoIsPaused;
    androidVideoVtable.pause = androidVideoPause;
    androidVideoVtable.resume = androidVideoResume;
    androidVideoVtable.setLoop = androidVideoSetLoop;
    androidVideoVtable.setMasterClock = nullptr;
    androidVideoVtable.update = androidVideoUpdate;
    androidVideoVtable.draw = androidVideoDraw;
    androidVideoVtable.duration = androidVideoDuration;
    androidVideoVtable.position = androidVideoPosition;
    androidVideoVtable.width = androidVideoWidth;
    androidVideoVtable.height = androidVideoHeight;
    androidVideoVtable.seek = androidVideoSeek;
    androidVideoVtable.setVolume = androidVideoSetVolume;
    decoder->vtable = &androidVideoVtable;
    decoder->impl = impl;
    return decoder;
}
