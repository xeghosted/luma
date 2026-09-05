#include "platform/log.h"
#include "game/profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>

// FreeBSD open(2) flags — the OpenOrbis headers don't provide them.
#define ORBIS_O_WRONLY 0x0001
#define ORBIS_O_APPEND 0x0008
#define ORBIS_O_CREAT  0x0200

// Written into the detected game's data root. Before detection succeeds
// there is nowhere correct to write, so those lines go to the klog only.
#define LOG_PATH (game::current() ? game::current()->log_path : nullptr)

namespace platform {

    static void (*g_line_sink)(const char*) = nullptr;

    void set_line_sink(void (*fn)(const char* line)) { g_line_sink = fn; }

    // One open/write/close per line: nothing is buffered, so a crash still
    // leaves every line that was written before it. The sink runs whether or
    // not the file opened - a failed log file must not also silence the editor.
    void log_line(const char* tag, const char* msg) {
        // Before the game is identified there is no correct file to write to,
        // and writing into one game's log while running the other would be
        // worse than not writing at all. Those lines still reach the sink and
        // the klog; they are simply not on disk.
        const char* path = LOG_PATH;
        int fd = path ? sceKernelOpen(path, ORBIS_O_WRONLY | ORBIS_O_APPEND | ORBIS_O_CREAT, 0666) : -1;
        if (fd >= 0) {
            if (tag && tag[0]) {
                sceKernelWrite(fd, "[", 1);
                sceKernelWrite(fd, tag, strlen(tag));
                sceKernelWrite(fd, "] ", 2);
            }
            sceKernelWrite(fd, msg, strlen(msg));
            sceKernelWrite(fd, "\n", 1);
            sceKernelClose(fd);
        }

        if (g_line_sink) {
            if (tag && tag[0]) {
                char joined[512];
                snprintf(joined, sizeof(joined), "[%s] %s", tag, msg);
                g_line_sink(joined);
            } else {
                g_line_sink(msg);
            }
        }
    }

    void logf(const char* tag, const char* fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log_line(tag, buf);
    }

    void klogf(const char* fmt, ...) {
        char buf[256];
        int n = 0;
        buf[n++] = 'G'; buf[n++] = 'L'; buf[n++] = 'U'; buf[n++] = 'A'; buf[n++] = ' ';

        va_list ap; va_start(ap, fmt);
        vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
        va_end(ap);

        size_t len = strlen(buf);
        if (len == 0 || buf[len - 1] != '\n') {
            if (len < sizeof(buf) - 1) { buf[len] = '\n'; buf[len + 1] = 0; }
        }
        sceKernelDebugOutText(0, buf);
    }
}
