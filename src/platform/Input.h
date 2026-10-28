#pragma once
// Action-based input. Gameplay code never looks at keys or buttons directly: it asks
// for actions (Fire, Jump...) whose bindings come from config.ini, so keyboard/mouse
// and any SDL_GameController-compatible pad (XInput/Xbox, DualShock 4, DualSense,
// Switch Pro...) are interchangeable and fully remappable.
#include "core/Config.h"
#include "core/MathUtil.h"
#include <SDL.h>
#include <string>
#include <vector>

namespace ca {

enum class Action {
    MoveForward, MoveBack, MoveLeft, MoveRight,
    Jump, Fire, Aim, Reload, Interact, NextWeapon, PrevWeapon, Grenade, Map, Pause,
    MenuUp, MenuDown, MenuLeft, MenuRight, MenuAccept, MenuBack,
    Count
};

const char* actionName(Action a);

struct Binding {
    enum class Type { Key, MouseButton, MouseWheelUp, MouseWheelDown, PadButton, PadTrigger };
    Type type = Type::Key;
    int code = 0;
};

// Parses "key:W", "mouse:left", "mouse:wheelup", "pad:a", "pad:righttrigger".
bool parseBinding(const std::string& text, Binding& out);
std::string describeBinding(const Binding& b);

// Radial dead zone with rescaling so output starts at 0 just outside the dead zone and
// reaches 1 at the rim. Exposed for unit tests.
Vec2 applyRadialDeadzone(float x, float y, float inner, float outer = 0.98f);

enum class InputDevice { KeyboardMouse, Gamepad };

class Input {
public:
    void init(const Settings& s);
    void shutdown();
    void applySettings(const Settings& s);
    static void defaultBindings(Settings& s);

    void handleEvent(const SDL_Event& e);
    void update(); // once per frame after events: computes held state and edge latches

    bool held(Action a) const;
    bool pressed(Action a) const;   // rising edge since last consumeTick()
    void consumeTick();             // call after each fixed simulation tick

    Vec2 moveAxis() const;          // x = strafe right, y = forward; |v| <= 1
    Vec2 takeMouseDelta();          // raw counts accumulated since last call
    Vec2 padLook() const;           // processed right stick in [-1,1]

    void rumble(float low, float high, int ms);
    InputDevice lastDevice() const { return lastDevice_; }
    bool hasGamepad() const { return pad_ != nullptr; }
    std::string gamepadName() const;
    const std::vector<Binding>& bindings(Action a) const { return bindings_[int(a)]; }

    // Scripted input for automated tests.
    void inject(Action a, bool down) { injected_[int(a)] = down; }
    void injectMouse(float dx, float dy) { mouseDx_ += dx; mouseDy_ += dy; }

private:
    bool evalBinding(const Binding& b) const;
    void openFirstPad();

    std::vector<Binding> bindings_[int(Action::Count)];
    bool held_[int(Action::Count)] = {};
    bool latch_[int(Action::Count)] = {};
    bool injected_[int(Action::Count)] = {};
    bool wheelUp_ = false, wheelDown_ = false;
    float mouseDx_ = 0, mouseDy_ = 0;
    SDL_GameController* pad_ = nullptr;
    SDL_JoystickID padId_ = -1;
    InputDevice lastDevice_ = InputDevice::KeyboardMouse;
    float deadzoneLeft_ = 0.2f, deadzoneRight_ = 0.15f, lookCurve_ = 1.6f;
    bool rumbleEnabled_ = true;
    float rumbleStrength_ = 1.0f;
};

} // namespace ca
