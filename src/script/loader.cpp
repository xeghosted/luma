#include "script/loader.h"
#include "script/lua_runtime.h"
#include "script/native_bridge.h"
#include "script/resource.h"
#include "rage/invoker/invoker.h"
#include "rage/invoker/hash_natives.h"
#include "game/profile.h"
#include "platform/log.h"
#include "net/mailbox.h"
#include "net/dispatch.h"

#include <orbis/libkernel.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace script {
namespace {

const char* script_dir() { return game::current()->script_dir; }
const char* prelude() { return game::current()->prelude_path; }

// FreeBSD open(2) flags and lseek whence — the OpenOrbis headers don't expose them.
#define ORBIS_O_RDONLY 0x0000
#define ORBIS_SEEK_SET 0
#define ORBIS_SEEK_END 2

void notify(const char* msg) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type            = NotificationRequest;
    req.targetId        = -1;
    req.useIconImageUri = 1;
    strncpy(req.iconUri, "cxml://psnotification/tex_icon_system", sizeof(req.iconUri) - 1);
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// Script output: untagged in the log (a script's own text, verbatim) and, via
// platform::log_line's own sink, out to the editor. NOT to the screen.
//
// It used to notify() here too, on the reasoning that a script logs because it
// wants to be seen. In practice that made every logged line a system
// notification: the two shipped hello examples between them log three lines
// every few seconds, which is roughly twenty notifications a minute, and the
// PS4 queue shows each for several seconds. It never drains, so the messages
// that actually matter -- "rdr2lua: ready", "base resolve FAILED", "control
// channel FAILED" -- arrive behind a backlog nobody can read past. A script
// that does want the screen now calls notify() and says so.
void log_sink(const char* msg) {
    platform::log_line(nullptr, msg);
}

// Run `fn` with the marshalled slots and copy back the three return slots. Three
// because a Vector3 return occupies one 8-byte slot per component rather than a
// packed 12 bytes — see the layout note in invoker.h.
void call_impl(void* fn, const uint64_t* slots, int n, uint64_t* result) {
    using namespace rage::invoker;
    native_setup ctx;
    for (int i = 0; i < n; ++i) ctx.push<uint64_t>(slots[i]);
    ((native_handler)fn)(&ctx);
    // The Lua path builds its own context rather than going through
    // invoker::invoke(), so it needs the Vector3 write-back too -- and it is
    // the path that matters most, since a script is where mem_alloc'd
    // out-params come from in the first place.
    ctx.set_vector_results();
    uint64_t* rd = (uint64_t*)ctx.get_return_data();
    result[0] = rd[0];
    result[1] = rd[1];
    result[2] = rd[2];
}

// The real native-call seam: marshalled slots -> RAGE call context -> impl.
void real_call_native(uint64_t rva, const uint64_t* slots, int n, uint64_t* result) {
    if (!rage::invoker::g_eboot_base) {
        result[0] = result[1] = result[2] = 0;
        return;
    }
    call_impl((void*)(rage::invoker::g_eboot_base + rva), slots, n, result);
}

// Same by hash, through the registry walked out of the game's own command table.
// False means "could not call it", never "it returned 0".
bool real_call_native_hash(uint64_t hash, const uint64_t* slots, int n, uint64_t* result) {
    if (!rage::hash_natives::usable())
        return false;
    void* fn = rage::hash_natives::find(hash);
    if (!fn)
        return false;
    call_impl(fn, slots, n, result);
    return true;
}

bool natives_ready() { return rage::hash_natives::usable(); }
bool native_exists(uint64_t hash) { return rage::hash_natives::find(hash) != nullptr; }
uint64_t native_addr(uint64_t hash) { return (uint64_t)rage::hash_natives::find(hash); }

// The hash registry is read out of the game rather than hooked into: GoldHEN's
// load order relative to the script system's registration is not something we
// control, so the table is walked once here — on the game thread, from inside
// the frame hook, which means a script has already been running natives and the
// table is populated — and retried from the frame callback if that first walk
// came back unusable.
int  g_hash_tries = 0;
int  g_hash_frames = 0;
const int HASH_MAX_TRIES = 10;
const int HASH_RETRY_FRAMES = 120;

void try_build_hash_registry() {
    if (rage::hash_natives::usable() || g_hash_tries >= HASH_MAX_TRIES)
        return;
    g_hash_tries++;
    if (rage::hash_natives::build())
        platform::logf("natives", "hash registry ready: %u natives (attempt %d)",
                       (unsigned)rage::hash_natives::entry_count(), g_hash_tries);
    else if (g_hash_tries == HASH_MAX_TRIES)
        platform::logf("natives", "hash registry unusable after %d attempts - "
                                  "hash-bound natives will raise instead of running",
                       HASH_MAX_TRIES);
}

bool load_file(const char* path, const char* name) {
    int fd = sceKernelOpen(path, ORBIS_O_RDONLY, 0);
    if (fd < 0) return false;
    off_t size = sceKernelLseek(fd, 0, ORBIS_SEEK_END);
    sceKernelLseek(fd, 0, ORBIS_SEEK_SET);
    if (size <= 0 || size > (4 << 20)) { sceKernelClose(fd); return false; }
    char* buf = (char*)malloc((size_t)size);
    if (!buf) { sceKernelClose(fd); return false; }
    size_t got = sceKernelRead(fd, buf, (size_t)size);
    sceKernelClose(fd);
    bool ok = (got == (size_t)size) && runtime_load_buffer(name, buf, got);
    free(buf);
    return ok;
}

int load_dir() {
    int dfd = sceKernelOpen(script_dir(), ORBIS_O_RDONLY, 0);
    if (dfd < 0) {
        notify("luma: the scripts directory was not found");
        return 0;
    }
    int loaded = 0;
    char dents[4096];
    int nb;
    while ((nb = sceKernelGetdents(dfd, dents, sizeof(dents))) > 0) {
        int off = 0;
        while (off < nb) {
            struct dirent* de = (struct dirent*)(dents + off);
            if (de->d_reclen == 0) break;
            const char* nm = de->d_name;
            size_t l = strlen(nm);
            if (l > 4 && strcmp(nm + l - 4, ".lua") == 0) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s", script_dir(), nm);
                if (load_file(path, nm)) ++loaded;
            }
            off += de->d_reclen;
        }
    }
    sceKernelClose(dfd);
    return loaded;
}

}  // namespace

int script_boot() {
    g_call_native      = real_call_native;
    g_call_native_hash = real_call_native_hash;
    g_natives_ready    = natives_ready;
    g_native_exists    = native_exists;
    g_native_addr      = native_addr;
    g_eboot_base_abs   = rage::invoker::g_eboot_base;
    runtime_set_log_sink(log_sink);
    runtime_set_notify_sink(notify);

    // Script output reaches the editor live. Installed here rather than in
    // net_server so the net layer stays free of any knowledge of logging.
    platform::set_line_sink(net::outbox_push_line);

    // Before any script runs, so the hash-bound half of natives.lua is live from
    // the first tick rather than raising through the first couple of seconds.
    try_build_hash_registry();

    if (!runtime_init()) {
        notify("luma: runtime_init FAILED");
        return -1;
    }

    // Prelude first (best effort) so scripts can call natives by name.
    load_file(prelude(), "natives.lua");

    int loaded = load_dir();
    char msg[64];
    snprintf(msg, sizeof(msg), "luma: %d script(s) loaded", loaded);
    notify(msg);

    // The resource model boots after the flat script directory, so a v1-style
    // script in the game's scripts/ directory keeps working exactly as before.
    if (resource_boot()) {
        int started = resource_autostart();
        char rmsg[64];
        snprintf(rmsg, sizeof(rmsg), "luma: %d resource(s) started", started);
        notify(rmsg);
    }

    return loaded;
}

void script_frame() {
    // Control-channel work first: a pushed script should take effect on the
    // same frame it arrives, not the next one.
    net::dispatch_pending();

    // Only if the boot-time walk failed; usable() short-circuits otherwise.
    if (++g_hash_frames >= HASH_RETRY_FRAMES) {
        g_hash_frames = 0;
        try_build_hash_registry();
    }
    runtime_tick();
    resource_tick();
}

int script_reload() {
    // Before load_dir, never after: the files are about to re-register their
    // callbacks, and anything left over from the previous run would be a
    // duplicate of one of them. See runtime_clear_ticks() for what that
    // duplication actually breaks. tests/run.sh gates this ordering, because
    // this file is ORBIS-only and no host test can link it.
    runtime_clear_ticks();
    return load_dir();
}

}  // namespace script
