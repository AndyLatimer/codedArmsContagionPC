// Game-specific hooks for Coded Arms: Contagion (ULES-00718), called by the recompiled code
// (tools/recomp/games/ULES00718/hooks.tsv).
//
// The front end talks to the game through the state in psp/host.h (mouse aim, aim down sights,
// frame rate, dev mode, OPTIONS row). In Coded Arms (ULES-00124) each of those is wired to the
// game by a hook at an address found with the store watchpoint build; see
// docs/ca1/game_hooks_ULES00124.cpp. Contagion is a different engine with its own addresses
// (TECHNICAL_NOTES.md section 10); the state not wired yet is kept but not applied.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "psp/host.h"
#include "psp/iso.h"
#include "psp/memory.h"
#include "psp/recomp.h"

namespace psp::host {
Aim g_aim;
std::atomic<bool> g_ads{false};
AdsSettings g_adsSettings;
std::atomic<uint32_t> g_weaponName{0};
DevMode g_dev;
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

// Disc text patches applied while reading the ISO (IsoReader::addPatch). None yet.
namespace psp::kernel {
void installTextPatches(IsoReader&) {}
}
