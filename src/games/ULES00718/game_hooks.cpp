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
DevMode g_dev;
GameOptions g_gameOptions;
std::atomic<int> g_targetFps{0};
}

namespace {
float rdf(uint32_t a) { uint32_t v = psp::read32(a); float f; std::memcpy(&f, &v, 4); return f; }
void wrf(uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); psp::write32(a, v); }
} // namespace

// Mouse / right stick aim ---------------------------------------------------------------------
// The player control function (0x0897196c: s3 = player, s2 = new state, s1 = input) asks
// TurnInput (0x089c0378) for this frame's turn from the pad, then adds it to the state's yaw
// (+16) and pitch (+20) in radians; the game itself wraps the yaw and clamps the pitch after
// that. The aim from the front end is added to those two deltas, so it goes through the same
// path as the pad (collision, network, replay of the state) and is independent of the frame
// rate: it is the motion accumulated since the last game frame.
// The view direction is (cos p sin y, sin p, -cos p cos y): a larger yaw turns right and a
// larger pitch looks up, while the front end's pitch grows downwards (screen space).
extern "C" void ca_hook_player_turn(Ctx* c) {
    const uint32_t sp = c->r[29];
    static const bool trace = std::getenv("CA_HOOK_TRACE") != nullptr;
    std::lock_guard<std::mutex> lk(psp::host::g_aim.m);
    if (trace)
        std::fprintf(stderr, "[HOOK] turn player=%08x pos=%.3f,%.3f,%.3f yaw=%f pitch=%f d=%f,%f aim=%f,%f\n", c->r[19],
                     rdf(c->r[18]), rdf(c->r[18] + 4), rdf(c->r[18] + 8), rdf(c->r[18] + 16), rdf(c->r[18] + 20),
                     rdf(sp + 236), rdf(sp + 232), psp::host::g_aim.yaw, psp::host::g_aim.pitch);
    if (!psp::host::g_aim.enabled || (psp::host::g_aim.yaw == 0 && psp::host::g_aim.pitch == 0)) return;
    wrf(sp + 236, rdf(sp + 236) + psp::host::g_aim.yaw);
    wrf(sp + 232, rdf(sp + 232) - psp::host::g_aim.pitch);
    psp::host::g_aim.yaw = psp::host::g_aim.pitch = 0;
}

// Disc text patches applied while reading the ISO (IsoReader::addPatch). None yet.
namespace psp::kernel {
void installTextPatches(IsoReader&) {}
}
