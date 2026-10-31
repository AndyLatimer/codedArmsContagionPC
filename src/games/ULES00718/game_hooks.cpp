// Game-specific hooks for Coded Arms: Contagion (ULES-00718), called by the recompiled code
// (tools/recomp/games/ULES00718/hooks.tsv).
//
// The front end talks to the game through the state in psp/host.h (mouse aim, aim down sights,
// frame rate, dev mode, OPTIONS row). In Coded Arms (ULES-00124) each of those is wired to the
// game by a hook at an address found with the store watchpoint build; see
// docs/ca1/game_hooks_ULES00124.cpp. Contagion is a different engine with its own addresses
// (TECHNICAL_NOTES.md sections 10 and 11); weapons (dev mode, number keys) go through the
// game's own Lua, as its scripts do.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "psp/host.h"
#include "psp/iso.h"
#include "psp/kernel.h"
#include "psp/memory.h"
#include "psp/recomp.h"

namespace psp::host {
Aim g_aim;
std::atomic<bool> g_ads{false};
AdsSettings g_adsSettings;
std::atomic<float> g_levelProjY{0};
std::atomic<uint32_t> g_weaponName{0};
std::atomic<uint32_t> g_playerUpdates{0};
LuaConsole g_luaConsole;
DevMode g_dev;
std::atomic<int> g_selectSlot{0};
GameOptions g_gameOptions;
std::atomic<int> g_targetFps{0};
}

namespace {
bool userPtr(uint32_t a) { return a >= 0x08800000 && a < 0x0A000000 && !(a & 3); }
float rdf(uint32_t a) { uint32_t v = psp::read32(a); float f; std::memcpy(&f, &v, 4); return f; }
void wrf(uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); psp::write32(a, v); }

// Current weapon ------------------------------------------------------------------------------
// The current weapon: player +0x29C -> +0x20 = the character, whose +0x54 is the weapon
// object; its first word points to the weapon's name ("g01" for the pistol).
void readWeapon(uint32_t player) {
    uint32_t name = 0;
    const uint32_t a = psp::read32(player + 0x29C);
    const uint32_t ch = userPtr(a) ? psp::read32(a + 0x20) : 0;
    const uint32_t w = userPtr(ch) ? psp::read32(ch + 0x54) : 0;
    const uint32_t n = userPtr(w) ? psp::read32(w) : 0;
    if (userPtr(n)) {
        name = psp::read32(n);
        // Up to 4 printable characters, then a terminator.
        for (int k = 0; k < 4; ++k) {
            const uint8_t b = (uint8_t)(name >> (8 * k));
            if (!b) { name &= (1u << (8 * k)) - 1; break; }
            if (b < 0x21 || b > 0x7E) { name = 0; break; }
        }
    }
    static const bool wtrace = std::getenv("CA_WEAPON_TRACE") != nullptr;
    if (wtrace && userPtr(w)) { // the weapon object's words that change (with the frame)
        static uint32_t last[96], lastW = 0, frame = 0;
        ++frame;
        for (int i = 0; i < 96; ++i) {
            const uint32_t v = psp::read32(w + 4 * i);
            if (w == lastW && v != last[i]) {
                float f; std::memcpy(&f, &v, 4);
                std::fprintf(stderr, "[WTRACE] %u +%02x %08x -> %08x (%g)\n", frame, 4 * i, last[i], v, f);
            }
            last[i] = v;
        }
        lastW = w;
    }
    if (psp::host::g_weaponName.exchange(name) != name && name) {
        const char t[5] = {(char)name, (char)(name >> 8), (char)(name >> 16), (char)(name >> 24), 0};
        std::fprintf(stderr, "[GAME] weapon %s\n", t);
    }
}
} // namespace

// Mouse / right stick aim ---------------------------------------------------------------------
// The player control function (0x0897196c: s3 = player, s2 = new state, s1 = input) asks
// TurnInput (0x089c0378) for this frame's turn from the pad, then adds it to the state's yaw
// (+16) and pitch (+20) in radians; the game itself wraps the yaw and clamps the pitch after
// that. The aim from the front end is added to those two deltas, so it goes through the same
// path as the pad (collision, network, replay of the state) and is independent of the frame
// rate: host::Aim::take() hands over the motion of the time this game frame covers.
// The view direction is (cos p sin y, sin p, -cos p cos y): a larger yaw turns right and a
// larger pitch looks up, while the front end's pitch grows downwards (screen space).
extern "C" void ca_hook_player_turn(Ctx* c) {
    const uint32_t sp = c->r[29];
    readWeapon(c->r[19]);
    psp::host::g_playerUpdates.fetch_add(1, std::memory_order_relaxed);
    static const bool inputTrace = std::getenv("CA_INPUT_TRACE") != nullptr;
    if (inputTrace) { // the input block the control function reads (s1), when it changes
        static uint32_t last[24];
        uint32_t now[24];
        bool diff = false;
        for (int i = 0; i < 24; ++i) { now[i] = psp::read32(c->r[17] + 4 * i); diff |= now[i] != last[i]; }
        if (diff) {
            std::fprintf(stderr, "[INPUT] %08x:", c->r[17]);
            for (int i = 0; i < 24; ++i) std::fprintf(stderr, " %08x", now[i]);
            std::fprintf(stderr, "\n");
            std::memcpy(last, now, sizeof last);
        }
    }
    static const bool trace = std::getenv("CA_HOOK_TRACE") != nullptr;
    std::lock_guard<std::mutex> lk(psp::host::g_aim.m);
    double yaw = 0, pitch = 0;
    if (psp::host::g_aim.enabled) psp::host::g_aim.take(yaw, pitch);
    if (trace)
        std::fprintf(stderr, "[HOOK] turn player=%08x pos=%.3f,%.3f,%.3f yaw=%f pitch=%f d=%f,%f aim=%f,%f\n", c->r[19],
                     rdf(c->r[18]), rdf(c->r[18] + 4), rdf(c->r[18] + 8), rdf(c->r[18] + 16), rdf(c->r[18] + 20),
                     rdf(sp + 236), rdf(sp + 232), yaw, pitch);
    if (yaw == 0 && pitch == 0) return;
    wrf(sp + 236, rdf(sp + 236) + (float)yaw);
    wrf(sp + 232, rdf(sp + 232) - (float)pitch);
}

// Developer Lua console --------------------------------------------------------------------
// The game runs Lua 5.0 (its base library: loadstring 0x08a2d4d8, pcall 0x08a2cf8c; print is
// compiled out). From those: luaL_loadbuffer 0x08a2a66c, lua_pcall 0x08a0a7a4, lua_gettop
// 0x08a0e488, lua_settop 0x08a0e418. In this build a stack slot (8 bytes) holds the value at
// +0 and the type at +4 (3 number, 4 string); L->top is at L+8; a string's text follows its
// 16-byte header, with the length at +12.
namespace {
constexpr uint32_t kLuaLoadBuffer = 0x08a2a66c, kLuaPcall = 0x08a0a7a4, kLuaGetTop = 0x08a0e488, kLuaSetTop = 0x08a0e418;

std::string luaTopToString(uint32_t L) {
    const uint32_t top = psp::read32(L + 8);
    if (!userPtr(top) || top < 8) return "?";
    const uint32_t obj = top - 8, tt = psp::read32(obj + 4), v = psp::read32(obj);
    switch (tt) {
    case 0: return "nil";
    case 1: return v ? "true" : "false";
    case 3: { float f; std::memcpy(&f, &v, 4); char b[32]; std::snprintf(b, sizeof b, "%g", f); return b; }
    case 4: {
        if (!userPtr(v)) return "?";
        const uint32_t len = psp::read32(v + 12);
        std::string out;
        for (uint32_t i = 0; i < len && i < 200000; ++i) out += (char)psp::read8(v + 16 + i);
        return out;
    }
    default: { char b[48]; std::snprintf(b, sizeof b, "(type %u: %08x)", tt, v); return b; }
    }
}
} // namespace

// Dev mode: the next / previous of the game's guns in the fourth slot, which takes any of them
// (player_state CheckWeaponAvailability: a weapon's wpn_type is the first slot it fits). The
// weapon HUD's LoadWpn unloads the slot's weapon, loads the new one and switches to it; once
// the old one is unloaded the game goes on through LoadWeapon, which would show the "loading
// plugin" box, so that one call goes straight to LoadWpn. Only three guns besides the pistol
// fit in memory at once, which is why they are changed in one slot.
static std::string devCycleChunk(int step) {
    char head[64];
    std::snprintf(head, sizeof head, "local step = %d\n", step);
    return head + std::string(R"(
local C = _import_cached_cache
local a = C.loader.cfg.objects.powerups.GetHandler().ammo
local list = {"smg", "rifle", "shotgun", "sniper", "rpg", "vulcan", "pulserifle", "grenadelauncher",
  "lightning", "gaussrifle", "nova", "dataleech"}
if a.unloading_weapon then return "dev: busy" end
local cur, i = a.slots[4], 0
for k, n in ipairs(list) do if n == cur then i = k end end
i = math.mod(i - 1 + step + 2 * table.getn(list), table.getn(list)) + 1
C.weapon_schemes.player_state.savedPlayerState:Get().weapon_slots[3] = list[i]
a.LoadWeapon = function(self, s, n) self.LoadWeapon = nil; return self:LoadWpn(s, n) end
if not cur then a.LoadWeapon = nil end
a:LoadWpn(4, list[i])
a.unlimited_ammo = true
return "dev: weapon " .. list[i]
)");
}

extern "C" void ca_hook_lua_pcall(Ctx* c) {
    static bool busy = false; // our own lua_pcall comes through here too
    if (busy) return;
    std::string code;
    if (const int step = psp::host::g_dev.cycle.exchange(0); step && psp::host::g_dev.enabled) {
        code = devCycleChunk(step);
    } else if (const int slot = psp::host::g_selectSlot.exchange(0); slot >= 1 && slot <= 5) {
        char chunk[512];
        std::snprintf(chunk, sizeof chunk,
                      "local a = _import_cached_cache.loader.cfg.objects.powerups.GetHandler().ammo\n"
                      "local n = a.slots[%d]\n"
                      "if not n or n == a.current_weapon or a.unloading_weapon then return 'slot %d: no change' end\n"
                      "a:SetWeapon(n)\n"
                      "return 'slot %d: ' .. n\n",
                      slot, slot, slot);
        code = chunk;
    } else {
        std::lock_guard<std::mutex> lk(psp::host::g_luaConsole.m);
        if (psp::host::g_luaConsole.pending.empty()) return;
        code = std::move(psp::host::g_luaConsole.pending.front());
        psp::host::g_luaConsole.pending.pop_front();
    }
    busy = true;
    const uint32_t L = c->r[4];
    static uint32_t buf = 0, cap = 0;
    const uint32_t need = (uint32_t)code.size() + 16;
    if (need > cap) { // the game leaves little of the user partition free: small blocks
        cap = std::max<uint32_t>((need + 4095) & ~4095u, 16 * 1024);
        buf = psp::kernel::allocHleMemory(cap, "dev Lua console");
        if (!buf) cap = 0;
    }
    if (!buf) {
        std::fprintf(stderr, "[LUA] no memory for the chunk\n");
        busy = false;
        return;
    }
    const char name[] = "=dev";
    for (uint32_t i = 0; i < sizeof name; ++i) psp::write8(buf + i, (uint8_t)name[i]);
    const uint32_t src = buf + 16;
    for (size_t i = 0; i < code.size(); ++i) psp::write8(src + (uint32_t)i, (uint8_t)code[i]);
    uint32_t top0 = 0, st = 0;
    psp::kernel::runGuestFunction(kLuaGetTop, L, 0, 0, 0, &top0);
    psp::kernel::runGuestFunction(kLuaLoadBuffer, L, src, (uint32_t)code.size(), buf, &st);
    if (st == 0) psp::kernel::runGuestFunction(kLuaPcall, L, 0, 1, 0, &st);
    const std::string result = luaTopToString(L);
    psp::kernel::runGuestFunction(kLuaSetTop, L, top0, 0, 0);
    std::fprintf(stderr, "[LUA] %s: %s\n", st == 0 ? "ok" : "error", result.c_str());
    busy = false;
}

// Disc text patches applied while reading the ISO (IsoReader::addPatch). None yet.
namespace psp::kernel {
void installTextPatches(IsoReader&) {}
}
