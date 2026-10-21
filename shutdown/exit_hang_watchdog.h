#pragma once
#include <windows.h>

// Runner only. The caller retains the exact CreateProcess HANDLE through the
// final wait. No explicit remote thread manipulation, termination or memory
// writes are performed. DbgHelp may briefly suspend threads during capture.
class ExitHangWatchdog {
public:
    ExitHangWatchdog() noexcept;
    ~ExitHangWatchdog() noexcept;
    ExitHangWatchdog(const ExitHangWatchdog&)=delete;
    ExitHangWatchdog& operator=(const ExitHangWatchdog&)=delete;
    bool Initialize(HANDLE exactChild,bool probe=false) noexcept;
    void Poll() noexcept;
    const char* Status() const noexcept;
private:
    struct State;
    State* state_=nullptr;
};

#ifdef EXIT_HANG_WATCHDOG_TESTING
void SetExitHangWatchdogTestClock(bool enabled,ULONGLONG tick) noexcept;
void SetExitHangWatchdogTestFailures(bool dumpFailure,bool unstableWorld) noexcept;
void SetExitHangWatchdogTestReads(bool failedRead,bool shortRead) noexcept;
#endif
