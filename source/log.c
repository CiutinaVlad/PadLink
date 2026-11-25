#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include "log.h"
#include "version.h"

static FILE *s_log;
static Mutex s_lock;   // console and file are shared with the USB threads
static bool s_echo = true;
void logSetEcho(bool on) { s_echo = on; }

void logLock(void)   { mutexLock(&s_lock); }
void logUnlock(void) { mutexUnlock(&s_lock); }

void logOpen(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/padlink", 0777);
    s_log = fopen("sdmc:/switch/padlink/padlink.log", "a");
    if (s_log) {
        fprintf(s_log, "\n===== PadLink " PADLINK_APP_VERSION " start, HOS %u.%u.%u, AMS %s =====\n",
                HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()),
                hosversionIsAtmosphere() ? "yes" : "no");
        fflush(s_log);
        fsync(fileno(s_log));
    }
}

void logClose(void) {
    if (s_log) { fprintf(s_log, "===== clean exit =====\n"); fclose(s_log); s_log = NULL; }
}

void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    mutexLock(&s_lock);
    if (s_echo) printf("%s\n", line);
    if (s_log) {
        fprintf(s_log, "[%8llu ms] %s\n", (unsigned long long)ms, line);
        fflush(s_log);
        fsync(fileno(s_log));   // survive a crash right after this line
    }
    mutexUnlock(&s_lock);
}
