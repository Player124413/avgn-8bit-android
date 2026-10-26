#pragma once

// Where the Vita build keeps data.win, saves, settings, logs and test files. Set per game by the build
// (BS_VITA_DATA_DIR in CMakeLists.txt, passed as a compile definition); no trailing slash.
#ifndef BS_VITA_DATA_DIR
#define BS_VITA_DATA_DIR "ux0:data/butterscotch"
#endif

#define BS_VITA_PATH(name) BS_VITA_DATA_DIR "/" name

// All-in-one VPK: the game's read-only files (data.win, texcache_*.bin, fonts, gamecontrollerdb.txt, Lang/, Attract/,
// Cutscenes/) sit at the root of the VPK, i.e. app0: (read-only). Saves, settings, log.txt, marker files and a
// regenerated texture cache still go to BS_VITA_DATA_DIR. Without app0:data.win the runtime uses the classic layout
// (everything, data.win included, in BS_VITA_DATA_DIR). See src/vita/main.c.
#define BS_VITA_APP0_DIR "app0:"
#define BS_VITA_APP0_PATH(name) BS_VITA_APP0_DIR name
