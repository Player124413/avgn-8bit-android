#ifndef _BS_INPUT_RECORDING_H_
#define _BS_INPUT_RECORDING_H_

#include "common.h"
#include <stdint.h>

#include "runner_keyboard.h"
#include "runner_gamepad.h"

typedef struct InputFrame {
    int32_t* keysPressed;
    int32_t* keysReleased;
    int32_t* padDown;      // playback only: gamepad slot 0 button indices pressed / released this frame
    int32_t* padUp;
} InputFrame;

typedef struct InputRecording {
    bool isRecording;
    bool isPlayback;
    bool filterDebugKeys;
    const char* recordFilePath;

    // Recording: stb_ds array of stb_ds InputFrame arrays (one per frame)
    InputFrame* recordedFrames;

    // Playback: same structure, loaded from JSON
    InputFrame* playbackFrames;
    int32_t playbackFrameCount;
    bool playbackEnded;

    // Playback of gamepad buttons (port testing: drives slot 0 like a connected pad)
    bool padActive;
    bool padHeld[GP_BUTTON_COUNT];
    bool padHeldPrev[GP_BUTTON_COUNT];
} InputRecording;

// Create a recorder that snapshots keyboard state each frame
InputRecording* InputRecording_createRecorder(const char* filePath);

// Create a player that loads recorded input from a JSON file.
// If recordFilePath is non-null, also enables recording (playback + record mode).
InputRecording* InputRecording_createPlayer(const char* playbackFilePath, const char* recordFilePath);

// Free all resources
void InputRecording_free(InputRecording* recording);

// Called each frame: plays back recorded input (if active), then snapshots keyboard state (if recording)
void InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frameNumber);

// Called each frame after platform input polling: applies played-back gamepad buttons to slot 0
void InputRecording_applyGamepad(InputRecording* recording, RunnerGamepadState* gp, int frameNumber);

// Write recorded frames to the JSON file (returns true on success)
bool InputRecording_save(InputRecording* recording);

// Null-safe check: returns true if recording is non-null and playback hasn't ended yet
bool InputRecording_isPlaybackActive(InputRecording* recording);

#endif /* _BS_INPUT_RECORDING_H_ */
