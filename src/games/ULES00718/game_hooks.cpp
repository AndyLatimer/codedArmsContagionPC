// Game-specific hooks for Coded Arms: Contagion (ULES-00718), called by the recompiled code
// (tools/recomp/games/ULES00718/hooks.tsv).
//
// The front end talks to the game through the state in psp/host.h (mouse aim, aim down sights,
// frame rate, dev mode, OPTIONS row). In Coded Arms (ULES-00124) each of those is wired to the
// game by a hook at an address found with the store watchpoint build; see
// docs/ca1/game_hooks_ULES00124.cpp. Contagion is a different engine, so none of those
// addresses apply: until its view update and frame interval setter are located the state is
// kept but not applied.

#include "psp/host.h"
#include "psp/iso.h"

namespace psp::host {
Aim g_aim;
std::atomic<bool> g_ads{false};
DevMode g_dev;
GameOptions g_gameOptions;
std::atomic<int> g_targetFps{0};
}

// Disc text patches applied while reading the ISO (IsoReader::addPatch). None yet.
namespace psp::kernel {
void installTextPatches(IsoReader&) {}
}
