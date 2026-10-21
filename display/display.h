#pragma once
#include <windows.h>

bool InstallDisplayHooks(HMODULE game);
const char* DisplayHookStatus();
void SetDisplayLogCallback(void (*callback)(const char*));

// Values match the launcher's [Display] ScreenMode setting.
enum class DisplayMode { Fullscreen = 0, Window = 1, Borderless = 2 };
DisplayMode GetDisplayMode();
bool SetDisplayMode(DisplayMode mode);

