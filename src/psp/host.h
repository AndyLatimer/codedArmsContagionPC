#pragma once
// State shared between the HLE layer (guest threads) and the native front end
// (window, input, renderer, audio). Plain atomics/mutex-protected data only.
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace psp::host {

// PSP controller buttons (SceCtrlData.Buttons).
enum : uint32_t {
    BTN_SELECT = 0x0001, BTN_START = 0x0008, BTN_UP = 0x0010, BTN_RIGHT = 0x0020, BTN_DOWN = 0x0040,
    BTN_LEFT = 0x0080, BTN_LTRIGGER = 0x0100, BTN_RTRIGGER = 0x0200, BTN_TRIANGLE = 0x1000,
    BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000, BTN_SQUARE = 0x8000,
};

struct Controller {
    std::atomic<uint32_t> buttons{0};
    std::atomic<uint8_t> lx{128}, ly{128};
};
extern Controller g_ctrl;

struct Display {
    std::atomic<uint32_t> topAddr{0}, bufferWidth{512}, pixelFormat{3};
    std::atomic<uint32_t> frameCount{0}; // incremented by sceDisplaySetFrameBuf
};
extern Display g_display;

// Audio: guest channels push 16-bit stereo samples; the host mixer consumes them.
struct AudioChannel {
    bool reserved = false;
    uint32_t sampleCount = 0, format = 0; // format 0x00 stereo, 0x10 mono
    int volLeft = 0x8000, volRight = 0x8000;
    std::vector<int16_t> queue;           // interleaved stereo, after volume
};
constexpr int kAudioChannels = 9; // 8 sceAudioCh channels and the sceAudioOutput2 (SRC) one
constexpr int kSrcChannel = 8;
extern AudioChannel g_audio[kAudioChannels];
extern std::mutex g_audioMutex;
// Set by a front end that drains the queues on the sound card's clock; blocking outputs then
// wait for it (and must be woken with kernel::notifyStateChanged after each drain).
extern std::atomic<bool> g_audioConsumer;
size_t audioQueuedFrames(int ch);

// Direct aim: radians added to the player's yaw/pitch by the game hooks (game_hooks.cpp).
// The front end adds motion with add() once per loop of its own (the display's rate); the game
// takes it with take() once per game frame. A plain hand-over gave the game frames 0 or 2
// loops' worth of motion whenever the two rates drifted apart: the camera moved in jumps while
// everything else was smooth. take() instead follows the motion as a timeline, `delayUs` behind
// real time, interpolated between the front end's samples, so each game frame gets the motion
// of exactly the time it covers.
struct Aim {
    std::mutex m;
    bool enabled = false;
    double totalYaw = 0, totalPitch = 0;   // everything added so far
    double takenYaw = 0, takenPitch = 0;   // what the game has taken
    struct Sample { uint64_t us; double yaw, pitch; };
    std::deque<Sample> samples;            // (time, totals) after each add
    uint64_t delayUs = 20000;              // a little more than one display refresh

    static uint64_t nowUs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // Caller holds m.
    void add(double dyaw, double dpitch) {
        enabled = true;
        const uint64_t now = nowUs();
        if (samples.empty()) samples.push_back({now - 1, totalYaw, totalPitch}); // motion starts now, not earlier
        totalYaw += dyaw;
        totalPitch += dpitch;
        samples.push_back({now, totalYaw, totalPitch});
        while (samples.size() > 64) samples.pop_front();
    }
    // Caller holds m. The motion due by now - delayUs (all of it once the samples are older).
    void take(double& dyaw, double& dpitch) {
        const uint64_t t = nowUs() - delayUs;
        double y = totalYaw, p = totalPitch;
        while (samples.size() >= 2 && samples[1].us <= t) samples.pop_front();
        if (!samples.empty() && samples.front().us > t) {
            y = takenYaw; // nothing due yet
            p = takenPitch;
        } else if (samples.size() >= 2) {
            const Sample& a = samples[0];
            const Sample& b = samples[1];
            const double f = b.us > a.us ? (double)(t - a.us) / (double)(b.us - a.us) : 1.0;
            y = a.yaw + (b.yaw - a.yaw) * f;
            p = a.pitch + (b.pitch - a.pitch) * f;
        } else if (samples.size() == 1) {
            samples.clear(); // all due: the next motion starts a new timeline
        }
        dyaw = y - takenYaw;
        dpitch = p - takenPitch;
        takenYaw = y;
        takenPitch = p;
    }
};
extern Aim g_aim;

// Aim down sights is held (front end); the renderer moves the weapon to the sights (gpu/ads.h).
extern std::atomic<bool> g_ads;

// View and aim down sights settings (front end controls.ini [ADS], graphics.ini, weapons.ini),
// read by the renderer every frame. Fields of view are horizontal, in degrees, for the PSP's
// 480x272 picture (the game's own is 80; wider windows add to the sides, Hor+).
struct AdsSettings {
    std::atomic<bool> enabled{true};
    std::atomic<float> transitionS{0.18f};   // hip <-> sights
    std::atomic<float> normalFov{80.0f};     // the level's view when not aiming
    std::atomic<float> adsFov{65.0f};        // and while aiming
    std::atomic<bool> hideCrosshair{true};   // while aiming
    std::atomic<float> weaponCloser{0.2f};   // share of its distance the weapon comes towards the eye
    std::atomic<float> sightX{0}, sightY{0}; // fine tuning of the sight point (weapon units, + = right/up)
    // 0: the weapon comes up to the eye; 1: zoom only (the weapon stays at the hip, the
    // crosshair stays: weapons held low, such as the Vulcan).
    std::atomic<int> mode{0};
    // Replace the measured alignment of the weapon (degrees; NaN: measured): its barrel's
    // turn up (pitch), to the left (yaw) and its cant (roll).
    std::atomic<float> pitchDeg{NAN}, yawDeg{NAN}, rollDeg{NAN};
};
extern AdsSettings g_adsSettings;
// The level camera's vertical projection scale (renderer): the game's scope zooms it.
extern std::atomic<float> g_levelProjY;

// The player's current weapon by the game's internal name (g01 = pistol), up to 4 characters
// packed little-endian; 0 when there is none (game_hooks.cpp, read every player update).
extern std::atomic<uint32_t> g_weaponName;
// Player updates so far (game_hooks.cpp): it stops while the game is paused or in its menus.
extern std::atomic<uint32_t> g_playerUpdates;

// Developer Lua console (game_hooks.cpp): chunks of Lua source run in the game's own Lua state
// the next time the game calls into Lua; each result (the chunk's return value, or the error)
// is written to the log as [LUA]. The front end queues dev.lua with F9; ca_psprun: lua FILE.
struct LuaConsole {
    std::mutex m;
    std::deque<std::string> pending;
};
extern LuaConsole g_luaConsole;

// Dev mode (front end [dev], game_hooks.cpp): requests to put the next (+1) or previous (-1)
// of the game's guns in the fourth weapon slot (which takes any of them), with endless ammo,
// through the game's own Lua (the weapon HUD's LoadWpn).
struct DevMode {
    std::atomic<bool> enabled{false};
    std::atomic<int> cycle{0};
};
extern DevMode g_dev;

// A request to switch to the weapon in slot 1-5 (number keys; 0: none), through the game's Lua
// (the weapon HUD's SetWeapon, as when the game changes weapon itself; game_hooks.cpp).
extern std::atomic<int> g_selectSlot;

// The game's OPTIONS screen (game_hooks.cpp): its selected row (0-4; 3 = the PC settings) and
// when it was last updated (steady clock, microseconds), so the front end knows it is showing.
struct GameOptions {
    std::atomic<int> row{-1};
    std::atomic<uint64_t> seenUs{0};
};
extern GameOptions g_gameOptions;

// Frame rate cap: 30, or 60 / 0 for the game's own maximum (60, one update per vblank; its
// logic follows the frame time, so the game's speed does not change). hle_devices.cpp.
extern std::atomic<int> g_targetFps;

} // namespace psp::host
