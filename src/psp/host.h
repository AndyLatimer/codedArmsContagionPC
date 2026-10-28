#pragma once
// State shared between the HLE layer (guest threads) and the native front end
// (window, input, renderer, audio). Plain atomics/mutex-protected data only.
#include <atomic>
#include <cstdint>
#include <mutex>
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

// Direct aim: radians added to the player's yaw/pitch by the game hooks (game_hooks.cpp) the
// next time the game updates the camera. The front end accumulates mouse motion here.
struct Aim {
    std::mutex m;
    float yaw = 0, pitch = 0;
    bool enabled = false;
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
};
extern AdsSettings g_adsSettings;

// The player's current weapon by the game's internal name (g01 = pistol), up to 4 characters
// packed little-endian; 0 when there is none (game_hooks.cpp, read every player update).
extern std::atomic<uint32_t> g_weaponName;

// Dev mode (front end [dev], game_hooks.cpp): every weapon and grenade, the five the player
// starts levels with (weapon ids 0-22, grenades 23-32, -1 empty), endless ammo, and requests to
// put the next (+1) or previous (-1) weapon in the place of the current one.
struct DevMode {
    std::atomic<bool> enabled{false};
    std::atomic<int> loadout[5] = {0, 4, 6, 19, 23};
    std::atomic<bool> infiniteAmmo{true};
    std::atomic<int> cycle{0};
};
extern DevMode g_dev;

// The game's OPTIONS screen (game_hooks.cpp): its selected row (0-4; 3 = the PC settings) and
// when it was last updated (steady clock, microseconds), so the front end knows it is showing.
struct GameOptions {
    std::atomic<int> row{-1};
    std::atomic<uint64_t> seenUs{0};
};
extern GameOptions g_gameOptions;

// Frame rate the game is run at: 60 or 30; 0 keeps the game's own (20 fps in missions).
// Applied when the game sets its frame interval (game_hooks.cpp).
extern std::atomic<int> g_targetFps;

} // namespace psp::host
