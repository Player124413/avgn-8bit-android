#ifndef _BS_OVERLAY_FILE_SYSTEM_H_
#define _BS_OVERLAY_FILE_SYSTEM_H_

#include "common.h"
#include "file_system.h"

// OverlayFileSystem implements GameMaker's two-area sandboxed file system on top of plain stdio.
// It holds two base paths:
// * bundlePath: read-only "File Bundle" area, where Included Files and the data.win live.
// * savePath: read/write "Save Area", the only place writes are allowed.
//
// Read operations check savePath first and fall back to bundlePath.
// Writes always target savePath. delete only acts on savePath (it will not touch a same-named file in the bundle).
//
// https://manual.gamemaker.io/lts/en/Additional_Information/The_File_System.htm
typedef struct {
    FileSystem base;
    char* bundlePath; // includes trailing '/'
    char* savePath; // includes trailing '/'
    // Optional read-only folder (ending with '/') checked before the save area and the bundle: the Vita's app0:, so the
    // VPK's own files (the cutscenes encoded for the Vita) win over same-named copies in ux0:data/<game> (the player's
    // stock game files, or an older install's). nullptr: not used.
    char* firstPath;
} OverlayFileSystem;

OverlayFileSystem* OverlayFileSystem_create(const char* bundlePath, const char* savePath);
void OverlayFileSystem_destroy(OverlayFileSystem* fs);

#endif /* _BS_OVERLAY_FILE_SYSTEM_H_ */
