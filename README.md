# Luma

A GoldHEN plugin that gives **RAGE games on PS4 a Lua runtime**, from one .prx.

It identifies the game it was loaded into by a byte signature and switches
everything that differs behind one profile: native table, frame hook, data root,
control port.

| | Grand Theft Auto V | Red Dead Redemption 2 |
| --- | --- | --- |
| Build | CUSA00411 v1.57 | CUSA03041 v1.32 |
| Data root | /data/gtalua/ | /data/rdr2lua/ |
| Control port | 9615 | 9616 |
| Natives | 6,691, walked out of the games masked registry at runtime | 7,072, compiled in from the eboot |
| Script globals | available | not located in this build |

The data roots are deliberately separate: one console can host both, and a
script must never land in the other games scripts/ directory.

**An unknown build does not load.** Luma writes the bytes it found at the
signature address into the log and stops, rather than running on with an
address table that belongs to a different executable.

# RDR2Lua

A GoldHEN plugin that lets you mod **Red Dead Redemption 2 on PS4 in Lua**. It boots a
Lua 5.4 runtime inside the game, gives scripts a bridge to the RAGE native functions,
and runs their `on_tick` callbacks once per frame on the game thread.

Ported from [GTALua](../GTALua), which does the same for GTA V. The Lua runtime, the
control channel, the resource model and the VS Code extension came across unchanged;
everything that touches the game — the invoker, the native table, the frame hook — was
re-derived against RDR2.

**Target: CUSA03041, game version 1.32.** An RVA is only valid for one build. On any
other version these addresses point at something else and will crash the game.

## Console layout

```
/data/rdr2lua/natives.lua      generated bindings for every native, loaded first
/data/rdr2lua/scripts/*.lua    your scripts, loaded in directory order at boot
/data/rdr2lua/resources/*/     resources, each with an fxmanifest.lua
/data/rdr2lua/rdr2lua.log      everything log()/print()/notify() wrote, plus load errors
```

## Natives

`natives.lua` binds **7,072 natives — every one this build registers — all by RVA.**

```lua
local ped = PLAYER_PED_ID()
local c   = GET_ENTITY_COORDS(ped, 1, 0)   -- Vector3 returns arrive as {x, y, z}
SET_ENTITY_COORDS(ped, c[1], c[2], c[3] + 50.0, false, false, false, true)
```

Two things are worth knowing before you go looking for a native by name:

- **Only 2,325 have a spelled-out name.** The other 4,747 are bound as
  `_0x<16 hex digits>`, because no public name exists for them. They are perfectly
  callable — you just have to know which one you want.
- **There is no hash registry to wait for.** GTALua reaches 3,674 of its natives by
  hash, resolved by walking the running game's own table, so scripts there have to
  guard on `natives_ready()`. RDR2 registers its natives in the eboot in the clear,
  so every address is known before boot and `natives_ready()` is true as soon as the
  image base resolves. `invoke_hash` still works; it is a binary search over a
  compiled-in table.

Out-parameters take an address: `mem_alloc(n)` gives you zeroed scratch, `read_mem` /
`read_float` / `read_string` read it back, `mem_free` releases it.

**A Vector3 out-param is 24 bytes, not 12.** RDR2 reads and writes the caller's
buffer at an 8-byte stride, so `mem_alloc(24)` and `read_vector3(p)`. See the comment
at the top of `src/rage/invoker/invoker.h`, which shows the disassembly this comes
from.

## How the native table was recovered

This is the part that is genuinely different from GTALua, and the reason the port was
tractable at all.

GTA V masks its native registry: every field is XOR-encoded and the table only exists
in the running process, so GTALua has to walk it at runtime and verify the result
before trusting it. **RDR2 does none of that.** The compiler emitted every
registration inline and in the clear:

```asm
movabs  rcx, 0x4EDE34FBADD967A6      ; the hash        (WAIT)
lea     rdx, [rip + 0x3906ef3]       ; the handler
mov     [r14 + rax*8 + 0x48], rcx    ; node->hashes[i]
mov     [r14 + rax*8 + 8],  rdx      ; node->funcs[i]
```

`tools/extract_registry.py` decodes those store pairs and recovers all 7,072
`hash -> RVA` mappings from `eboot.bin` alone — no console, no disassembler, ~4
seconds. `tools/gen_natives.py` then joins that against the names and signatures in
`tools/rdr2_native_signatures.h` **on the hash**, never on position, which is what
makes a wrong name unable to drag a wrong address along with it.

Regenerate both after any change:

```sh
python tools/extract_registry.py /path/to/eboot.bin -o tools/native_registry.json
python tools/gen_natives.py
```

`tests/run.sh` gates on the committed bindings matching a fresh regeneration, so they
cannot drift from the registry unnoticed.

## What has actually been proven

The plugin runs on a real console. From a boot log on CUSA03041 v1.32:

```
[hash_natives] 7072 natives compiled in; GET_HASH_KEY -> 0x3DBB3D48 (OK)
rdr2lua: frame hook is alive (frame 122)
hello: ped=258 at -1347.5, 2436.1, 308.5
hello.lua: ped=258 at -1908.8, 3281.8, 546.9 (0.0 m/s)
player model is 2.01 m tall
```

Each line settles something different:

| evidence | what it establishes |
| --- | --- |
| `GET_HASH_KEY -> 0x3DBB3D48` | the game computed `joaat("rdr2lua")` itself and matched the predicted value — the extracted registry and the base + RVA arithmetic are right |
| `frame hook is alive` | the detour and its trampoline survive the relocation |
| real world coordinates | Vector3 **returns** decode from the padded three-slot layout |
| `0.0 m/s` | float returns |
| `2.01 m tall` | Vector3 **out-params**: two padded 24-byte buffers through `set_vector_results`. This is the path that silently returned `{0,0,0}` on GTA V for a long time, so a non-zero answer is the one that matters |

Earlier, calling extracted addresses directly over RPC also gave `SQRT(16.0)` =
4.0, `POW(2.0, 10.0)` = 1024.0 and `GET_HASH_KEY("gtalua")` = `0x72B71C36`.

**Known gap:** `script_global()` / `set_script_global()` raise an error. The
script-global block table has not been located in this build, and GTA V's
address would read and write arbitrary memory here. Everything above works
without it — see `src/script/script_mem.h`.

## Build

Needs the OpenOrbis toolchain with `OO_PS4_TOOLCHAIN` set, under WSL or Linux.

```sh
./build.sh          # -> build/RDR2Lua.prx
bash tests/run.sh   # host tests: bridge, marshalling, net, resources
```

## Deploy

`pwsh tools/deploy.ps1 -Ip <console-ip>` pushes the prx and scripts over GoldHEN's FTP.
Add the plugin to `/data/GoldHEN/plugins.ini` under `CUSA03041`, then restart the game.

## The live loop

The plugin listens on **TCP 9616** — deliberately not GTALua's 9615, so both can sit on
one console without the editor attaching to the wrong game. The VS Code extension
[RAGE Script Manager](https://marketplace.visualstudio.com/items?itemName=DominikHeise.rage-script-manager) connects, pushes scripts and restarts
resources without a game restart; a new `.prx` still needs one. It serves GTALua too —
point `rageScriptManager.port` at 9616 for this one. `pwsh tools/console.ps1 <command>` speaks the same protocol from
a shell.

**There is no authentication by default.** Anyone who can reach port 9616 can run
arbitrary Lua in the game. Write a secret to `/data/rdr2lua/token` and set
`rageScriptManager.token` in VS Code to gate it.
