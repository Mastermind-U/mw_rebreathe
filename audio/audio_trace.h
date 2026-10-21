#pragma once
#include <windows.h>
// Install only while the game's primary thread is suspended.
// Measurement is enabled only when MW_MODERN_TRACE=1.
bool InstallAudioTrace(HMODULE image);
void FlushAudioTrace();
const char* AudioTraceStatus();
#ifdef AUDIO_TRACE_TESTING
void SetAudioTraceTestFailures(int flushCall,int protectionCall);
#endif
