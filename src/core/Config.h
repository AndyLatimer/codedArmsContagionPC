#pragma once
// User settings persisted to an INI file (config.ini). Every value the player can
// change lives here so the options menu, the command line and the file agree.
#include <map>
#include <string>

namespace ca {

// Raw INI document: section -> key -> value (all lowercase keys).
class Ini {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void parse(const std::string& text);
    std::string serialize() const;

    std::string get(const std::string& section, const std::string& key, const std::string& def) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    const std::map<std::string, std::map<std::string, std::string>>& sections() const { return data_; }

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

enum class FullscreenMode { Windowed = 0, Borderless = 1, Exclusive = 2 };
enum class TextureFilter { Nearest = 0, Bilinear = 1, Trilinear = 2 };

struct Settings {
    // [video]
    int width = 1280, height = 720;          // window size; 0 = desktop resolution in fullscreen
    FullscreenMode fullscreen = FullscreenMode::Windowed;
    bool vsync = true;
    int fpsCap = 0;                          // 0 = unlimited (only used when vsync is off)
    float fovHorizontal16x9 = 75.0f;         // Hor+: wider screens see more, vertical FOV is constant
    TextureFilter textureFilter = TextureFilter::Trilinear;
    int anisotropy = 8;
    int msaa = 4;                            // requires restart
    bool hudClamp16x9 = true;                // keep HUD inside a 16:9 region on ultrawide
    float hudScale = 1.0f;

    // [audio] 0..1
    float masterVolume = 0.8f, sfxVolume = 1.0f, musicVolume = 0.7f;

    // [input]
    float mouseSensX = 3.0f, mouseSensY = 3.0f;
    bool mouseInvertY = false, mouseInvertX = false;
    float padSensX = 1.0f, padSensY = 0.8f;   // multiplier of base turn rate
    bool padInvertY = false, padInvertX = false;
    float deadzoneLeft = 0.20f, deadzoneRight = 0.15f;
    float padLookCurve = 1.6f;                // response exponent for the look stick
    bool rumble = true;
    float rumbleStrength = 1.0f;

    // [debug]
    bool showOverlay = false;

    // [bindings] action name -> comma separated binding list
    std::map<std::string, std::string> bindings;

    void fromIni(const Ini& ini);
    void toIni(Ini& ini) const;
    void clampValues();
};

// Locates the config file: next to the executable when writable (portable install),
// otherwise in the per-user preferences folder.
std::string resolveConfigPath();

} // namespace ca
