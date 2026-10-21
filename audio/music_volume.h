#pragma once
#include <windows.h>
bool InstallMusicVolumeHooks(HMODULE image);
const char* MusicVolumeHookStatus();
using MusicVolumeLogCallback=void(*)(const char*);
void SetMusicVolumeLogCallback(MusicVolumeLogCallback callback);
#ifdef MUSIC_VOLUME_TESTING
void SetMusicVolumeTestFailures(int flushCall,int protectionCall);
#endif
