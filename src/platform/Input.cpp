#include "platform/Input.h"
#include "core/Log.h"
#include <cctype>
#include <cmath>
#include <cstring>

namespace ca {

static const char* kActionNames[] = {
    "move_forward", "move_back", "move_left", "move_right",
    "jump", "fire", "aim", "reload", "interact", "next_weapon", "prev_weapon", "grenade", "map", "pause",
    "menu_up", "menu_down", "menu_left", "menu_right", "menu_accept", "menu_back",
};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == size_t(Action::Count), "action names");

const char* actionName(Action a) { return kActionNames[int(a)]; }

static std::string lowerTrim(const std::string& s) {
    std::string r;
    for (char c : s) if (!std::isspace((unsigned char)c)) r += char(std::tolower((unsigned char)c));
    return r;
}

bool parseBinding(const std::string& text, Binding& out) {
    size_t colon = text.find(':');
    if (colon == std::string::npos) return false;
    std::string kind = lowerTrim(text.substr(0, colon));
    std::string name = text.substr(colon + 1);
    while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
    while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
    std::string lname = lowerTrim(name);

    if (kind == "key") {
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        out = {Binding::Type::Key, int(sc)};
        return true;
    }
    if (kind == "mouse") {
        if (lname == "wheelup") { out = {Binding::Type::MouseWheelUp, 0}; return true; }
        if (lname == "wheeldown") { out = {Binding::Type::MouseWheelDown, 0}; return true; }
        static const struct { const char* n; int b; } kBtn[] = {
            {"left", SDL_BUTTON_LEFT}, {"middle", SDL_BUTTON_MIDDLE}, {"right", SDL_BUTTON_RIGHT},
            {"x1", SDL_BUTTON_X1}, {"x2", SDL_BUTTON_X2}};
        for (auto& b : kBtn) if (lname == b.n) { out = {Binding::Type::MouseButton, b.b}; return true; }
        return false;
    }
    if (kind == "pad") {
        if (lname == "lefttrigger" || lname == "lt" || lname == "l2") {
            out = {Binding::Type::PadTrigger, SDL_CONTROLLER_AXIS_TRIGGERLEFT}; return true;
        }
        if (lname == "righttrigger" || lname == "rt" || lname == "r2") {
            out = {Binding::Type::PadTrigger, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}; return true;
        }
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(lname.c_str());
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return false;
        out = {Binding::Type::PadButton, int(b)};
        return true;
    }
    return false;
}

std::string describeBinding(const Binding& b) {
    switch (b.type) {
    case Binding::Type::Key: return std::string("key:") + SDL_GetScancodeName(SDL_Scancode(b.code));
    case Binding::Type::MouseButton: {
        static const char* n[] = {"?", "left", "middle", "right", "x1", "x2"};
        return std::string("mouse:") + (b.code >= 1 && b.code <= 5 ? n[b.code] : "?");
    }
    case Binding::Type::MouseWheelUp: return "mouse:wheelup";
    case Binding::Type::MouseWheelDown: return "mouse:wheeldown";
    case Binding::Type::PadButton: return std::string("pad:") + SDL_GameControllerGetStringForButton(SDL_GameControllerButton(b.code));
    case Binding::Type::PadTrigger: return b.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? "pad:lefttrigger" : "pad:righttrigger";
    }
    return "?";
}

Vec2 applyRadialDeadzone(float x, float y, float inner, float outer) {
    float mag = std::sqrt(x * x + y * y);
    if (mag <= inner || mag <= 1e-6f) return {0, 0};
    float n = clampf((mag - inner) / std::max(1e-4f, outer - inner), 0.0f, 1.0f);
    float s = n / mag;
    return {x * s, y * s};
}

void Input::defaultBindings(Settings& s) {
    // PC-standard defaults. Only filled for actions the user has not configured.
    const std::pair<Action, const char*> defs[] = {
        {Action::MoveForward, "key:W, key:Up"},
        {Action::MoveBack, "key:S, key:Down"},
        {Action::MoveLeft, "key:A, key:Left"},
        {Action::MoveRight, "key:D, key:Right"},
        {Action::Jump, "key:Space, pad:a"},
        {Action::Fire, "mouse:left, pad:righttrigger"},
        {Action::Aim, "mouse:right, pad:lefttrigger"},
        {Action::Reload, "key:R, pad:x"},
        {Action::Interact, "key:E, pad:b"},
        {Action::NextWeapon, "mouse:wheeldown, key:Q, pad:y"},
        {Action::PrevWeapon, "mouse:wheelup"},
        {Action::Grenade, "key:G, pad:rightshoulder"},
        {Action::Map, "key:Tab, pad:back"},
        {Action::Pause, "key:Escape, pad:start"},
        {Action::MenuUp, "key:Up, key:W, pad:dpup"},
        {Action::MenuDown, "key:Down, key:S, pad:dpdown"},
        {Action::MenuLeft, "key:Left, key:A, pad:dpleft"},
        {Action::MenuRight, "key:Right, key:D, pad:dpright"},
        {Action::MenuAccept, "key:Return, key:Space, pad:a"},
        {Action::MenuBack, "key:Escape, key:Backspace, pad:b"},
    };
    for (auto& [a, txt] : defs)
        if (!s.bindings.count(actionName(a))) s.bindings[actionName(a)] = txt;
}

void Input::init(const Settings& s) {
    SDL_GameControllerEventState(SDL_ENABLE);
    applySettings(s);
    openFirstPad();
}

void Input::shutdown() {
    if (pad_) SDL_GameControllerClose(pad_);
    pad_ = nullptr;
}

void Input::applySettings(const Settings& s) {
    deadzoneLeft_ = s.deadzoneLeft;
    deadzoneRight_ = s.deadzoneRight;
    lookCurve_ = s.padLookCurve;
    rumbleEnabled_ = s.rumble;
    rumbleStrength_ = s.rumbleStrength;
    for (int i = 0; i < int(Action::Count); ++i) {
        bindings_[i].clear();
        auto it = s.bindings.find(kActionNames[i]);
        if (it == s.bindings.end()) continue;
        size_t start = 0;
        const std::string& list = it->second;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            Binding b;
            if (parseBinding(item, b)) bindings_[i].push_back(b);
            else if (item.find_first_not_of(" \t") != std::string::npos)
                log::warn("Ignoring invalid binding '%s' for %s", item.c_str(), kActionNames[i]);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
}

void Input::openFirstPad() {
    if (pad_) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        pad_ = SDL_GameControllerOpen(i);
        if (pad_) {
            padId_ = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_));
            log::info("Gamepad connected: %s", SDL_GameControllerName(pad_));
            return;
        }
    }
}

std::string Input::gamepadName() const {
    const char* n = pad_ ? SDL_GameControllerName(pad_) : nullptr;
    return n ? n : "";
}

void Input::handleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED: openFirstPad(); break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (pad_ && e.cdevice.which == padId_) {
            log::info("Gamepad disconnected");
            SDL_GameControllerClose(pad_);
            pad_ = nullptr;
            padId_ = -1;
            openFirstPad();
        }
        break;
    case SDL_MOUSEMOTION:
        mouseDx_ += float(e.motion.xrel);
        mouseDy_ += float(e.motion.yrel);
        lastDevice_ = InputDevice::KeyboardMouse;
        break;
    case SDL_MOUSEWHEEL:
        if (e.wheel.y > 0) wheelUp_ = true;
        if (e.wheel.y < 0) wheelDown_ = true;
        lastDevice_ = InputDevice::KeyboardMouse;
        break;
    case SDL_KEYDOWN:
    case SDL_MOUSEBUTTONDOWN:
        lastDevice_ = InputDevice::KeyboardMouse;
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        lastDevice_ = InputDevice::Gamepad;
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (std::abs(e.caxis.value) > 12000) lastDevice_ = InputDevice::Gamepad;
        break;
    default: break;
    }
}

bool Input::evalBinding(const Binding& b) const {
    switch (b.type) {
    case Binding::Type::Key: {
        int n = 0;
        const Uint8* ks = SDL_GetKeyboardState(&n);
        return b.code < n && ks[b.code];
    }
    case Binding::Type::MouseButton: return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(b.code)) != 0;
    case Binding::Type::MouseWheelUp: return wheelUp_;
    case Binding::Type::MouseWheelDown: return wheelDown_;
    case Binding::Type::PadButton: return pad_ && SDL_GameControllerGetButton(pad_, SDL_GameControllerButton(b.code));
    case Binding::Type::PadTrigger:
        return pad_ && SDL_GameControllerGetAxis(pad_, SDL_GameControllerAxis(b.code)) > 16000;
    }
    return false;
}

void Input::update() {
    // Left stick doubles as menu navigation.
    Vec2 ls{0, 0};
    if (pad_) {
        ls.x = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        ls.y = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    }
    for (int i = 0; i < int(Action::Count); ++i) {
        bool h = injected_[i];
        for (const Binding& b : bindings_[i]) h = h || evalBinding(b);
        switch (Action(i)) {
        case Action::MenuUp: h = h || ls.y < -0.6f; break;
        case Action::MenuDown: h = h || ls.y > 0.6f; break;
        case Action::MenuLeft: h = h || ls.x < -0.6f; break;
        case Action::MenuRight: h = h || ls.x > 0.6f; break;
        default: break;
        }
        if (h && !held_[i]) latch_[i] = true;
        held_[i] = h;
    }
    // Wheel "buttons" are momentary: held for exactly one frame.
    wheelUp_ = wheelDown_ = false;
}

bool Input::held(Action a) const { return held_[int(a)]; }
bool Input::pressed(Action a) const { return latch_[int(a)]; }

void Input::consumeTick() {
    for (bool& l : latch_) l = false;
}

Vec2 Input::moveAxis() const {
    Vec2 v{0, 0};
    if (held(Action::MoveForward)) v.y += 1;
    if (held(Action::MoveBack)) v.y -= 1;
    if (held(Action::MoveRight)) v.x += 1;
    if (held(Action::MoveLeft)) v.x -= 1;
    if (pad_) {
        float x = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        float y = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        Vec2 s = applyRadialDeadzone(x, y, deadzoneLeft_);
        v.x += s.x;
        v.y -= s.y; // stick up is negative
    }
    float m = std::sqrt(v.x * v.x + v.y * v.y);
    if (m > 1.0f) { v.x /= m; v.y /= m; }
    return v;
}

Vec2 Input::takeMouseDelta() {
    Vec2 d{mouseDx_, mouseDy_};
    mouseDx_ = mouseDy_ = 0;
    return d;
}

Vec2 Input::padLook() const {
    if (!pad_) return {0, 0};
    float x = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
    float y = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
    Vec2 s = applyRadialDeadzone(x, y, deadzoneRight_);
    float m = std::sqrt(s.x * s.x + s.y * s.y);
    if (m > 0) {
        float curved = std::pow(m, lookCurve_);
        s.x *= curved / m;
        s.y *= curved / m;
    }
    return s;
}

void Input::rumble(float low, float high, int ms) {
    if (!pad_ || !rumbleEnabled_) return;
    float k = rumbleStrength_;
    SDL_GameControllerRumble(pad_, Uint16(clampf(low * k, 0, 1) * 65535), Uint16(clampf(high * k, 0, 1) * 65535), Uint32(ms));
}

} // namespace ca
