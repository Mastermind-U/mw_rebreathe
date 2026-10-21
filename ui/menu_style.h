#pragma once
#include "native_ui.h"
bool InstallMenuStyle(HMODULE game);
const char* MenuStyleStatus();
void SetMenuStyleLogCallback(void (*callback)(const char*));
void LogMenuStyle(const char* line);
void ApplyMenuTheme(HMODULE game, ModernUi::Element* root);
