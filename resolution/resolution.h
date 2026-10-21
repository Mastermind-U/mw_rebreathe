#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Call once during startup, before Morrowind creates the Video options menu.
// Patches process memory only. The executable on disk is left intact.
bool InstallResolutionHooks(HMODULE game);
const char* ResolutionHookStatus();

