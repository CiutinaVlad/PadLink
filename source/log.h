#pragma once
// Crash-safe step log on the SD card: every line is flushed and fsync'd, so if the
// console crashes, the last line in sdmc:/switch/padlink/padlink.log is the last
// step that finished. Lines also go to the on-screen console.
#include <switch.h>

void logOpen(void);
void logClose(void);
void logSetEcho(bool on);   // false: log file only, nothing on screen
// Hold this around consoleUpdate()/cursor moves on the main thread.
void logLock(void);
void logUnlock(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)
// Log a libnx call result as "step: rc=0x... (module-desc)".
#define LOG_RC(step, rc) logf_("%-34s rc=0x%08X (%04d-%04d)", step, (unsigned)(rc), 2000 + R_MODULE(rc), R_DESCRIPTION(rc))
