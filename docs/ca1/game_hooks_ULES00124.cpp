// Game-specific hooks called by the recompiled code (tools/recomp/hooks_ULES00124.tsv).
//
// Mouse aim: once per frame Coded Arms updates the player's view with the function at
// 0x0889ba9c(view object, input, ...). It turns the view by integrating the turn/look inputs
// (input+0x78 / +0x79) into the yaw at view+0x51C and the pitch at view+0x524 (radians), clamps
// the pitch to +-1.309 rad and normalises the yaw (TECHNICAL_NOTES.md section 13). The hook at
// its entry adds the mouse motion accumulated by the front end to those angles first.
//
// Frame rate: the game renders a frame every N vblanks. 0x08879ff8(settings, N) is its frame
// interval setter: it stores N at settings+16, 60/N at +12 and (float)(60/N) at +8 (the block
// pointed to by 0x09bd2004). Missions use N = 3 (20 fps), menus N = 2. The hook lowers N when
// the setter runs (60 fps: 1, 30 fps: 2).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
constexpr float kPitchLimit = 1.3090000f; // 0x3FA78D37, the game's own clamp
constexpr float kPi = 3.14159265f;
}

uint32_t frameSettingsForTrace();

namespace {
void devTick(Ctx* c);
}

extern "C" void ca_hook_player_view(Ctx* c) {
    devTick(c);
    const uint32_t view = c->r[19], input = c->r[18];
    static const bool trace = std::getenv("CA_HOOK_TRACE") != nullptr;
    static const bool perf = std::getenv("CA_PERF") != nullptr;
    if (perf) {
        static uint32_t calls = 0;
        static auto last = std::chrono::steady_clock::now();
        ++calls;
        auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            std::fprintf(stderr, "[PERF] game view updates/s=%u\n", calls);
            calls = 0;
            last = now;
        }
    }
    if (trace)
        std::fprintf(stderr, "[HOOK] view=%08x input=%08x N=%d yaw=%f pitch=%f pos=%f,%f t=%lld\n", view, input,
                     frameSettingsForTrace() ? (int)(int16_t)psp::read16(frameSettingsForTrace() + 16) : -1, rdf(view + 0x51C),
                     rdf(view + 0x524), rdf(view), rdf(view + 8),
                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> lk(psp::host::g_aim.m);
    if (!psp::host::g_aim.enabled || (psp::host::g_aim.yaw == 0 && psp::host::g_aim.pitch == 0)) return;
    float yaw = rdf(view + 0x51C) - psp::host::g_aim.yaw; // mouse right turns right: the yaw decreases
    float pitch = rdf(view + 0x524) + psp::host::g_aim.pitch; // mouse up looks up: the pitch decreases
    yaw = std::remainder(yaw, 2 * kPi);
    pitch = std::fmax(-kPitchLimit, std::fmin(kPitchLimit, pitch));
    wrf(view + 0x51C, yaw);
    wrf(view + 0x524, pitch);
    psp::host::g_aim.yaw = psp::host::g_aim.pitch = 0;
}

// Frame rate --------------------------------------------------------------------------------
namespace {
struct FrameRate {
    uint32_t settings = 0; // the block the setter wrote
    int requested = 0;     // the game's own interval for the current screen
    int effective = 0;     // the interval in use
} g_fr;
} // namespace
uint32_t frameSettingsForTrace() { return g_fr.settings; }

extern "C" void ca_hook_frame_interval(Ctx* c) {
    // At 0x08879ff8 (sll v1, a1, 16), the entry of SetFrameInterval(settings, interval). The
    // interval may only change here: the game derives per-frame steps (e.g. the player's
    // movement) from it when a screen starts, so changing it afterwards changes the game's
    // speed (TECHNICAL_NOTES.md section 15).
    g_fr.settings = c->r[4];
    g_fr.requested = (int16_t)c->r[5];
    const int fps = psp::host::g_targetFps.load();
    const int n = fps >= 60 ? 1 : (fps >= 30 ? 2 : 0);
    g_fr.effective = g_fr.requested;
    if (n && g_fr.requested > n) {
        c->r[5] = (uint32_t)n;
        c->r[3] = (uint32_t)n << 16;
        g_fr.effective = n;
    }
}

// The main loop's frame wait (0x08837f70): entry = the frame's work is done, exit = the next
// frame starts with a0 = the vblanks the last frame took (the counter at s0+4).
extern "C" void ca_hook_frame_work_done(Ctx*) {}

extern "C" void ca_hook_frame_start(Ctx* c) {
    static const bool perf = std::getenv("CA_PERF") != nullptr;
    if (!perf) return;
    static auto last = std::chrono::steady_clock::now();
    static int frames = 0, late = 0;
    ++frames;
    late += (int)c->r[4] > g_fr.effective && g_fr.effective > 0;
    auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1)) {
        std::fprintf(stderr, "[PERF] game frames %d (late %d), interval: game %d, in use %d\n", frames, late, g_fr.requested,
                     g_fr.effective);
        frames = late = 0;
        last = now;
    }
}

// Dev mode -----------------------------------------------------------------------------------
// The player's weapons are built from the profile's inventory (TECHNICAL_NOTES.md section 21):
// 52 entries of 3 bytes (item, equip slot 1-5 or 0, ?) at profile+0, entry i holding item i
// (1-23 weapons, 24-33 grenades), the count of each item at profile+268+item, the weapon
// objects at *(profile+372)[i], the number equipped (at most 5) at profile+496, the current
// weapon at profile+156 and a requested switch at profile+500. The ammo in reserve of weapon
// id i (item i+1) is a u32 at 0x08928c94() + 4*i (0x09bf4124 in the story mode).
namespace {
// Runs guest function `fn` on the calling guest thread, below its stack, and returns v0.
uint32_t callGuest(Ctx* c, uint32_t fn, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
    Ctx sub = *c;
    sub.r[4] = a0;
    sub.r[5] = a1;
    sub.r[6] = a2;
    sub.r[7] = a3;
    sub.r[29] = (c->r[29] - 0x40) & ~0xFu;
    call_indirect(&sub, fn);
    return sub.r[2];
}

bool inRam(uint32_t a) { return a >= 0x08800000 && a < 0x0A000000; }

uint32_t g_devProfile = 0;            // the stocked profile (0: none yet)
uint8_t g_devGrenades[34] = {};       // the grenade counts it was given

// 0x08927620(profile, ...) sets the player's weapons up when a level starts (and 0x08927498
// creates an object for each equipped weapon). The hook at its entry first puts every weapon
// and grenade in the inventory with 0x08926bd4(profile, item), the game's own "add to
// inventory" (used when an item is picked up), and equips the dev loadout.
void devStock(Ctx* c, uint32_t profile) {
    auto& d = psp::host::g_dev;
    for (uint32_t item = 1; item < 34; ++item) callGuest(c, 0x08926bd4, profile, item);
    for (uint32_t item = 1; item < 34; ++item) psp::write8(profile + 3 * item + 1, 0);
    for (int k = 0; k < 5; ++k) {
        int id = d.loadout[k];
        if (id >= 0 && id < 33) psp::write8(profile + 3 * (id + 1) + 1, (uint8_t)(k + 1));
    }
    for (uint32_t item = 24; item < 34; ++item) g_devGrenades[item] = psp::read8(profile + 268 + item);
    g_devProfile = profile;
    if (std::getenv("CA_DEV_TRACE")) {
        std::fprintf(stderr, "[DEV] profile %08x stocked:", profile);
        for (uint32_t item = 1; item < 34; ++item)
            std::fprintf(stderr, " %d:%d/%d", (int8_t)psp::read8(profile + 3 * item), (int8_t)psp::read8(profile + 3 * item + 1),
                         (int8_t)psp::read8(profile + 268 + item));
        std::fprintf(stderr, "\n");
    }
}

// Puts the next (dir 1) or previous (-1) weapon the player has not equipped in the place of the
// current one, and asks the game to switch to it, as when a weapon is picked up (0x088a486c).
void devCycle(Ctx* c, uint32_t player, uint32_t p, int dir) {
    const int cur = (int16_t)psp::read16(p + 156);
    if (cur < 1 || cur > 23 || (int16_t)psp::read16(p + 500) >= 0 || psp::read8(p + 502) != 0) return;
    const uint32_t objs = psp::read32(p + 372);
    if (!inRam(objs)) return;
    int n = cur;
    do n = (n - 1 + dir + 23) % 23 + 1;
    while (n != cur && (psp::read8(p + 3 * n) != n || psp::read8(p + 3 * n + 1) != 0));
    if (n == cur) return;
    const uint8_t slot = psp::read8(p + 3 * cur + 1);
    psp::write8(p + 3 * cur + 1, 0);
    psp::write16(p + 496, (uint16_t)((int16_t)psp::read16(p + 496) - 1));
    if (psp::read32(objs + 4 * n)) { // equipped before: its object is still there
        psp::write16(p + 496, (uint16_t)((int16_t)psp::read16(p + 496) + 1));
    } else if ((int32_t)callGuest(c, 0x08926a04, p, (uint32_t)n, 0, psp::read8(p + 504)) < 0) {
        psp::write8(p + 3 * cur + 1, slot); // could not equip it: undo
        psp::write16(p + 496, (uint16_t)((int16_t)psp::read16(p + 496) + 1));
        return;
    }
    psp::write8(p + 3 * n + 1, slot);
    psp::write16(p + 500, (uint16_t)n); // as 0x08926118
    psp::write8(player + 1418, 0);
    if (uint32_t hud = callGuest(c, 0x089048a8, 0)) callGuest(c, 0x08900df0, hud); // the HUD's weapon list
    if (std::getenv("CA_DEV_TRACE")) std::fprintf(stderr, "[DEV] weapon %d -> %d (slot %d)\n", cur - 1, n - 1, slot);
}

// Once per frame on the player's thread: weapon changes and ammo.
void devTick(Ctx* c) {
    auto& d = psp::host::g_dev;
    const uint32_t p = g_devProfile;
    if (!d.enabled || !p) return;
    // The player object (the view update's s3) holds its profile at +1244.
    const uint32_t player = c->r[19];
    if (int dir = d.cycle.exchange(0); dir && psp::read32(player + 1244) == p) devCycle(c, player, p, dir > 0 ? 1 : -1);
    if (!d.infiniteAmmo) return;
    const uint32_t reserve = callGuest(c, 0x08928c94, 0);
    if (inRam(reserve) && inRam(reserve + 4 * 23))
        for (uint32_t i = 0; i < 23; ++i)
            if (psp::read32(reserve + 4 * i) < 300) psp::write32(reserve + 4 * i, 300);
    for (uint32_t item = 24; item < 34; ++item)
        if (psp::read8(p + 3 * item) == item && psp::read8(p + 268 + item) < g_devGrenades[item])
            psp::write8(p + 268 + item, g_devGrenades[item]);
}
} // namespace

extern "C" void ca_hook_dev_player_weapons(Ctx* c) { // after 0x08927620
    if (psp::host::g_dev.enabled) devStock(c, c->r[4]);
}

// Options menu ---------------------------------------------------------------------------------
// The game's OPTIONS screen (terminal) has five rows; the fourth, ACCEPT CHALLENGE, is for the
// ad hoc multiplayer, which the port does not have. It becomes the PC settings: its label and
// help text are changed in the disc's message tables as they are read (one table per language,
// length-prefixed strings; the new text is padded with NULs to the same length), and the
// screen's update function (0x08964e1c, a0 = the menu; selected row at +158, s16) tells the
// front end which row is selected, which opens the PC settings for that row
// (TECHNICAL_NOTES.md section 22).
namespace {
struct TextPatch {
    uint64_t pos;       // the string in the disc image
    uint32_t size;      // its length with the NUL (from the table's length prefix)
    uint64_t hash;      // FNV-1a of its original bytes, checked before patching
    const char* text;   // in the game's encoding (accents as two bytes, e.g. 0x83 0x60 = o acute)
};
const TextPatch kTextPatches[] = {
    {74969841, 17, 0x0aea366d052ef76cull, "PC SETUP"},
    {74970029, 73, 0x3e122ae438a7ec7aull, "PC settings: video, mouse, gamepad and controls."},
    {75024509, 9, 0x54fbce7a2887d089ull, "PC-SETUP"},
    {75024720, 84, 0xe7b66ab321dceeb2ull, "PC-Einstellungen: Grafik, Maus, Controller und Tasten."},
    {75081222, 17, 0xdd6a08592a0d2716ull, "AJUSTES PC"},
    {75081487, 72, 0xc1a62b7d7a6d94a9ull, "Ajustes del PC: v\203Vdeo, rat\203`n, mando y controles."},
    {75137173, 15, 0xf401d404819bbb36ull, "R\202\255GLAGES PC"},
    {75137392, 65, 0xcc5a33964d15512aull, "R\203Nglages PC : vid\203No, souris, manette, touches."},
    {75192366, 14, 0xeea8d180b445f91dull, "IMPOSTAZ. PC"},
    {75192634, 82, 0x4c7e6c4c72c30a0eull, "Impostazioni PC: video, mouse, controller e tasti."},
};
} // namespace

namespace psp::kernel {
// Called when the disc is opened (kernel.cpp).
void installTextPatches(IsoReader& iso) {
    int applied = 0;
    for (const TextPatch& p : kTextPatches) {
        std::string bytes(p.text);
        if (bytes.size() >= p.size) continue;
        bytes.resize(p.size - 1, '\0');
        applied += iso.addPatch(p.pos, p.size, p.hash, bytes);
    }
    std::fprintf(stderr, "[PATCH] %d of %zu menu texts\n", applied, sizeof kTextPatches / sizeof kTextPatches[0]);
}
} // namespace psp::kernel

extern "C" void ca_hook_options_menu(Ctx* c) { // after 0x08964e1c (a0 = the OPTIONS menu)
    auto& o = psp::host::g_gameOptions;
    const int row = (int16_t)psp::read16(c->r[4] + 158);
    static const bool trace = std::getenv("CA_HOOK_TRACE") != nullptr;
    if (trace && row != o.row) std::fprintf(stderr, "[HOOK] options menu %08x row %d\n", c->r[4], row);
    o.row = row;
    o.seenUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
