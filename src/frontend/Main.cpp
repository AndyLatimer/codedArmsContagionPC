// Coded Arms: Contagion (PC) - native front end for the statically recompiled game.
//
// The recompiled game code runs on its own threads inside the HLE runtime (src/psp); this
// file owns the window, the OpenGL context (the GE renderer executes here), keyboard/mouse/
// gamepad input mapped onto the PSP controller, audio output and the user settings
// (config.ini next to the executable or in the user's preferences folder).
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/Config.h"
#include "frontend/CrashLog.h"
#include "frontend/DialogOverlay.h"
#include "frontend/OptionsMenu.h"
#include "platform/Input.h"
#include "psp/gpu/button_glyphs.h"
#include "psp/gpu/gpu.h"
#include "psp/gpu/minimap.h"
#include "psp/host.h"
#include "psp/host_dialog.h"
#include "psp/kernel.h"
#include "render/GL.h"

namespace psp::kernel {
extern std::string g_hostMsRoot; // hle_io.cpp
}

namespace {

// ---- settings -----------------------------------------------------------------------------

enum PspInput {
    IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L, IN_R, IN_START, IN_SELECT,
    IN_STICK_UP, IN_STICK_DOWN, IN_STICK_LEFT, IN_STICK_RIGHT,
    IN_ADS,   // aim down sights (the port's own feature, not a PSP button)
    IN_RUN,   // keyboard movement at full stick (the game runs with the stick pushed all the way)
    IN_CREEP, // keyboard movement at a slight push (slow, quiet walk; the game has no crouch)
    IN_COUNT
};
const char* kInputNames[IN_COUNT] = {"up", "down", "left", "right", "cross", "circle", "square", "triangle",
                                     "l", "r", "start", "select", "stick_up", "stick_down", "stick_left", "stick_right",
                                     "aim_down_sights", "run", "creep"};
const uint32_t kInputButtons[12] = {
    psp::host::BTN_UP, psp::host::BTN_DOWN, psp::host::BTN_LEFT, psp::host::BTN_RIGHT, psp::host::BTN_CROSS,
    psp::host::BTN_CIRCLE, psp::host::BTN_SQUARE, psp::host::BTN_TRIANGLE, psp::host::BTN_LTRIGGER,
    psp::host::BTN_RTRIGGER, psp::host::BTN_START, psp::host::BTN_SELECT};
// Defaults follow the game's control scheme A (its default, Settings > Controls): analog = move
// and strafe, triangle/cross/square/circle = look up/down/left/right (the mouse and the right
// stick aim directly instead, game_hooks.cpp), up = toggle scope, down = toggle lock-on, left =
// change weapon, right = reload, L = jump, R = fire, select = toggle visor, start = pause. In the
// menus cross confirms and triangle goes back (circle in the system dialogs, so Backspace is
// both). The right mouse button / left trigger aim down sights.
const char* kDefaultBindings[IN_COUNT] = {
    "key:Up, mouse:middle, key:Z, pad:dpup", "key:Down, key:Q, pad:dpdown",
    "key:Left, mouse:wheelup, mouse:wheeldown, key:C, pad:dpleft", "key:Right, key:R, pad:dpright",
    "key:Return, pad:a", "key:Backspace, pad:b", "pad:x", "key:Backspace, pad:y", "key:Space, pad:leftshoulder",
    "mouse:left, pad:rightshoulder, pad:righttrigger", "key:Escape, key:P, pad:start", "key:Tab, key:V, pad:back",
    "key:W", "key:S", "key:A", "key:D", "mouse:right, pad:lefttrigger", "key:Left Shift", "key:Left Ctrl"};

struct FrontSettings {
    std::string iso;
    int width = 1280, height = 720;
    int fullscreen = 0;     // 0 windowed, 1 borderless desktop, 2 exclusive
    bool vsync = true;
    int renderScale = 0;    // 0 = from the window height
    bool linearOutput = true;
    bool forceLinearTextures = false;
    bool stretch = false;
    int widescreen = 1;     // 1: widen the view to the window's aspect (Hor+); 0: original 480x272
    bool pcButtonHints = true; // PC keys in place of the PSP button symbols
    bool minimap = false;      // the port's minimap (work in progress, off by default; N toggles it)
    float minimapSize = 0.30f; // fraction of the picture's height
    float minimapRange = 160;  // world units from the centre to the edge
    int minimapCorner = 0;     // 0 bottom left (above the health bar), 1 top right (below the game's radar)
    int frameRate = 60;     // 60 or 30 (fixed per screen); 0 = the game's own (20 fps in missions)
    bool dev = false;       // dev mode (testing): every weapon, endless ammo, F5/F6 change the weapon
    std::string devLoadout = "0, 4, 6, 19, 23"; // the five weapons levels start with (ids, see LEEME)
    float volume = 1.0f;
    float deadzone = 0.2f;
    float walkSpeed = 0.7f, creepSpeed = 0.45f; // stick push of the movement keys (run = 1)
    // Mouse look (until the game's camera is driven directly: mapped onto the look buttons).
    float mouseSensX = 1.0f, mouseSensY = 1.0f;
    bool invertY = false;
    std::string lookUp = "triangle", lookDown = "cross", lookLeft = "square", lookRight = "circle";
    bool mouseLook = true;
    float adsSens = 0.6f;    // look sensitivity while aiming down sights
    bool adsToggle = false;  // aim down sights: hold (false) or toggle (true)
    int mouseLookMode = 1;   // 0 off, 1 direct aim, 2 look buttons
    float deadzoneRight = 0.15f, padCurve = 1.6f, padTurnRate = 3.0f; // rad/s at full deflection
    float padSensX = 1.0f, padSensY = 0.8f;
    bool padInvertX = false, padInvertY = false;
    bool invertX = false;
    std::vector<ca::Binding> bindings[IN_COUNT];
    std::string bindingText[IN_COUNT];
};

float getF(const ca::Ini& ini, const char* s, const char* k, float d) {
    std::string v = ini.get(s, k, "");
    return v.empty() ? d : (float)std::atof(v.c_str());
}

void parseList(const std::string& text, std::vector<ca::Binding>& out) {
    out.clear();
    size_t p = 0;
    while (p <= text.size()) {
        size_t c = text.find(',', p);
        std::string part = text.substr(p, c == std::string::npos ? std::string::npos : c - p);
        ca::Binding b;
        if (ca::parseBinding(part, b)) out.push_back(b);
        if (c == std::string::npos) break;
        p = c + 1;
    }
}

void loadSettings(FrontSettings& s, ca::Ini& ini, const std::string& path) {
    ini.load(path);
    s.iso = ini.get("game", "iso", s.iso);
    s.width = (int)getF(ini, "video", "width", (float)s.width);
    s.height = (int)getF(ini, "video", "height", (float)s.height);
    s.fullscreen = (int)getF(ini, "video", "fullscreen", (float)s.fullscreen);
    s.vsync = getF(ini, "video", "vsync", 1) != 0;
    s.renderScale = (int)getF(ini, "video", "render_scale", 0);
    s.linearOutput = getF(ini, "video", "linear_output", 1) != 0;
    s.forceLinearTextures = getF(ini, "video", "force_linear_textures", 0) != 0;
    s.stretch = getF(ini, "video", "stretch", 0) != 0;
    s.widescreen = (int)getF(ini, "video", "widescreen", 1);
    s.frameRate = (int)getF(ini, "video", "frame_rate", 60);
    s.pcButtonHints = getF(ini, "video", "pc_button_hints", 1) != 0;
    s.minimap = getF(ini, "video", "minimap", 0) != 0;
    s.minimapSize = std::clamp(getF(ini, "video", "minimap_size", 0.30f), 0.1f, 0.6f);
    s.minimapRange = std::clamp(getF(ini, "video", "minimap_range", 160.0f), 40.0f, 1000.0f);
    s.minimapCorner = (int)getF(ini, "video", "minimap_corner", 0) == 1 ? 1 : 0;
    s.dev = getF(ini, "dev", "enabled", 0) != 0;
    s.devLoadout = ini.get("dev", "loadout", s.devLoadout);
    s.volume = getF(ini, "audio", "volume", 1.0f);
    s.deadzone = getF(ini, "input", "deadzone", 0.2f);
    s.walkSpeed = std::clamp(getF(ini, "input", "walk_speed", 0.7f), 0.35f, 1.0f);
    s.creepSpeed = std::clamp(getF(ini, "input", "creep_speed", 0.45f), 0.35f, 1.0f);
    s.mouseLookMode = (int)getF(ini, "input", "mouse_look", 1);
    s.mouseLook = s.mouseLookMode != 0;
    s.invertX = getF(ini, "input", "invert_x", 0) != 0;
    s.deadzoneRight = getF(ini, "input", "deadzone_right", 0.15f);
    s.padCurve = getF(ini, "input", "pad_look_curve", 1.6f);
    s.padTurnRate = getF(ini, "input", "pad_turn_rate", 3.0f);
    s.padSensX = getF(ini, "input", "pad_sens_x", 1.0f);
    s.padSensY = getF(ini, "input", "pad_sens_y", 0.8f);
    s.padInvertX = getF(ini, "input", "pad_invert_x", 0) != 0;
    s.padInvertY = getF(ini, "input", "pad_invert_y", 0) != 0;
    s.mouseSensX = getF(ini, "input", "mouse_sens_x", 1.0f);
    s.mouseSensY = getF(ini, "input", "mouse_sens_y", 1.0f);
    s.adsSens = std::clamp(getF(ini, "input", "ads_sensitivity", 0.6f), 0.1f, 2.0f);
    s.adsToggle = getF(ini, "input", "ads_toggle", 0) != 0;
    s.invertY = getF(ini, "input", "invert_y", 0) != 0;
    for (int i = 0; i < IN_COUNT; ++i) {
        s.bindingText[i] = ini.get("bindings", kInputNames[i], kDefaultBindings[i]);
        parseList(s.bindingText[i], s.bindings[i]);
    }
}

void saveSettings(const FrontSettings& s, ca::Ini& ini, const std::string& path) {
    ini.set("game", "iso", s.iso);
    ini.set("video", "width", std::to_string(s.width));
    ini.set("video", "height", std::to_string(s.height));
    ini.set("video", "fullscreen", std::to_string(s.fullscreen));
    ini.set("video", "vsync", s.vsync ? "1" : "0");
    ini.set("video", "render_scale", std::to_string(s.renderScale));
    ini.set("video", "linear_output", s.linearOutput ? "1" : "0");
    ini.set("video", "force_linear_textures", s.forceLinearTextures ? "1" : "0");
    ini.set("video", "stretch", s.stretch ? "1" : "0");
    ini.set("video", "widescreen", std::to_string(s.widescreen));
    ini.set("video", "frame_rate", std::to_string(s.frameRate));
    ini.set("video", "pc_button_hints", s.pcButtonHints ? "1" : "0");
    ini.set("video", "minimap", s.minimap ? "1" : "0");
    ini.set("video", "minimap_size", std::to_string(s.minimapSize));
    ini.set("video", "minimap_range", std::to_string(s.minimapRange));
    ini.set("video", "minimap_corner", std::to_string(s.minimapCorner));
    ini.set("dev", "enabled", s.dev ? "1" : "0");
    ini.set("dev", "loadout", s.devLoadout);
    ini.set("audio", "volume", std::to_string(s.volume));
    ini.set("input", "deadzone", std::to_string(s.deadzone));
    ini.set("input", "walk_speed", std::to_string(s.walkSpeed));
    ini.set("input", "creep_speed", std::to_string(s.creepSpeed));
    ini.set("input", "mouse_look", std::to_string(s.mouseLookMode));
    ini.set("input", "ads_sensitivity", std::to_string(s.adsSens));
    ini.set("input", "ads_toggle", s.adsToggle ? "1" : "0");
    ini.set("input", "invert_x", s.invertX ? "1" : "0");
    ini.set("input", "deadzone_right", std::to_string(s.deadzoneRight));
    ini.set("input", "pad_look_curve", std::to_string(s.padCurve));
    ini.set("input", "pad_turn_rate", std::to_string(s.padTurnRate));
    ini.set("input", "pad_sens_x", std::to_string(s.padSensX));
    ini.set("input", "pad_sens_y", std::to_string(s.padSensY));
    ini.set("input", "pad_invert_x", s.padInvertX ? "1" : "0");
    ini.set("input", "pad_invert_y", s.padInvertY ? "1" : "0");
    ini.set("input", "mouse_sens_x", std::to_string(s.mouseSensX));
    ini.set("input", "mouse_sens_y", std::to_string(s.mouseSensY));
    ini.set("input", "invert_y", s.invertY ? "1" : "0");
    for (int i = 0; i < IN_COUNT; ++i) ini.set("bindings", kInputNames[i], s.bindingText[i]);
    ini.save(path);
}

// ---- input --------------------------------------------------------------------------------

struct InputState {
    SDL_GameController* pad = nullptr;
    float lookAccumX = 0, lookAccumY = 0;
    bool adsOn = false, adsWasHeld = false;
    // Mouse buttons from SDL events (more reliable than polling in relative mode); `mouseClicked`
    // keeps presses released before the next frame so short clicks are not lost.
    Uint32 mouseHeld = 0, mouseClicked = 0;
    // Keys pressed since the last frame, so a press released within one frame is not lost.
    bool keyClicked[SDL_NUM_SCANCODES] = {};
    bool wheelUp = false, wheelDown = false;
    bool waitRelease = false; // after a menu closes: no input to the game until all are released
    bool confirmWasHeld = false;
    bool openPcSettings = false; // the PC SETUP row of the game's OPTIONS was confirmed
};

bool bindingHeld(const ca::Binding& b, InputState& in, const Uint8* keys, Uint32 mouse) {
    using T = ca::Binding::Type;
    switch (b.type) {
    case T::Key: return keys[b.code] != 0 || in.keyClicked[b.code];
    case T::MouseButton: return (mouse & SDL_BUTTON(b.code)) != 0;
    case T::MouseWheelUp: return in.wheelUp;
    case T::MouseWheelDown: return in.wheelDown;
    case T::PadButton: return in.pad && SDL_GameControllerGetButton(in.pad, (SDL_GameControllerButton)b.code);
    case T::PadTrigger: return in.pad && SDL_GameControllerGetAxis(in.pad, (SDL_GameControllerAxis)b.code) > 12000;
    }
    return false;
}

int inputIndex(const std::string& name) {
    for (int i = 0; i < 12; ++i) if (name == kInputNames[i]) return i;
    return -1;
}

// Mouse counts to radians at sensitivity 1 (about 0.13 degrees per count).
constexpr float kRadiansPerCount = 0.0022f;

void updateController(const FrontSettings& s, InputState& in, bool focused, float dt) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    Uint32 mouse = focused ? (in.mouseHeld | in.mouseClicked) : 0;
    in.mouseClicked = 0;
    struct ClearKeys { InputState& in; ~ClearKeys() { std::fill(std::begin(in.keyClicked), std::end(in.keyClicked), false); } } clearKeys{in};
    uint32_t buttons = 0;
    bool held[IN_COUNT] = {};
    for (int i = 0; i < IN_COUNT; ++i)
        for (const auto& b : s.bindings[i])
            if (bindingHeld(b, in, keys, mouse)) held[i] = true;
    if (in.waitRelease) {
        if (std::any_of(std::begin(held), std::end(held), [](bool h) { return h; })) {
            psp::host::g_ctrl.buttons = 0;
            psp::host::g_ctrl.lx = psp::host::g_ctrl.ly = 128;
            psp::host::g_ads = false;
            in.wheelUp = in.wheelDown = false;
            in.lookAccumX = in.lookAccumY = 0;
            return;
        }
        in.waitRelease = false;
    }
    // The game's OPTIONS screen with its PC SETUP row selected (the old ACCEPT CHALLENGE,
    // game_hooks.cpp): confirm opens the PC settings; the game does not see confirm or
    // left/right there, so the multiplayer option stays off.
    const uint64_t nowUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool onPcRow = psp::host::g_gameOptions.row == 3 && nowUs - psp::host::g_gameOptions.seenUs < 250000;
    const bool confirm = held[IN_CROSS];
    if (onPcRow) {
        if (confirm && !in.confirmWasHeld) in.openPcSettings = true;
        for (int k : {IN_CROSS, IN_LEFT, IN_RIGHT, IN_SQUARE, IN_STICK_LEFT, IN_STICK_RIGHT}) held[k] = false;
    }
    in.confirmWasHeld = confirm;
    for (int i = 0; i < 12; ++i) if (held[i]) buttons |= kInputButtons[i];

    // Aim down sights (drawn by the renderer, gpu/ads.h); slower look while aiming.
    if (s.adsToggle) {
        if (held[IN_ADS] && !in.adsWasHeld) in.adsOn = !in.adsOn;
    } else {
        in.adsOn = held[IN_ADS];
    }
    in.adsWasHeld = held[IN_ADS];
    if (!focused) in.adsOn = false;
    psp::host::g_ads = in.adsOn;
    const float lookScale = in.adsOn ? s.adsSens : 1.0f;

    // Analog stick: gamepad left stick, or the digital stick bindings.
    float ax = 0, ay = 0;
    if (in.pad) {
        ax = SDL_GameControllerGetAxis(in.pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        ay = SDL_GameControllerGetAxis(in.pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        ca::Vec2 v = ca::applyRadialDeadzone(ax, ay, s.deadzone);
        ax = v.x;
        ay = v.y;
        // Right stick: direct aim at a turn rate (rad/s) with a radial dead zone and a response
        // curve, scaled by the frame time.
        float rx = SDL_GameControllerGetAxis(in.pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        float ry = SDL_GameControllerGetAxis(in.pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        ca::Vec2 r = ca::applyRadialDeadzone(rx, ry, s.deadzoneRight);
        float mag = std::sqrt(r.x * r.x + r.y * r.y);
        if (mag > 0) {
            float k = std::pow(mag, s.padCurve) / mag;
            std::lock_guard<std::mutex> lk(psp::host::g_aim.m);
            psp::host::g_aim.enabled = true;
            psp::host::g_aim.yaw += r.x * k * s.padTurnRate * s.padSensX * lookScale * dt * (s.padInvertX ? -1.0f : 1.0f);
            psp::host::g_aim.pitch += r.y * k * s.padTurnRate * s.padSensY * lookScale * dt * (s.padInvertY ? -1.0f : 1.0f);
        }
    }
    // Keys move at a walk; run pushes the stick all the way (the game's top speed), creep just
    // past the game's dead zone (about 0.3).
    const float keyStick = held[IN_CREEP] ? s.creepSpeed : (held[IN_RUN] ? 1.0f : s.walkSpeed);
    if (held[IN_STICK_LEFT]) ax = -keyStick;
    if (held[IN_STICK_RIGHT]) ax = keyStick;
    if (held[IN_STICK_UP]) ay = -keyStick;
    if (held[IN_STICK_DOWN]) ay = keyStick;
    if (onPcRow) ax = 0;
    if (ax != 0 && ay != 0 && (held[IN_STICK_LEFT] || held[IN_STICK_RIGHT]) && (held[IN_STICK_UP] || held[IN_STICK_DOWN])) {
        ax *= 0.7071f;
        ay *= 0.7071f;
    }

    // Mouse: direct aim (default) adds the motion to the game's view angles (game_hooks.cpp);
    // mouse_look = 2 instead spends it as presses of the look buttons like the original pad.
    if (s.mouseLookMode == 1 && focused && SDL_GetRelativeMouseMode()) {
        std::lock_guard<std::mutex> lk(psp::host::g_aim.m);
        psp::host::g_aim.enabled = true;
        psp::host::g_aim.yaw += in.lookAccumX * kRadiansPerCount * lookScale;
        psp::host::g_aim.pitch += in.lookAccumY * kRadiansPerCount * lookScale;
        in.lookAccumX = in.lookAccumY = 0;
    } else if (s.mouseLookMode == 2 && focused && SDL_GetRelativeMouseMode()) {
        auto press = [&](const std::string& n) { int k = inputIndex(n); if (k >= 0) buttons |= kInputButtons[k]; };
        const float step = 6.0f;
        if (in.lookAccumX <= -step) { press(s.lookLeft); in.lookAccumX += step; }
        else if (in.lookAccumX >= step) { press(s.lookRight); in.lookAccumX -= step; }
        else in.lookAccumX *= 0.8f;
        if (in.lookAccumY <= -step) { press(s.lookUp); in.lookAccumY += step; }
        else if (in.lookAccumY >= step) { press(s.lookDown); in.lookAccumY -= step; }
        else in.lookAccumY *= 0.8f;
    }
    in.wheelUp = in.wheelDown = false;

    static const bool inputDebug = std::getenv("CA_INPUT_DEBUG") != nullptr;
    static bool lastAds = false;
    if (inputDebug && (buttons != psp::host::g_ctrl.buttons.load() || in.adsOn != lastAds))
        std::fprintf(stderr, "[INPUT] buttons %08x ads %d focused %d\n", buttons, (int)in.adsOn, (int)focused);
    lastAds = in.adsOn;
    psp::host::g_ctrl.buttons = buttons;
    psp::host::g_ctrl.lx = (uint8_t)std::clamp((int)std::lround(128 + ax * 127), 0, 255);
    psp::host::g_ctrl.ly = (uint8_t)std::clamp((int)std::lround(128 + ay * 127), 0, 255);
}

// ---- button hints -----------------------------------------------------------------------------

// Short label of the first keyboard/mouse binding of a PSP button, for the keycaps drawn over
// the game's PSP button symbols (psp/gpu/button_glyphs.h).
std::string keyLabel(const std::vector<ca::Binding>& list) {
    using T = ca::Binding::Type;
    for (const ca::Binding& b : list) {
        if (b.type == T::MouseButton) {
            static const char* n[] = {"", "LMB", "MMB", "RMB", "MB4", "MB5"};
            return b.code >= 1 && b.code <= 5 ? n[b.code] : "";
        }
        if (b.type == T::MouseWheelUp) return "WHU";
        if (b.type == T::MouseWheelDown) return "WHD";
        if (b.type != T::Key) continue;
        const std::string name = SDL_GetScancodeName(SDL_Scancode(b.code));
        static const std::pair<const char*, const char*> kShort[] = {
            {"Return", "\x01"}, {"Keypad Enter", "\x01"}, {"Up", "\x02"}, {"Down", "\x03"}, {"Left", "\x04"},
            {"Right", "\x05"}, {"Backspace", "BKS"}, {"Space", "SPC"}, {"Escape", "ESC"}, {"Tab", "TAB"},
            {"Left Shift", "SHF"}, {"Right Shift", "SHF"}, {"Left Ctrl", "CTL"}, {"Right Ctrl", "CTL"},
            {"Left Alt", "ALT"}, {"Right Alt", "ALT"}, {"CapsLock", "CAP"}, {"Delete", "DEL"}, {"Insert", "INS"},
            {"Home", "HOM"}, {"End", "END"}, {"PageUp", "PGU"}, {"PageDown", "PGD"}};
        for (const auto& [full, label] : kShort)
            if (name == full) return label;
        std::string out;
        for (char c : name)
            if (std::isalnum((unsigned char)c) && out.size() < 3) out += (char)std::toupper((unsigned char)c);
        return out;
    }
    return "";
}

void installButtonLabels(const FrontSettings& s) {
    psp::gpu::ButtonLabels l;
    l.cross = keyLabel(s.bindings[IN_CROSS]);
    l.circle = keyLabel(s.bindings[IN_CIRCLE]);
    l.square = keyLabel(s.bindings[IN_SQUARE]);
    l.triangle = keyLabel(s.bindings[IN_TRIANGLE]);
    l.l = keyLabel(s.bindings[IN_L]);
    l.r = keyLabel(s.bindings[IN_R]);
    l.start = keyLabel(s.bindings[IN_START]);
    l.select = keyLabel(s.bindings[IN_SELECT]);
    psp::gpu::setButtonLabels(l);
}

// ---- audio --------------------------------------------------------------------------------

std::atomic<float> g_volume{1.0f};

std::atomic<int> g_audioUnderruns{0};

// Mixes the game's sceAudio channels. Blocking outputs on the game side wait for these queues
// to drain (host::g_audioConsumer), so the game is paced by this callback and the queues hold
// a steady ~2048 frames: enough to ride out scheduling jitter without audible gaps.
void audioCallback(void*, Uint8* stream, int len) {
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    const int n = len / 2;
    std::vector<int> mix((size_t)n, 0);
    {
        std::lock_guard<std::mutex> lk(psp::host::g_audioMutex);
        for (auto& ch : psp::host::g_audio) {
            if (ch.queue.empty()) continue;
            size_t take = std::min<size_t>(ch.queue.size(), (size_t)n);
            if (take < (size_t)n && ch.reserved) g_audioUnderruns++;
            for (size_t i = 0; i < take; ++i) mix[i] += ch.queue[i];
            ch.queue.erase(ch.queue.begin(), ch.queue.begin() + (long)take);
        }
    }
    psp::kernel::notifyStateChanged();
    const float v = g_volume.load();
    for (int i = 0; i < n; ++i) out[i] = (int16_t)std::clamp((int)(mix[i] * v), -32768, 32767);
}

// ---- present --------------------------------------------------------------------------------

const char* kBlitVs =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "uniform vec4 uRect;\n" // x0,y0,x1,y1 in NDC
    "uniform vec2 uUV1;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : 0.0, (gl_VertexID & 2) != 0 ? 1.0 : 0.0);\n"
    "  vUV = vec2(p.x * uUV1.x, p.y * uUV1.y);\n"
    "  gl_Position = vec4(mix(uRect.x, uRect.z, p.x), mix(uRect.y, uRect.w, p.y), 0.0, 1.0);\n"
    "}\n";
const char* kBlitFs =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = vec4(texture(uTex, vUV).rgb, 1.0); }\n";
const char* kOverlayFs =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = texture(uTex, vUV); }\n";

// Name of the first keyboard/mouse binding, for the dialog key legend.
std::string keyName(const std::vector<ca::Binding>& list) {
    for (const ca::Binding& b : list) {
        if (b.type == ca::Binding::Type::MouseButton) return b.code == 1 ? "Mouse L" : (b.code == 3 ? "Mouse R" : "Mouse");
        if (b.type != ca::Binding::Type::Key) continue;
        const std::string n = SDL_GetScancodeName(SDL_Scancode(b.code));
        return n == "Return" ? "Enter" : n;
    }
    return "?";
}

GLuint makeProgram(const char* vs, const char* fs) {
    auto sh = [](GLenum t, const char* src) {
        GLuint s = glCreateShader(t);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };
    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

// ---- options menu (F1) ------------------------------------------------------------------------

const char* const kInputLabels[IN_COUNT] = {
    "Arriba - mira telescópica", "Abajo - fijar blanco", "Izquierda - cambiar de arma", "Derecha - recargar",
    "Cruz - mirar abajo / aceptar", "Círculo - mirar a la derecha / atrás", "Cuadrado - mirar a la izquierda",
    "Triángulo - mirar arriba / atrás", "L - saltar", "R - disparar", "Start - pausa", "Select - visor",
    "Stick - avanzar", "Stick - retroceder", "Stick - paso a la izquierda", "Stick - paso a la derecha",
    "Apuntar con la mira", "Correr", "Andar con sigilo"};

// 0 keyboard, 1 mouse, 2 pad: a new binding replaces those of its own kind only.
int bindingDevice(const ca::Binding& b) {
    using T = ca::Binding::Type;
    return b.type == T::Key ? 0 : (b.type == T::PadButton || b.type == T::PadTrigger ? 2 : 1);
}

std::string bindingLabel(const ca::Binding& b) {
    using T = ca::Binding::Type;
    switch (b.type) {
    case T::Key: return SDL_GetScancodeName(SDL_Scancode(b.code));
    case T::MouseButton: {
        static const char* n[] = {"Ratón", "Ratón izq.", "Ratón central", "Ratón der.", "Ratón 4", "Ratón 5"};
        return b.code >= 1 && b.code <= 5 ? n[b.code] : n[0];
    }
    case T::MouseWheelUp: return "Rueda arriba";
    case T::MouseWheelDown: return "Rueda abajo";
    case T::PadButton: {
        const char* n = SDL_GameControllerGetStringForButton(SDL_GameControllerButton(b.code));
        return std::string("Mando ") + (n ? n : "?");
    }
    case T::PadTrigger: return b.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? "Mando LT" : "Mando RT";
    }
    return "?";
}

std::string bindingsLabel(const std::vector<ca::Binding>& list) {
    std::string out;
    for (const ca::Binding& b : list) out += (out.empty() ? "" : " / ") + bindingLabel(b);
    return out.empty() ? "-" : out;
}

void setBindingText(FrontSettings& s, int i) {
    std::string t;
    for (const ca::Binding& b : s.bindings[i]) t += (t.empty() ? "" : ", ") + ca::describeBinding(b);
    s.bindingText[i] = t;
}

// `b` becomes input `i`'s binding of its kind (keyboard, mouse or pad), replacing the others of
// that kind, and is taken off any other input.
void assignBinding(FrontSettings& s, int i, const ca::Binding& b) {
    auto same = [&](const ca::Binding& x) { return x.type == b.type && x.code == b.code; };
    for (int j = 0; j < IN_COUNT; ++j) {
        auto& l = s.bindings[j];
        const size_t n = l.size();
        if (j == i) l.erase(std::remove_if(l.begin(), l.end(), [&](const ca::Binding& x) { return bindingDevice(x) == bindingDevice(b); }), l.end());
        else l.erase(std::remove_if(l.begin(), l.end(), same), l.end());
        if (j == i) l.insert(l.begin(), b);
        if (j == i || l.size() != n) setBindingText(s, j);
    }
}

struct MenuState {
    ca::OptionsMenu menu;
    int capture = -1;          // input waiting for a key (-1: none)
    bool quit = false;
};

std::string onOff(bool v) { return v ? "Sí" : "No"; }

ca::MenuItem header(const char* label) {
    ca::MenuItem m;
    m.label = label;
    m.header = true;
    return m;
}

ca::MenuItem toggle(const char* label, bool& v, std::function<void()> apply = {}, const char* note = "") {
    ca::MenuItem m;
    m.label = label;
    m.value = [&v] { return onOff(v); };
    m.change = [&v, apply](int) { v = !v; if (apply) apply(); };
    m.note = note;
    return m;
}

ca::MenuItem choice(const char* label, std::vector<std::string> names, std::function<int()> get, std::function<void(int)> set,
                    const char* note = "") {
    ca::MenuItem m;
    m.label = label;
    m.value = [names, get] { int k = get(); return k >= 0 && k < (int)names.size() ? names[(size_t)k] : "?"; };
    m.change = [n = (int)names.size(), get, set](int d) { set(((std::max(0, get()) + d) % n + n) % n); };
    m.note = note;
    return m;
}

ca::MenuItem slider(const char* label, float& v, float lo, float hi, float step, bool percent, std::function<void()> apply = {}) {
    ca::MenuItem m;
    m.label = label;
    m.value = [&v, percent] {
        char b[32];
        if (percent) std::snprintf(b, sizeof b, "%d %%", (int)std::lround(v * 100));
        else std::snprintf(b, sizeof b, "%.1f", v);
        return std::string(b);
    };
    m.change = [&v, lo, hi, step, apply](int d) {
        v = std::clamp(std::round((v + d * step) / step) * step, lo, hi);
        if (apply) apply();
    };
    return m;
}

std::vector<ca::MenuItem> buildMenu(FrontSettings& s, SDL_Window* win, MenuState& st) {
    std::vector<ca::MenuItem> m;
    m.push_back(header("Vídeo"));
    m.push_back(choice("Pantalla", {"Ventana", "Pantalla completa"},
                       [win] { return (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) ? 1 : 0; },
                       [&s, win](int v) {
                           s.fullscreen = v;
                           SDL_SetWindowFullscreen(win, v ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                       }));
    static const int kSizes[][2] = {{960, 544}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    m.push_back(choice("Tamaño de la ventana", {"960 x 544", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"},
                       [&s] {
                           for (int k = 0; k < 6; ++k) if (s.width == kSizes[k][0] && s.height == kSizes[k][1]) return k;
                           return -1;
                       },
                       [&s, win](int k) {
                           s.width = kSizes[k][0];
                           s.height = kSizes[k][1];
                           if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) {
                               SDL_SetWindowSize(win, s.width, s.height);
                               SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                           }
                       }));
    m.push_back(toggle("Sincronización vertical", s.vsync, [&s] { SDL_GL_SetSwapInterval(s.vsync ? 1 : 0); }));
    m.push_back(choice("Fotogramas por segundo", {"60", "30", "Original (20 en misiones)"},
                       [&s] { return s.frameRate >= 60 ? 0 : (s.frameRate >= 30 ? 1 : 2); },
                       [&s](int k) {
                           s.frameRate = k == 0 ? 60 : (k == 1 ? 30 : 0);
                           psp::host::g_targetFps = s.frameRate;
                       },
                       "al cambiar de pantalla"));
    m.push_back(choice("Resolución interna", {"Automática", "1x", "2x", "3x", "4x", "5x", "6x"},
                       [&s] { return std::clamp(s.renderScale, 0, 6); }, [&s](int k) { s.renderScale = k; }, "al reiniciar"));
    m.push_back(choice("Panorámico", {"No (480x272)", "Sí"}, [&s] { return s.widescreen ? 1 : 0; },
                       [&s](int k) { s.widescreen = k; }, "al reiniciar"));
    m.push_back(toggle("Estirar la imagen a la ventana", s.stretch));
    m.push_back(toggle("Suavizar la imagen", s.linearOutput));
    m.push_back(toggle("Teclas del PC en las ayudas", s.pcButtonHints, {}, "al reiniciar"));

    m.push_back(header("Audio"));
    m.push_back(slider("Volumen", s.volume, 0, 1, 0.05f, true, [&s] { g_volume = s.volume; }));

    m.push_back(header("Ratón"));
    m.push_back(slider("Sensibilidad horizontal", s.mouseSensX, 0.1f, 5, 0.1f, false));
    m.push_back(slider("Sensibilidad vertical", s.mouseSensY, 0.1f, 5, 0.1f, false));
    m.push_back(toggle("Invertir eje vertical", s.invertY));
    m.push_back(toggle("Invertir eje horizontal", s.invertX));
    m.push_back(slider("Sensibilidad al apuntar con la mira", s.adsSens, 0.1f, 2, 0.1f, false));
    m.push_back(choice("Apuntar con la mira", {"Mantener", "Pulsar para alternar"}, [&s] { return s.adsToggle ? 1 : 0; },
                       [&s](int k) { s.adsToggle = k != 0; }));

    m.push_back(header("Mando"));
    m.push_back(slider("Sensibilidad horizontal", s.padSensX, 0.1f, 3, 0.1f, false));
    m.push_back(slider("Sensibilidad vertical", s.padSensY, 0.1f, 3, 0.1f, false));
    m.push_back(toggle("Invertir eje vertical", s.padInvertY));
    m.push_back(slider("Zona muerta del stick izquierdo", s.deadzone, 0, 0.5f, 0.05f, true));
    m.push_back(slider("Zona muerta del stick derecho", s.deadzoneRight, 0, 0.5f, 0.05f, true));

    m.push_back(header("Controles (Intro y pulsa la tecla nueva)"));
    for (int i = 0; i < IN_COUNT; ++i) {
        ca::MenuItem it;
        it.label = kInputLabels[i];
        it.value = [&s, i] { return bindingsLabel(s.bindings[i]); };
        it.activate = [&st, i] {
            st.capture = i;
            st.menu.setPrompt(std::string("Pulsa la tecla, botón del ratón o del mando (F1 cancela)"));
        };
        m.push_back(it);
    }
    {
        ca::MenuItem it;
        it.label = "Restaurar los controles por defecto";
        it.activate = [&s] {
            for (int i = 0; i < IN_COUNT; ++i) {
                s.bindingText[i] = kDefaultBindings[i];
                parseList(s.bindingText[i], s.bindings[i]);
            }
            if (s.pcButtonHints) installButtonLabels(s);
        };
        m.push_back(it);
    }

    m.push_back(header("Otros"));
    m.push_back(toggle("Modo dev (todas las armas, F5/F6)", s.dev, [&s] { psp::host::g_dev.enabled = s.dev; },
                       "desde el siguiente nivel"));
    {
        ca::MenuItem it;
        it.label = "Volver";
        it.help = "Vuelve a Options. Los cambios se guardan en config.ini.";
        it.activate = [&st] { st.menu.close(); };
        m.push_back(it);
    }
    static const std::pair<const char*, const char*> kHelp[] = {
        {"Pantalla", "Ventana o pantalla completa (también con F11)."},
        {"Tamaño de la ventana", "Tamaño de la ventana cuando no está a pantalla completa."},
        {"Sincronización vertical", "Sincroniza con el monitor para evitar cortes en la imagen."},
        {"Fotogramas por segundo", "60 o 30; Original = 20 en las misiones, como en la PSP."},
        {"Resolución interna", "Resolución a la que se dibuja el juego (Automática: según la ventana)."},
        {"Panorámico", "Amplía la vista al formato de la ventana (16:9, 21:9...)."},
        {"Estirar la imagen a la ventana", "Llena la ventana sin mantener la proporción."},
        {"Suavizar la imagen", "Filtro bilineal al escalar la imagen a la ventana."},
        {"Teclas del PC en las ayudas", "Muestra las teclas del PC en lugar de los botones de la PSP."},
        {"Volumen", "Volumen general del juego."},
        {"Sensibilidad horizontal", "Velocidad de giro."},
        {"Sensibilidad vertical", "Velocidad al mirar arriba y abajo."},
        {"Invertir eje vertical", "Invierte arriba y abajo al mirar."},
        {"Invertir eje horizontal", "Invierte izquierda y derecha al girar."},
        {"Sensibilidad al apuntar con la mira", "Velocidad del ratón y del mando mientras se apunta con la mira."},
        {"Apuntar con la mira", "Mantener pulsado para apuntar, o pulsar una vez para alternar."},
        {"Zona muerta del stick izquierdo", "Movimiento del stick que se ignora (desgaste del mando)."},
        {"Zona muerta del stick derecho", "Movimiento del stick que se ignora (desgaste del mando)."},
        {"Restaurar los controles por defecto", "Vuelve a la configuración de teclas inicial."},
        {"Modo dev (todas las armas, F5/F6)", "Pruebas: todas las armas y munición infinita; F5/F6 cambian el arma."},
    };
    for (ca::MenuItem& it : m) {
        if (it.header || !it.help.empty()) continue;
        for (const auto& h : kHelp) if (it.label == h.first) it.help = h.second;
        if (it.help.empty() && it.activate && it.value) it.help = "Pulsa " + keyName(s.bindings[IN_CROSS]) + " y después la tecla o el botón nuevo.";
    }
    return m;
}

std::string findIso(const std::string& configured) {
    if (!configured.empty()) return configured;
    std::string base;
    if (char* b = SDL_GetBasePath()) { base = b; SDL_free(b); }
    for (const char* name : {"game/CodedArmsContagion.iso", "CodedArmsContagion.iso", "game/Coded.Arms.Contagion.iso", "Coded.Arms.Contagion.iso"}) {
        std::string p = base + name;
        if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return p; }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    {
        std::string dir;
        if (char* b = SDL_GetBasePath()) { dir = b; SDL_free(b); }
        ca::initCrashLog(dir);
    }
    if (std::getenv("CA_TEST_CRASH")) // checks the crash report: an invalid write after 5 s
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            *static_cast<volatile int*>(nullptr) = 1;
        }).detach();
    FrontSettings s;
    ca::Ini ini;
    const std::string cfgPath = ca::resolveConfigPath();
    loadSettings(s, ini, cfgPath);
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--iso") && i + 1 < argc) s.iso = argv[++i];
        else if (!std::strcmp(argv[i], "--windowed")) s.fullscreen = 0;
        else if (!std::strcmp(argv[i], "--fullscreen")) s.fullscreen = 1;
    }
    saveSettings(s, ini, cfgPath); // writes the defaults on first run
    // The memory stick (saves, profiles) lives next to config.ini, not in the working directory.
    psp::kernel::g_hostMsRoot = cfgPath.substr(0, cfgPath.find_last_of("/\\") + 1) + "memstick/";

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    std::string iso = findIso(s.iso);
    if (iso.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Coded Arms: Contagion",
                                 "Your Coded Arms: Contagion (ULES-00718) UMD image was not found.\n"
                                 "Put it at game/CodedArmsContagion.iso next to the executable, set [game] iso= in config.ini,\n"
                                 "or start with --iso PATH.", nullptr);
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (s.fullscreen == 1) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (s.fullscreen == 2) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* win = SDL_CreateWindow("Coded Arms: Contagion", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, s.width, s.height, flags);
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Coded Arms: Contagion", "OpenGL 3.3 is required.", win);
        return 1;
    }
    const char* missing = nullptr;
    if (!ca::loadGL(&missing)) { std::fprintf(stderr, "missing GL function %s\n", missing); return 1; }
    SDL_GL_SetSwapInterval(s.vsync ? 1 : 0);

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    psp::gpu::Settings gs;
    gs.scale = s.renderScale > 0 ? s.renderScale : std::clamp((dh + 271) / 272, 1, 8);
    gs.forceLinear = s.forceLinearTextures;
    // Hor+: the PSP's 480x272 is ~16:9; wider windows see more to the sides (set at start-up).
    if (s.widescreen && dw > 0 && dh > 0 && (float)dw / dh > 480.0f / 272.0f + 0.01f) gs.wideAspect = (float)dw / dh;
    psp::gpu::install(gs);
    if (s.pcButtonHints) installButtonLabels(s);
    psp::host::g_targetFps = s.frameRate >= 60 ? 60 : (s.frameRate >= 30 ? 30 : 0);
    psp::host::g_dev.enabled = s.dev;
    {
        int ids[5] = {-1, -1, -1, -1, -1};
        std::sscanf(s.devLoadout.c_str(), "%d , %d , %d , %d , %d", &ids[0], &ids[1], &ids[2], &ids[3], &ids[4]);
        for (int i = 0; i < 5; ++i) psp::host::g_dev.loadout[i] = ids[i] >= 0 && ids[i] < 33 ? ids[i] : -1;
        if (s.dev) std::fprintf(stderr, "[DEV] dev mode: weapons %d %d %d %d %d, F5/F6 change the weapon\n", ids[0], ids[1], ids[2], ids[3], ids[4]);
    }
    if (!psp::gpu::renderInit()) return 1;
    GLuint blit = makeProgram(kBlitVs, kBlitFs);
    GLuint overlayProg = makeProgram(kBlitVs, kOverlayFs);
    GLuint overlayTex = 0;
    glGenTextures(1, &overlayTex);
    glBindTexture(GL_TEXTURE_2D, overlayTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ca::kOverlayW, ca::kOverlayH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint64_t overlaySerial = ~0ull;
    constexpr int kMapPx = 256;
    GLuint mapTex = 0;
    glGenTextures(1, &mapTex);
    glBindTexture(GL_TEXTURE_2D, mapTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kMapPx, kMapPx, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<uint32_t> mapPx;
    psp::gpu::minimap::setEnabled(s.minimap);
    std::vector<uint32_t> overlayPx;
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);

    // Audio
    g_volume = s.volume;
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audioCallback;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) {
        psp::host::g_audioConsumer = !std::getenv("CA_AUDIO_WALLCLOCK");
        SDL_PauseAudioDevice(audio, 0);
    }

    InputState in;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) { in.pad = SDL_GameControllerOpen(i); break; }

    psp::kernel::BootOptions opt;
    opt.isoPath = iso;
    // The system dialogs (load/save, messages) read the controller like on the PSP and are
    // drawn by the overlay below.
    psp::host::setDialogUsesController(true);
    std::thread game([opt] {
        int r = psp::kernel::boot(opt);
        SDL_Event q{};
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
        (void)r;
    });
    game.detach();

    SDL_SetRelativeMouseMode(s.mouseLook ? SDL_TRUE : SDL_FALSE);
    const bool perf = std::getenv("CA_PERF") != nullptr;
    Uint64 lastTicks = SDL_GetPerformanceCounter();
    psp::gpu::PresentInfo frame;
    bool haveFrame = false, running = true, focused = true;
    // PC settings, opened from the game's OPTIONS screen (its PC SETUP row): the game is paused
    // while they are open.
    MenuState menu;
    bool menuActive = false;
    auto openMenu = [&] {
        menu.menu.open(buildMenu(s, win, menu));
        menu.menu.setFooter("<> Cambiar   " + keyName(s.bindings[IN_CROSS]) + " OK   " + keyName(s.bindings[IN_CIRCLE]) + "/Esc Volver");
        menu.menu.setTitle("PC Setup");
        menuActive = true;
        psp::kernel::setPaused(true);
        SDL_SetRelativeMouseMode(SDL_FALSE);
    };
    auto toOverlay = [&](int mx, int my, int& ox, int& oy) {
        int ww = 1, wh = 1;
        SDL_GetWindowSize(win, &ww, &wh);
        const float aspect = 480.0f / 272.0f, wa = (float)ww / (float)std::max(wh, 1);
        float sx = 1, sy = 1;
        if (wa > aspect) sx = aspect / wa; else sy = wa / aspect;
        ox = (int)(((2.0f * mx / std::max(ww, 1) - 1) + sx) / (2 * sx) * ca::kOverlayW);
        oy = (int)(((2.0f * my / std::max(wh, 1) - 1) + sy) / (2 * sy) * ca::kOverlayH);
    };
    auto captureBinding = [&](ca::Binding b) {
        assignBinding(s, menu.capture, b);
        menu.capture = -1;
        menu.menu.setPrompt("");
    };
    // Menu input; true when the event was used.
    auto menuEvent = [&](const SDL_Event& ev) {
        using BT = ca::Binding::Type;
        switch (ev.type) {
        case SDL_KEYDOWN: {
            if (ev.key.repeat && menu.capture >= 0) return true;
            const SDL_Scancode k = ev.key.keysym.scancode;
            if (menu.capture >= 0) {
                if (k == SDL_SCANCODE_F1) { menu.capture = -1; menu.menu.setPrompt(""); }
                else captureBinding(ca::Binding{BT::Key, (int)k});
                return true;
            }
            auto bound = [&](int input) {
                for (const ca::Binding& b : s.bindings[input]) if (b.type == BT::Key && b.code == (int)k) return true;
                return false;
            };
            if (k == SDL_SCANCODE_ESCAPE || bound(IN_CIRCLE) || bound(IN_TRIANGLE)) menu.menu.close();
            else if (k == SDL_SCANCODE_UP || k == SDL_SCANCODE_W) menu.menu.move(-1);
            else if (k == SDL_SCANCODE_DOWN || k == SDL_SCANCODE_S) menu.menu.move(1);
            else if (k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_A) menu.menu.change(-1);
            else if (k == SDL_SCANCODE_RIGHT || k == SDL_SCANCODE_D) menu.menu.change(1);
            else if ((k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || bound(IN_CROSS)) && !ev.key.repeat) menu.menu.activate();
            else if (k == SDL_SCANCODE_PAGEUP) for (int i = 0; i < 8; ++i) menu.menu.move(-1);
            else if (k == SDL_SCANCODE_PAGEDOWN) for (int i = 0; i < 8; ++i) menu.menu.move(1);
            return true;
        }
        case SDL_MOUSEMOTION: {
            int ox, oy;
            toOverlay(ev.motion.x, ev.motion.y, ox, oy);
            if (menu.capture < 0) menu.menu.hover(ox, oy);
            return true;
        }
        case SDL_MOUSEBUTTONDOWN: {
            if (menu.capture >= 0) { captureBinding(ca::Binding{BT::MouseButton, (int)ev.button.button}); return true; }
            int ox, oy;
            toOverlay(ev.button.x, ev.button.y, ox, oy);
            if (ev.button.button == SDL_BUTTON_LEFT) menu.menu.click(ox, oy);
            return true;
        }
        case SDL_MOUSEBUTTONUP: return true;
        case SDL_MOUSEWHEEL:
            if (menu.capture >= 0) {
                if (ev.wheel.y) captureBinding(ca::Binding{ev.wheel.y > 0 ? BT::MouseWheelUp : BT::MouseWheelDown, 0});
            } else if (ev.wheel.y) {
                menu.menu.wheel(ev.wheel.y > 0 ? -2 : 2);
            }
            return true;
        case SDL_CONTROLLERBUTTONDOWN: {
            const int b = ev.cbutton.button;
            if (menu.capture >= 0) {
                if (b == SDL_CONTROLLER_BUTTON_GUIDE) { menu.capture = -1; menu.menu.setPrompt(""); }
                else captureBinding(ca::Binding{BT::PadButton, b});
                return true;
            }
            if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) menu.menu.move(-1);
            else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) menu.menu.move(1);
            else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) menu.menu.change(-1);
            else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) menu.menu.change(1);
            else if (b == SDL_CONTROLLER_BUTTON_A) menu.menu.activate();
            else if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START || b == SDL_CONTROLLER_BUTTON_GUIDE) menu.menu.close();
            return true;
        }
        case SDL_CONTROLLERAXISMOTION:
            if (menu.capture >= 0 && ev.caxis.value > 16000 &&
                (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT))
                captureBinding(ca::Binding{BT::PadTrigger, (int)ev.caxis.axis});
            return true;
        }
        return false;
    };
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11) {
                bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                continue;
            }
            static const bool eventDebug = std::getenv("CA_EVENT_DEBUG") != nullptr;
            if (eventDebug) std::fprintf(stderr, "[EVENT] %x %d\n", e.type, e.type == SDL_WINDOWEVENT ? (int)e.window.event : -1);
            if (menuActive && menuEvent(e)) continue;
            switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
                if (std::getenv("CA_INPUT_DEBUG")) std::fprintf(stderr, "[INPUT] key %s\n", SDL_GetScancodeName(e.key.keysym.scancode));
                if (e.key.keysym.scancode < SDL_NUM_SCANCODES && !e.key.repeat) in.keyClicked[e.key.keysym.scancode] = true;
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (e.key.keysym.scancode == SDL_SCANCODE_N && !e.key.repeat) {
                    s.minimap = !s.minimap;
                    psp::gpu::minimap::setEnabled(s.minimap);
                } else if (s.dev && (e.key.keysym.scancode == SDL_SCANCODE_F5 || e.key.keysym.scancode == SDL_SCANCODE_F6)) {
                    psp::host::g_dev.cycle = e.key.keysym.scancode == SDL_SCANCODE_F6 ? 1 : -1;
                } else if (e.key.keysym.scancode == SDL_SCANCODE_F10) {
                    SDL_SetRelativeMouseMode(SDL_GetRelativeMouseMode() ? SDL_FALSE : SDL_TRUE);
                }
                break;
            case SDL_MOUSEMOTION:
                if (SDL_GetRelativeMouseMode()) {
                    in.lookAccumX += e.motion.xrel * s.mouseSensX * (s.invertX ? -1.0f : 1.0f);
                    in.lookAccumY += e.motion.yrel * s.mouseSensY * (s.invertY ? -1.0f : 1.0f);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                in.mouseHeld |= SDL_BUTTON(e.button.button);
                in.mouseClicked |= SDL_BUTTON(e.button.button);
                break;
            case SDL_MOUSEBUTTONUP:
                in.mouseHeld &= ~SDL_BUTTON(e.button.button);
                break;
            case SDL_MOUSEWHEEL:
                if (e.wheel.y > 0) in.wheelUp = true;
                if (e.wheel.y < 0) in.wheelDown = true;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!in.pad) in.pad = SDL_GameControllerOpen(e.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (in.pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(in.pad))) {
                    SDL_GameControllerClose(in.pad);
                    in.pad = nullptr;
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { focused = false; in.mouseHeld = 0; }
                break;
            }
        }
        if (menu.quit) running = false;
        if (menuActive && !menu.menu.isOpen()) { // closed: resume, keep the settings
            menuActive = false;
            menu.capture = -1;
            menu.menu.setPrompt("");
            saveSettings(s, ini, cfgPath);
            if (s.pcButtonHints) installButtonLabels(s);
            SDL_SetRelativeMouseMode(s.mouseLook ? SDL_TRUE : SDL_FALSE);
            in.mouseHeld = in.mouseClicked = 0;
            in.lookAccumX = in.lookAccumY = 0;
            in.waitRelease = true;
            psp::kernel::setPaused(false);
        }
        Uint64 nowTicks = SDL_GetPerformanceCounter();
        float dt = std::min(0.1f, (float)(nowTicks - lastTicks) / (float)SDL_GetPerformanceFrequency());
        lastTicks = nowTicks;
        if (!menuActive) updateController(s, in, focused, dt);
        if (in.openPcSettings) {
            in.openPcSettings = false;
            if (!menuActive) openMenu();
        }
        if (perf) {
            static Uint64 lastReport = nowTicks;
            if (nowTicks - lastReport >= SDL_GetPerformanceFrequency()) {
                lastReport = nowTicks;
                std::fprintf(stderr, "[perf] audio underruns %d\n", g_audioUnderruns.exchange(0));
            }
        }

        psp::gpu::PresentInfo p;
        const Uint64 pumpStart = SDL_GetPerformanceCounter();
        int pumped = 0;
        if (psp::gpu::renderPump(p, 4)) { frame = p; haveFrame = true; ++pumped; }
        // Show the newest frame: frames left queued would make the game wait for the display.
        // Bounded, so a slow GPU (or a software renderer) still lets input and the window run.
        while (SDL_GetPerformanceCounter() - pumpStart < SDL_GetPerformanceFrequency() / 30 && psp::gpu::renderPump(p, 0)) {
            frame = p;
            ++pumped;
        }
        if (perf) {
            static double pumpMs = 0, maxMs = 0;
            static int loops = 0, frames = 0;
            const double ms = 1000.0 * (double)(SDL_GetPerformanceCounter() - pumpStart) / (double)SDL_GetPerformanceFrequency();
            pumpMs += ms;
            maxMs = std::max(maxMs, ms);
            ++loops;
            frames += pumped;
            if (loops == 60) {
                std::fprintf(stderr, "[perf] 60 loops: %d frames, render %.1f ms avg, %.1f ms max\n", frames, pumpMs / 60, maxMs);
                pumpMs = maxMs = 0;
                loops = frames = 0;
            }
        }

        SDL_GL_GetDrawableSize(win, &dw, &dh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(1, 1, 1, 1);
        glViewport(0, 0, dw, dh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (haveFrame) {
            // Fit 480:272 into the window (or stretch). Framebuffer row 0 is the top row.
            float aspect = (frame.width * frame.u1) / std::max(1.0f, frame.height * frame.v1), wa = (float)dw / (float)std::max(dh, 1);
            float sx = 1, sy = 1;
            if (!s.stretch) { if (wa > aspect) sx = aspect / wa; else sy = wa / aspect; }
            glUseProgram(blit);
            glUniform4f(glGetUniformLocation(blit, "uRect"), -sx, sy, sx, -sy);
            glUniform2f(glGetUniformLocation(blit, "uUV1"), frame.u1, frame.v1);
            glUniform1i(glGetUniformLocation(blit, "uTex"), 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, frame.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s.linearOutput ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s.linearOutput ? GL_LINEAR : GL_NEAREST);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        psp::host::DialogView dialog;
        const bool dialogUp = psp::host::currentDialog(dialog);
        if (haveFrame && s.minimap && !dialogUp && psp::gpu::minimap::render(mapPx, kMapPx, s.minimapRange)) {
            // Bottom left of the picture, above the game's health bar.
            float aspect = (frame.width * frame.u1) / std::max(1.0f, frame.height * frame.v1), wa = (float)dw / (float)std::max(dh, 1);
            float sx = 1, sy = 1;
            if (!s.stretch) { if (wa > aspect) sx = aspect / wa; else sy = wa / aspect; }
            const float hNdc = 2 * sy * s.minimapSize;              // map height in NDC
            const float wNdc = hNdc * (float)dh / (float)std::max(dw, 1); // square on screen
            const float marginX = 0.02f * 2 * sy * (float)dh / (float)std::max(dw, 1);
            float x0 = -sx + marginX, y0 = -sy + 2 * sy * 0.10f;
            if (s.minimapCorner == 1) { // top right, below the game's own radar (about 30% down)
                x0 = sx - marginX - wNdc;
                y0 = sy - 2 * sy * 0.32f - hNdc;
            }
            glBindTexture(GL_TEXTURE_2D, mapTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kMapPx, kMapPx, GL_RGBA, GL_UNSIGNED_BYTE, mapPx.data());
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(overlayProg);
            glUniform4f(glGetUniformLocation(overlayProg, "uRect"), x0, y0 + hNdc, x0 + wNdc, y0);
            glUniform2f(glGetUniformLocation(overlayProg, "uUV1"), 1.0f, 1.0f);
            glUniform1i(glGetUniformLocation(overlayProg, "uTex"), 0);
            glActiveTexture(GL_TEXTURE0);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        }
        if (menuActive) {
            menu.menu.paint(overlayPx);
            glBindTexture(GL_TEXTURE_2D, overlayTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ca::kOverlayW, ca::kOverlayH, GL_RGBA, GL_UNSIGNED_BYTE, overlayPx.data());
            overlaySerial = ~0ull;
        } else if (dialogUp && dialog.serial != overlaySerial) {
            const bool cross = dialog.crossConfirms;
            ca::DialogKeys keys{keyName(s.bindings[cross ? IN_CROSS : IN_CIRCLE]), keyName(s.bindings[cross ? IN_CIRCLE : IN_CROSS]),
                                keyName(s.bindings[IN_UP]) + "/" + keyName(s.bindings[IN_DOWN]),
                                keyName(s.bindings[IN_LEFT]) + "/" + keyName(s.bindings[IN_RIGHT])};
            ca::paintDialog(dialog, keys, overlayPx);
            glBindTexture(GL_TEXTURE_2D, overlayTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ca::kOverlayW, ca::kOverlayH, GL_RGBA, GL_UNSIGNED_BYTE, overlayPx.data());
            overlaySerial = dialog.serial;
        }
        if (menuActive || dialogUp) {
            // The PSP screen's shape, centred.
            const float aspect = 480.0f / 272.0f, wa = (float)dw / (float)std::max(dh, 1);
            float sx = 1, sy = 1;
            if (wa > aspect) sx = aspect / wa; else sy = wa / aspect;
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(overlayProg);
            glUniform4f(glGetUniformLocation(overlayProg, "uRect"), -sx, sy, sx, -sy);
            glUniform2f(glGetUniformLocation(overlayProg, "uUV1"), 1.0f, 1.0f);
            glUniform1i(glGetUniformLocation(overlayProg, "uTex"), 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, overlayTex);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        } else {
            overlaySerial = ~0ull;
        }
        SDL_GL_SwapWindow(win);
    }
    psp::host::g_audioConsumer = false;
    if (audio) SDL_CloseAudioDevice(audio);
    std::_Exit(0);
}
