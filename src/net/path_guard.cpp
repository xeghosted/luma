// src/net/path_guard.cpp
#include "net/path_guard.h"
#include "game/profile.h"

#include <stdio.h>
#include <string.h>

namespace net {

// The sandbox root is the detected game's data root, so a PUT can never
// escape into the other game's tree either.
static const char* ROOT_FALLBACK = "/data/luma/";
const char* root() {
    const game::profile* p = game::current();
    return p ? p->data_root : ROOT_FALLBACK;
}

namespace {
    // True if the path contains ".." as a whole segment. "..hidden" is a
    // filename and must not trip this.
    bool has_dotdot_segment(const char* p) {
        const char* seg = p;
        for (;;) {
            const char* end = strchr(seg, '/');
            size_t n = end ? (size_t)(end - seg) : strlen(seg);
            if (n == 2 && seg[0] == '.' && seg[1] == '.')
                return true;
            if (!end) return false;
            seg = end + 1;
        }
    }
}

bool safe_path(const char* rel, char* out, size_t out_len) {
    if (!rel || !rel[0]) return false;
    if (rel[0] == '/') return false;
    if (strchr(rel, '\\')) return false;
    if (has_dotdot_segment(rel)) return false;

    size_t need = strlen(root()) + strlen(rel);
    if (need + 1 > out_len) return false;

    snprintf(out, out_len, "%s%s", root(), rel);
    return true;
}

}  // namespace net
