#pragma once

// The one place that knows where RDR2Lua writes. There is no console on PS4, so
// diagnostics go to a file you pull over FTP; the loud ones also raise an
// on-screen notification. Everything that logs — main.cpp, the script loader,
// the native registry — goes through here so the path is defined once.
namespace platform {
    // Appends "[tag] msg\n". A null or empty tag writes the message alone,
    // which is what the Lua log()/print() sink wants.
    void log_line(const char* tag, const char* msg);
    void logf(const char* tag, const char* fmt, ...);

    // Kernel-log output (sceKernelDebugOutText, channel 0). Where GoldHEN
    // broadcasts the kernel log over TCP 3232, these lines appear live in
    // `nc <ip> 3232` INTERLEAVED with the game's own klog and any crash dump —
    // unlike the /data file, which needs FTP after the fact. Every line is
    // prefixed "GLUA" so it greps cleanly out of the firehose. Use for
    // crash-window tracing.
    //
    // "Where GoldHEN broadcasts it" is a real condition, not a formality. This
    // used to be written as a fact, and on 2026-09-03 a console falsified it:
    // TCP 3232 accepted a connection and streamed nothing for eight seconds
    // while traffic that klogs on every connection was driven through 9616.
    // The whole net layer logs through here and nothing else, so on a console
    // in that state net/ is silent everywhere — no `client connected`, no
    // `reply queue full`, no `malformed frame`. Adding a second sink into
    // rdr2lua.log was weighed and declined (an open/write/close and an outbox
    // push on the accept loop and the frame reader, to fix a channel that works
    // when the console is set up right). The consequence is accepted, and the
    // debugging step that follows from it is: get the klog working FIRST.
    // docs/m3-hardware-session.md has the measurement.
    //
    // NOT named klog(): GoldHEN's Utilities.h does `#define klog printf`, which
    // would rewrite this identifier wherever that header is in scope.
    void klogf(const char* fmt, ...);

    // An optional third destination for every line log_line() writes, on top of
    // the file. The control channel installs the outbox here so script output
    // reaches the editor live. Called on whatever thread logged; it must not
    // block and must not allocate — see net::outbox_push_line.
    void set_line_sink(void (*fn)(const char* line));
}
