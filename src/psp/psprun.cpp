// ca_psprun: headless runner for the recompiled game (development tool).
//   ca_psprun --iso CodedArms.iso [--trace] [--seconds N] [--wav out.wav]
//             [--press BUTTON@SECONDS[:HOLD]]... [--memstick DIR] [--gl [--scale N] [--shot SECONDS:FILE.png]...]
// --gl renders the GE output with OpenGL in a hidden window (works under Xvfb) and --shot
// saves the presented frame at the given times (for comparison with reference frames).
// --memstick (or CA_MEMSTICK) is the host folder mapped to ms0:/ (default ./memstick).
// System dialogs (savedata, messages) have no screen here: they are logged, and answered by the
// policy in CA_DIALOG_POLICY after CA_DIALOG_DELAY_MS (default 500) of being shown:
//   default  - the dialog's default: the highlighted Yes/No choice, the highlighted savedata
//              entry, OK; a screen with only a Back button is closed with it
//   yes / no - choose Yes / No in yes/no questions, otherwise as default
//   cancel   - press the cancel button
//   none     - never answer
//   buttons  - no policy: the dialogs read the emulated controller like PPSSPP (use --press)
// --wav consumes the guest audio channels in real time like a host mixer would and writes
// the mix to a 44.1 kHz stereo WAV file (for checking sound output without a window).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <map>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "psp/ge.h"
#include "psp/gpu/gpu.h"
#include "psp/gpu/button_glyphs.h"
#include "psp/gpu/minimap.h"
#include "render/GL.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "psp/host.h"
#include "psp/memory.h"
#include "psp/recomp.h"
#include "psp/host_dialog.h"
#include "psp/kernel.h"

namespace psp::kernel {
extern std::string g_hostMsRoot;
}

namespace {
FILE* g_wav = nullptr;
uint32_t g_wavFrames = 0;

void writeWavHeader(FILE* f, uint32_t frames) {
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f); u32(36 + frames * 4); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(frames * 4);
    std::fseek(f, 0, SEEK_END);
}

// Every 10 ms takes 441 frames from each channel (silence where a channel ran dry), mixes
// them and appends them to the WAV file.
void wavThread() {
    using namespace psp::host;
    auto next = std::chrono::steady_clock::now();
    std::vector<int16_t> out(441 * 2);
    for (;;) {
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
        std::vector<int> mix(441 * 2, 0);
        {
            std::lock_guard<std::mutex> lk(g_audioMutex);
            for (auto& ch : g_audio) {
                size_t n = std::min<size_t>(ch.queue.size(), mix.size());
                for (size_t i = 0; i < n; ++i) mix[i] += ch.queue[i];
                ch.queue.erase(ch.queue.begin(), ch.queue.begin() + (long)n);
            }
        }
        for (size_t i = 0; i < mix.size(); ++i) out[i] = (int16_t)std::clamp(mix[i], -32768, 32767);
        std::fwrite(out.data(), 2, out.size(), g_wav);
        g_wavFrames += 441;
    }
}

// CA_GE_STATS=1: counts GE commands, primitive/vertex types and texture/framebuffer formats
// and prints them at exit (scopes what the renderer must support).
struct GeStats : psp::ge::Renderer {
    std::mutex m;
    std::map<std::string, uint64_t> counts;
    void bump(const char* fmt, uint32_t v) { char b[64]; std::snprintf(b, sizeof b, fmt, v); counts[b]++; }
    void command(uint32_t, uint32_t cmd, uint32_t data, uint32_t, uint32_t) override {
        std::lock_guard<std::mutex> lk(m);
        bump("cmd %02x", cmd);
        switch (cmd) {
        case 0x04: bump("prim type %u", (data >> 16) & 7);
            bump("prim vtype %06x", psp::ge::reg(0x12));
            bump("prim texfmt %u", psp::ge::reg(0x1E) ? psp::ge::reg(0xC3) & 0xF : 99);
            bump("prim clear %u", psp::ge::reg(0xD3) & 1);
            bump("prim fbfmt %u", psp::ge::reg(0xD2) & 3);
            bump("prim fb %06x", psp::ge::reg(0x9C));
            bump("prim tex0 %06x", psp::ge::reg(0x1E) ? psp::ge::reg(0xA0) : 0);
            bump("prim blend %06x", psp::ge::reg(0x21) ? psp::ge::reg(0xDF) : 0xFFFFFF);
            bump("prim lighting %u", psp::ge::reg(0x17) & 1);
            bump("prim fog %u", psp::ge::reg(0x1F) & 1);
            bump("prim stencil %u", psp::ge::reg(0x24) & 1);
            bump("prim texfunc %06x", psp::ge::reg(0xC9));
            bump("prim texmap %06x", psp::ge::reg(0xC0));
            break;
        case 0x05: case 0x06: bump("patch %u", cmd); break;
        case 0xC4: bump("clutfmt %06x", psp::ge::reg(0xC5)); break;
        case 0xEA: bump("transfer bpp %u", data & 1);
            bump("transfer src %06x", psp::ge::reg(0xB2)); bump("transfer dst %06x", psp::ge::reg(0xB4)); break;
        }
    }
    ~GeStats() override {}
};
GeStats* g_geStats = nullptr;

struct Press { uint32_t buttons; double at, hold; };

void logDialog(const psp::host::DialogView& v) {
    using namespace psp::host;
    std::string text = v.text;
    for (char& ch : text) if (ch == '\n') ch = '|';
    std::string extra;
    if (v.yesNo) extra += v.yesNoChoice ? " [YES] no" : " yes [NO]";
    if (v.ok) extra += " [OK]";
    if (v.busy) extra += " (busy)";
    if (v.confirmButton) extra += " confirm=" + v.confirmLabel;
    if (v.cancelButton) extra += " cancel=" + v.cancelLabel;
    std::fprintf(stderr, "[DIALOG] %s%s%s: \"%s\"%s\n", v.kind == DialogKind::Message ? "message" : "savedata",
                 v.banner.empty() ? "" : " ", v.banner.c_str(), text.c_str(), extra.c_str());
    for (size_t i = 0; i < v.entries.size(); ++i) {
        const DialogSaveEntry& e = v.entries[i];
        std::fprintf(stderr, "[DIALOG]   %c %s: %s / %s (%lld bytes%s, icon %zu bytes)\n", (int)i == v.selected ? '>' : ' ',
                     e.saveName.c_str(), e.title.c_str(), e.savedataTitle.c_str(), (long long)e.sizeBytes, e.broken ? ", broken" : "",
                     e.iconPng.size());
    }
    if (v.showsEntry)
        std::fprintf(stderr, "[DIALOG]   slot %s: %s / %s (%lld bytes, icon %zu bytes)\n", v.entry.saveName.c_str(), v.entry.title.c_str(),
                     v.entry.savedataTitle.c_str(), (long long)v.entry.sizeBytes, v.entry.iconPng.size());
}

void installDialogPolicy() {
    using namespace psp::host;
    const char* p = std::getenv("CA_DIALOG_POLICY");
    std::string policy = p ? p : "default";
    const char* d = std::getenv("CA_DIALOG_DELAY_MS");
    uint64_t delayUs = (uint64_t)(d ? std::atoi(d) : 500) * 1000;
    setDialogObserver(logDialog);
    if (policy == "buttons") { setDialogUsesController(true); return; }
    if (policy == "none") return;
    setDialogAutoResponder([policy, delayUs](const DialogView& v, uint64_t visibleUs, DialogInput& in) {
        if (visibleUs < delayUs || v.fade < 255) return false;
        if (policy == "cancel") {
            if (!v.cancelButton) return false;
            in.cancel = true;
        } else if (v.yesNo) {
            in.yesNoChoice = policy == "yes" ? 1 : (policy == "no" ? 0 : v.yesNoChoice);
            in.confirm = true;
        } else if (v.confirmButton) {
            in.select = v.selected;
            in.confirm = true;
        } else if (v.cancelButton) {
            in.cancel = true; // result screens ("Save completed") close with the cancel button
        } else {
            return false;
        }
        std::fprintf(stderr, "[DIALOG] auto answer (%s): %s%s\n", policy.c_str(), in.confirm ? "confirm" : "cancel",
                     in.yesNoChoice < 0 ? "" : (in.yesNoChoice ? " yes" : " no"));
        return true;
    });
}

uint32_t buttonByName(const std::string& n) {
    using namespace psp::host;
    static const std::pair<const char*, uint32_t> names[] = {
        {"SELECT", BTN_SELECT}, {"START", BTN_START}, {"UP", BTN_UP}, {"RIGHT", BTN_RIGHT},
        {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"L", BTN_LTRIGGER}, {"R", BTN_RTRIGGER},
        {"TRIANGLE", BTN_TRIANGLE}, {"CIRCLE", BTN_CIRCLE}, {"CROSS", BTN_CROSS}, {"SQUARE", BTN_SQUARE}};
    for (auto& [name, bit] : names) if (n == name) return bit;
    return 0;
}

// Plays scripted button presses (debugging aid: lets headless runs get past menus).
void pressThread(std::vector<Press> presses) {
    auto t0 = std::chrono::steady_clock::now();
    std::sort(presses.begin(), presses.end(), [](const Press& a, const Press& b) { return a.at < b.at; });
    for (const Press& p : presses) {
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds((long long)(p.at * 1000)));
        psp::host::g_ctrl.buttons |= p.buttons;
        std::fprintf(stderr, "[RUN] press %04x at %.2fs\n", p.buttons, p.at);
        std::this_thread::sleep_for(std::chrono::milliseconds((long long)(p.hold * 1000)));
        psp::host::g_ctrl.buttons &= ~p.buttons;
    }
}
} // namespace

// Runs the game on a worker thread and the renderer on this one, in a hidden GL window.
// Interactive control (--control FIFO): one command per line, "press BUTTON[+BUTTON...] [SECONDS]",
// "hold BUTTONS", "release", "stick X Y" (0..255), "shot FILE.png", "quit".
float g_aspect = 0;
std::mutex g_ctlMutex;
std::vector<std::string> g_ctlShots;
void controlThread(std::string path) {
    for (;;) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            char cmd[32] = {}, arg[256] = {};
            double secs = 0.15;
            int n = std::sscanf(l.c_str(), "%31s %255s %lf", cmd, arg, &secs);
            if (n < 1) continue;
            std::string c = cmd;
            auto mask = [](const std::string& a) {
                uint32_t m = 0;
                size_t p = 0;
                while (p <= a.size()) {
                    size_t q = a.find('+', p);
                    m |= buttonByName(a.substr(p, q == std::string::npos ? std::string::npos : q - p));
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
                return m;
            };
            if (c == "press") {
                uint32_t m = mask(arg);
                psp::host::g_ctrl.buttons |= m;
                std::this_thread::sleep_for(std::chrono::milliseconds((long long)(secs * 1000)));
                psp::host::g_ctrl.buttons &= ~m;
            } else if (c == "hold") {
                psp::host::g_ctrl.buttons |= mask(arg);
            } else if (c == "release") {
                psp::host::g_ctrl.buttons = 0;
                psp::host::g_ctrl.lx = 128;
                psp::host::g_ctrl.ly = 128;
            } else if (c == "stick") {
                int x = 128, y = 128;
                std::sscanf(l.c_str(), "%*s %d %d", &x, &y);
                psp::host::g_ctrl.lx = (uint8_t)x;
                psp::host::g_ctrl.ly = (uint8_t)y;
            } else if (c == "profdump") { // profdump FILE: calls per function since the last dump (profile build)
                if (FILE* o = std::fopen(arg, "w")) {
                    for (uint32_t i = 0; i < 0x400000 / 4; ++i)
                        if (g_prof_counts[i]) std::fprintf(o, "%08x %u\n", 0x08804000u + i * 4, g_prof_counts[i]);
                    std::fclose(o);
                }
                std::memset(g_prof_counts, 0, sizeof(uint32_t) * (0x400000 / 4));
            } else if (c == "poke8") { // poke8 ADDR VALUE (hex address)
                unsigned a = 0, v = 0;
                if (std::sscanf(l.c_str(), "%*s %x %u", &a, &v) == 2) psp::write8(a, (uint8_t)v);
            } else if (c == "dev") { // dev 0|1 [w0 w1 w2 w3 w4]: dev mode loadout
                int v[6] = {0, 0, 4, 6, 19, 23};
                int n2 = std::sscanf(l.c_str(), "%*s %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
                psp::host::g_dev.enabled = v[0] != 0;
                if (n2 == 6) for (int i = 0; i < 5; ++i) psp::host::g_dev.loadout[i] = v[i + 1];
            } else if (c == "pause") { // pause 0|1: the front end menus' pause
                psp::kernel::setPaused(std::atoi(arg) != 0);
            } else if (c == "devcycle") { // devcycle 1|-1: next / previous weapon in place of the current one
                psp::host::g_dev.cycle = std::atoi(arg);
            } else if (c == "ads") { // ads 0|1: aim down sights
                psp::host::g_ads = std::atoi(arg) != 0;
            } else if (c == "mapshot") { // mapshot FILE: the minimap (256 px, 150 units)
                std::vector<uint32_t> px;
                if (psp::gpu::minimap::render(px, 256, 150.0f)) stbi_write_png(arg, 256, 256, 4, px.data(), 256 * 4);
                else std::fprintf(stderr, "[CTL] mapshot: no map\n");
            } else if (c == "shot") {
                std::lock_guard<std::mutex> lk(g_ctlMutex);
                g_ctlShots.push_back(arg);
            } else if (c == "gpudump") {
                psp::gpu::dumpNextFrames(n >= 2 && std::atoi(arg) > 0 ? std::atoi(arg) : 1);
            } else if (c == "aim") {
                float y = 0, p = 0;
                std::sscanf(l.c_str(), "%*s %f %f", &y, &p);
                std::lock_guard<std::mutex> lk(psp::host::g_aim.m);
                psp::host::g_aim.enabled = true;
                psp::host::g_aim.yaw += y;
                psp::host::g_aim.pitch += p;
            } else if (c == "memdump") {
                // User RAM 0x08800000..0x0A000000 to a file
                if (FILE* o = std::fopen(arg, "wb")) {
                    std::fwrite(psp::hostPtr(0x08800000), 1, 0x01800000, o);
                    std::fclose(o);
                }
            } else if (c == "skipfbtex") {
                psp::gpu::setSkipFramebufferTextures(std::atoi(arg) != 0);
            } else if (c == "fps") { // frame rate: 60, 30 or 0 (the game's)
                psp::host::g_targetFps = std::atoi(arg);
            } else if (c == "maxdraw") {
                psp::gpu::setMaxDraws(std::atoi(arg));
            } else if (c == "quit") {
                std::_Exit(0);
            }
            std::fprintf(stderr, "[CTL] %s\n", l.c_str());
        }
        std::fclose(f);
    }
}

int runWithGl(const psp::kernel::BootOptions& opt, int scale, std::vector<std::pair<double, std::string>> shots) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("ca_psprun", 0, 0, 480, 272, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { std::fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
    const char* missing = nullptr;
    if (!ca::loadGL(&missing)) { std::fprintf(stderr, "missing GL function %s\n", missing); return 1; }
    std::fprintf(stderr, "[RUN] GL: %s / %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    psp::gpu::Settings gs;
    gs.scale = scale;
    gs.wideAspect = g_aspect;
    psp::gpu::install(gs);
    if (const char* g = std::getenv("CA_GLYPH_LABELS")) { // cross,circle,square,triangle,l,r,start,select
        std::vector<std::string> v(1);
        for (const char* p = g; *p; ++p) {
            if (*p == ',') v.emplace_back();
            else v.back() += *p == '_' ? psp::gpu::kGlyphEnter : *p;
        }
        v.resize(8);
        psp::gpu::setButtonLabels({v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]});
    }
    if (!psp::gpu::renderInit()) return 1;
    std::thread([opt] { std::_Exit(psp::kernel::boot(opt)); }).detach();
    std::sort(shots.begin(), shots.end());
    auto t0 = std::chrono::steady_clock::now();
    psp::gpu::PresentInfo last;
    bool have = false;
    uint64_t frames = 0;
    for (;;) {
        psp::gpu::PresentInfo p;
        if (psp::gpu::renderPump(p, 5)) { last = p; have = true; ++frames; }
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(g_ctlMutex);
            for (auto& f : g_ctlShots) shots.insert(shots.begin(), {0.0, f});
            g_ctlShots.clear();
        }
        while (!shots.empty() && t >= shots.front().first) {
            std::vector<uint32_t> px;
            int w = 0, h = 0;
            if (have && psp::gpu::readPresented(last, px, w, h)) {
                stbi_write_png(shots.front().second.c_str(), w, h, 4, px.data(), w * 4);
                std::fprintf(stderr, "[RUN] shot %s at %.2fs (frame %llu, %llu presented)\n", shots.front().second.c_str(), t,
                             (unsigned long long)last.frame, (unsigned long long)frames);
            } else {
                std::fprintf(stderr, "[RUN] shot %s at %.2fs: nothing presented yet\n", shots.front().second.c_str(), t);
            }
            shots.erase(shots.begin());
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
    }
}

int main(int argc, char** argv) {
    psp::kernel::BootOptions opt;
    if (const char* ms = std::getenv("CA_MEMSTICK")) psp::kernel::g_hostMsRoot = ms;
    int seconds = 0;
    std::vector<Press> presses;
    bool gl = false;
    int scale = 1;
    std::vector<std::pair<double, std::string>> shots;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--iso") && i + 1 < argc) opt.isoPath = argv[++i];
        else if (!std::strcmp(argv[i], "--trace")) opt.traceHle = true;
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--gl")) gl = true;
        else if (!std::strcmp(argv[i], "--control") && i + 1 < argc) std::thread(controlThread, std::string(argv[++i])).detach();
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--aspect") && i + 1 < argc) g_aspect = (float)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t c = spec.find(':');
            if (c == std::string::npos) { std::fprintf(stderr, "bad --shot %s\n", spec.c_str()); return 2; }
            shots.push_back({std::atof(spec.substr(0, c).c_str()), spec.substr(c + 1)});
        }
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t at = spec.find('@'), colon = spec.find(':');
            Press p{buttonByName(spec.substr(0, at)), 0, 0.15};
            if (at == std::string::npos || !p.buttons) { std::fprintf(stderr, "bad --press %s\n", spec.c_str()); return 2; }
            p.at = std::atof(spec.c_str() + at + 1);
            if (colon != std::string::npos) p.hold = std::atof(spec.c_str() + colon + 1);
            presses.push_back(p);
        }
        else if (!std::strcmp(argv[i], "--memstick") && i + 1 < argc) psp::kernel::g_hostMsRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) {
            g_wav = std::fopen(argv[++i], "wb");
            if (g_wav) { writeWavHeader(g_wav, 0); std::thread(wavThread).detach(); }
        }
    }
    if (opt.isoPath.empty()) {
        std::fprintf(stderr, "usage: ca_psprun --iso CodedArms.iso [--trace] [--seconds N] [--wav out.wav]\n");
        return 2;
    }
    if (seconds > 0) {
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::fprintf(stderr, "[RUN] time limit reached\n");
            if (g_geStats) {
                std::lock_guard<std::mutex> lk(g_geStats->m);
                for (auto& [k, v] : g_geStats->counts) std::fprintf(stderr, "[GE] %-24s %llu\n", k.c_str(), (unsigned long long)v);
            }
            if (g_wav) { writeWavHeader(g_wav, g_wavFrames); std::fflush(g_wav); }
            std::fflush(stderr);
            std::_Exit(0);
        }).detach();
    }
    if (!psp::kernel::g_hostMsRoot.empty() && psp::kernel::g_hostMsRoot.back() != '/') psp::kernel::g_hostMsRoot += '/';
    installDialogPolicy();
    if (!presses.empty()) std::thread(pressThread, presses).detach();
    if (gl) return runWithGl(opt, scale, shots);
    if (std::getenv("CA_GE_STATS")) { g_geStats = new GeStats; psp::ge::setRenderer(g_geStats); }
    return psp::kernel::boot(opt);
}
