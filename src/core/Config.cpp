#include "core/Config.h"
#include "core/MathUtil.h"
#include <SDL.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ca {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void Ini::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        data_[section][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
}

bool Ini::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    parse(ss.str());
    return true;
}

std::string Ini::serialize() const {
    std::string out = "# Coded Arms PC port - configuration. Edit freely; invalid values are clamped.\n";
    for (const auto& [sec, kv] : data_) {
        out += "\n[" + sec + "]\n";
        for (const auto& [k, v] : kv) out += k + " = " + v + "\n";
    }
    return out;
}

bool Ini::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << serialize();
    return bool(f);
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    auto s = data_.find(section);
    if (s == data_.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    data_[section][key] = value;
}

static float getF(const Ini& ini, const char* s, const char* k, float def) {
    std::string v = ini.get(s, k, "");
    if (v.empty()) return def;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    return end != v.c_str() ? f : def;
}
static int getI(const Ini& ini, const char* s, const char* k, int def) { return int(getF(ini, s, k, float(def))); }
static bool getB(const Ini& ini, const char* s, const char* k, bool def) {
    std::string v = lower(ini.get(s, k, ""));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
static std::string fmtF(float f) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3g", f);
    return b;
}

static const char* kFilterNames[] = {"nearest", "bilinear", "trilinear"};
static const char* kFullscreenNames[] = {"windowed", "borderless", "exclusive"};

void Settings::fromIni(const Ini& ini) {
    width = getI(ini, "video", "width", width);
    height = getI(ini, "video", "height", height);
    std::string fs = lower(ini.get("video", "fullscreen", kFullscreenNames[int(fullscreen)]));
    for (int i = 0; i < 3; ++i) if (fs == kFullscreenNames[i] || fs == std::to_string(i)) fullscreen = FullscreenMode(i);
    vsync = getB(ini, "video", "vsync", vsync);
    fpsCap = getI(ini, "video", "fps_cap", fpsCap);
    fovHorizontal16x9 = getF(ini, "video", "fov", fovHorizontal16x9);
    std::string tf = lower(ini.get("video", "texture_filter", kFilterNames[int(textureFilter)]));
    for (int i = 0; i < 3; ++i) if (tf == kFilterNames[i]) textureFilter = TextureFilter(i);
    anisotropy = getI(ini, "video", "anisotropy", anisotropy);
    msaa = getI(ini, "video", "msaa", msaa);
    hudClamp16x9 = getB(ini, "video", "hud_clamp_16x9", hudClamp16x9);
    hudScale = getF(ini, "video", "hud_scale", hudScale);

    masterVolume = getF(ini, "audio", "master", masterVolume);
    sfxVolume = getF(ini, "audio", "sfx", sfxVolume);
    musicVolume = getF(ini, "audio", "music", musicVolume);

    mouseSensX = getF(ini, "input", "mouse_sens_x", mouseSensX);
    mouseSensY = getF(ini, "input", "mouse_sens_y", mouseSensY);
    mouseInvertX = getB(ini, "input", "mouse_invert_x", mouseInvertX);
    mouseInvertY = getB(ini, "input", "mouse_invert_y", mouseInvertY);
    padSensX = getF(ini, "input", "pad_sens_x", padSensX);
    padSensY = getF(ini, "input", "pad_sens_y", padSensY);
    padInvertX = getB(ini, "input", "pad_invert_x", padInvertX);
    padInvertY = getB(ini, "input", "pad_invert_y", padInvertY);
    deadzoneLeft = getF(ini, "input", "deadzone_left", deadzoneLeft);
    deadzoneRight = getF(ini, "input", "deadzone_right", deadzoneRight);
    padLookCurve = getF(ini, "input", "pad_look_curve", padLookCurve);
    rumble = getB(ini, "input", "rumble", rumble);
    rumbleStrength = getF(ini, "input", "rumble_strength", rumbleStrength);

    showOverlay = getB(ini, "debug", "show_overlay", showOverlay);

    auto it = ini.sections().find("bindings");
    if (it != ini.sections().end())
        for (const auto& [k, v] : it->second) bindings[k] = v;
    clampValues();
}

void Settings::toIni(Ini& ini) const {
    ini.set("video", "width", std::to_string(width));
    ini.set("video", "height", std::to_string(height));
    ini.set("video", "fullscreen", kFullscreenNames[int(fullscreen)]);
    ini.set("video", "vsync", vsync ? "1" : "0");
    ini.set("video", "fps_cap", std::to_string(fpsCap));
    ini.set("video", "fov", fmtF(fovHorizontal16x9));
    ini.set("video", "texture_filter", kFilterNames[int(textureFilter)]);
    ini.set("video", "anisotropy", std::to_string(anisotropy));
    ini.set("video", "msaa", std::to_string(msaa));
    ini.set("video", "hud_clamp_16x9", hudClamp16x9 ? "1" : "0");
    ini.set("video", "hud_scale", fmtF(hudScale));

    ini.set("audio", "master", fmtF(masterVolume));
    ini.set("audio", "sfx", fmtF(sfxVolume));
    ini.set("audio", "music", fmtF(musicVolume));

    ini.set("input", "mouse_sens_x", fmtF(mouseSensX));
    ini.set("input", "mouse_sens_y", fmtF(mouseSensY));
    ini.set("input", "mouse_invert_x", mouseInvertX ? "1" : "0");
    ini.set("input", "mouse_invert_y", mouseInvertY ? "1" : "0");
    ini.set("input", "pad_sens_x", fmtF(padSensX));
    ini.set("input", "pad_sens_y", fmtF(padSensY));
    ini.set("input", "pad_invert_x", padInvertX ? "1" : "0");
    ini.set("input", "pad_invert_y", padInvertY ? "1" : "0");
    ini.set("input", "deadzone_left", fmtF(deadzoneLeft));
    ini.set("input", "deadzone_right", fmtF(deadzoneRight));
    ini.set("input", "pad_look_curve", fmtF(padLookCurve));
    ini.set("input", "rumble", rumble ? "1" : "0");
    ini.set("input", "rumble_strength", fmtF(rumbleStrength));

    ini.set("debug", "show_overlay", showOverlay ? "1" : "0");

    for (const auto& [k, v] : bindings) ini.set("bindings", k, v);
}

void Settings::clampValues() {
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (fpsCap < 0) fpsCap = 0;
    fovHorizontal16x9 = clampf(fovHorizontal16x9, 50.0f, 120.0f);
    anisotropy = std::max(1, std::min(16, anisotropy));
    if (msaa != 0 && msaa != 2 && msaa != 4 && msaa != 8) msaa = 0;
    hudScale = clampf(hudScale, 0.5f, 2.0f);
    masterVolume = clampf(masterVolume, 0, 1);
    sfxVolume = clampf(sfxVolume, 0, 1);
    musicVolume = clampf(musicVolume, 0, 1);
    mouseSensX = clampf(mouseSensX, 0.05f, 50.0f);
    mouseSensY = clampf(mouseSensY, 0.05f, 50.0f);
    padSensX = clampf(padSensX, 0.1f, 5.0f);
    padSensY = clampf(padSensY, 0.1f, 5.0f);
    deadzoneLeft = clampf(deadzoneLeft, 0.0f, 0.9f);
    deadzoneRight = clampf(deadzoneRight, 0.0f, 0.9f);
    padLookCurve = clampf(padLookCurve, 1.0f, 4.0f);
    rumbleStrength = clampf(rumbleStrength, 0.0f, 1.0f);
}

std::string resolveConfigPath() {
    std::string dir;
    if (char* base = SDL_GetBasePath()) {
        dir = base;
        SDL_free(base);
        std::string probe = dir + ".write_test";
        if (FILE* f = std::fopen(probe.c_str(), "w")) {
            std::fclose(f);
            std::remove(probe.c_str());
            return dir + "config.ini";
        }
    }
    if (char* pref = SDL_GetPrefPath("CodedArmsContagionPC", "CodedArmsContagion")) {
        dir = pref;
        SDL_free(pref);
    }
    return dir + "config.ini";
}

} // namespace ca
