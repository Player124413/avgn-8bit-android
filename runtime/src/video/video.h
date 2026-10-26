#ifndef _BS_VIDEO_H_
#define _BS_VIDEO_H_

#include "common.h"
#include "runner.h"

typedef struct VideoDecoder VideoDecoder;

typedef enum {
    VIDEO_FRAME_NONE = 0,
    VIDEO_FRAME_READY = 1,
    VIDEO_FRAME_LOOPED = 2,
    VIDEO_FRAME_HELD = 3,
} VideoFrameResult;

typedef struct {
    bool (*init)(void);
    void (*quit)(VideoDecoder* decoder);
    bool (*open)(VideoDecoder* decoder, const char* url);
    void (*close)(VideoDecoder* decoder);
    bool (*extractAudio)(VideoDecoder* decoder, Runner* runner, const char* url);
    bool (*isRunning)(VideoDecoder* decoder);
    bool (*isPaused)(VideoDecoder* decoder);
    void (*pause)(VideoDecoder* decoder);
    void (*resume)(VideoDecoder* decoder);
    void (*setLoop)(VideoDecoder* decoder, bool loop);
    void (*setMasterClock)(VideoDecoder* decoder, double positionSeconds);
    int32_t (*update)(VideoDecoder* decoder);
    void (*draw)(VideoDecoder* decoder, Runner* runner, int32_t surfaceId);
    double (*duration)(VideoDecoder* decoder);
    double (*position)(VideoDecoder* decoder);
    int32_t (*width)(VideoDecoder* decoder);
    int32_t (*height)(VideoDecoder* decoder);
    // Optional (nullptr when unsupported): video_seek_to, and video_set_volume for backends that play the
    // audio themselves instead of extracting it for the runner's audio system.
    void (*seek)(VideoDecoder* decoder, double positionSeconds);
    void (*setVolume)(VideoDecoder* decoder, double volume);
} VideoDecoderVtable;

struct VideoDecoder {
    VideoDecoderVtable* vtable;
    void* impl;
};

VideoDecoder* VideoDecoder_createBackend(void);

void Video_executePendingAsyncEvents(Runner* runner);

// App lifecycle (Android: the runner loop is parked and the audio device is suspended while the app is in the
// background). A backend that plays its own audio, like MediaPlayer, keeps going on its own, so it is paused here.
// Only a video this paused is resumed, so a cutscene the game paused itself stays paused.
void Video_pauseForBackground(void);
void Video_resumeForForeground(void);

#endif /* _BS_VIDEO_H_ */