// RDR2Lua — a small GoldHEN plugin that lets you mod Red Dead Redemption 2 on
// PS4 (CUSA03041 v1.32) in Lua.
//
// It boots a Lua 5.4 runtime inside the game, gives scripts a bridge to the RAGE
// native functions, and runs their on_tick callbacks once per frame on the game
// thread. Drop a .lua in /data/rdr2lua/scripts and restart the game.
//
// The frame hook is a detour on the PLAYER_PED_ID native — RDR2's counterpart to
// GTA V's GET_PLAYER_PED. The base game's own scripts call it many times per
// frame, which gives us a reliable place to run on the correct thread. That also
// means our tick lives inside whichever script thread called it: see README.

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include <GoldHEN/Common.h>
#include <orbis/libkernel.h>

#include "rage/invoker/invoker.h"
#include "game/game_thread.h"
#include "game/profile.h"
#include "script/loader.h"
#include "platform/log.h"
#include "net/net_server.h"
#include "version.h"

#define PLUGIN_NAME    "Luma"

#define attr_public __attribute__((visibility("default")))

attr_public const char *g_pluginName    = PLUGIN_NAME;
attr_public const char *g_pluginDesc    = "Lua scripting for RAGE games on PS4 - GTA V and Red Dead Redemption 2";
attr_public const char *g_pluginAuth    = "unknown";
attr_public const char *g_pluginVersion = PLUGIN_VERSION;

// On-screen notifications vanish after a few seconds, so mirror everything into
// the log file; pull it over FTP to read results after the fact.
static void notify(const char *msg)
{
    platform::log_line("Boot", msg);

    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type            = NotificationRequest;
    req.targetId        = -1;
    req.useIconImageUri = 1;
    strncpy(req.iconUri, "cxml://psnotification/tex_icon_system", sizeof(req.iconUri) - 1);
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void per_frame(void)
{
    script::script_frame();
}

static void boot_scripts(void)
{
    script::script_boot();
}

extern "C" {

int module_start(size_t argc, const void *argp)
{
    (void)argc;
    (void)argp;

    notify(PLUGIN_NAME " v" PLUGIN_VERSION " loaded");

    if (!rage::invoker::resolve_base())
    {
        notify("luma: base resolve FAILED - natives unavailable");
        return 0;
    }

    // Which game is this? Everything below depends on the answer -- the hook
    // address, the data root, the port -- so a failure here is fatal rather
    // than something to carry on past with a default. GoldHEN loads this plugin
    // for whatever title id it is listed under, and being listed under an
    // unsupported one is exactly how a wrong address table would get used.
    const game::profile* profile = game::detect();
    if (!profile)
    {
        notify("luma: unsupported game - see the log; not loading");
        return 0;
    }

    if (!game::install_frame_hook())
    {
        notify("luma: frame hook FAILED - scripts will not tick");
        return 0;
    }

    game::set_frame_callback(per_frame);

    if (!net::server_start())
        notify("luma: control channel FAILED - scripts still load from disk");

    // Boot Lua on the game thread: script top-level code may call natives, which
    // are only safe there.
    game::run_on_game_thread(boot_scripts);

    notify("luma: ready");
    return 0;
}

int module_stop(size_t argc, const void *argp)
{
    (void)argc;
    (void)argp;
    net::server_stop();
    notify(PLUGIN_NAME " unloaded");
    return 0;
}

}
