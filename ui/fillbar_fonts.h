#pragma once
#include <windows.h>

// Native fillbar captions use the compact stock font role (Font1).
bool InstallFillbarFontHooks(HMODULE gameModule);
const char* FillbarFontHookStatus();
