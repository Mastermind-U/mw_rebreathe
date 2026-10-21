#pragma once
#include <windows.h>
bool InstallScreenshotHooks(HMODULE image);
const char* ScreenshotHookStatus();
void SetScreenshotLogCallback(void (*callback)(const char*));
