#include "psp/gpu/gpu.h"
#include "psp/gpu/ads.h"
#include "psp/gpu/button_glyphs.h"
#include "psp/gpu/minimap.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "psp/ge.h"
#include "psp/gpu/ge_state.h"
#include "psp/gpu/shaders.h"
#include "psp/gpu/texture_decoder.h"
#include "psp/gpu/vertex_decoder.h"
#include "psp/kernel.h"
#include "psp/memory.h"
#include "render/GL.h"

extern "C" int stbi_write_png(char const* filename, int w, int h, int comp, const void* data, int stride_in_bytes);

namespace psp::gpu {

namespace {

// CA_PERF=1: per-second timing of the renderer stages (microseconds) on stderr.
struct PerfCounters {
    std::atomic<uint64_t> flips{0}, waitUs{0}, patchUs{0}, drawUs{0}, texUs{0}, execUs{0}, presents{0}, patches{0}, draws{0}, verts{0};
} g_perf;
const bool g_perfOn = std::getenv("CA_PERF") != nullptr;
struct ScopedUs {
    std::atomic<uint64_t>& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ScopedUs(std::atomic<uint64_t>& a) : acc(a) {}
    ~ScopedUs() { acc += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
};
void perfReport() {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    std::fprintf(stderr, "[PERF] flips=%llu queue-wait=%llums ", (unsigned long long)g_perf.flips.exchange(0), (unsigned long long)g_perf.waitUs.exchange(0) / 1000);
    std::fprintf(stderr, "presents=%llu draws=%llu patches=%llu verts=%llu | draw=%llums patch=%llums tex=%llums exec=%llums\n",
                 (unsigned long long)g_perf.presents.exchange(0), (unsigned long long)g_perf.draws.exchange(0),
                 (unsigned long long)g_perf.patches.exchange(0), (unsigned long long)g_perf.verts.exchange(0),
                 (unsigned long long)g_perf.drawUs.exchange(0) / 1000, (unsigned long long)g_perf.patchUs.exchange(0) / 1000,
                 (unsigned long long)g_perf.texUs.exchange(0) / 1000, (unsigned long long)g_perf.execUs.exchange(0) / 1000);
}

// ---- data shared by recorder and executor ------------------------------------------------

// Uniform block (std140: only vec4/mat4 members). Must match kUboSource in shaders.h.
struct Ubo {
    float world[16], view[16], proj[16], tgen[16];
    float vpScale[4];    // xyz scale, w = through mode
    float vpCenter[4];   // xyz center
    float offsetFb[4];   // offset x/y (pixels), fb width/height (pixels)
    float matAmbient[4], matDiffuse[4], matSpecular[4], matEmissive[4], ambient[4];
    float lightPos[4][4], lightDir[4][4], lightAtt[4][4];
    float lightAmb[4][4], lightDiff[4][4], lightSpec[4][4];
    float lightParams[4][4]; // enabled, type, comp, -
    float lightSpot[4][4];   // cutoff, exponent
    float flags0[4];         // lighting, hasColor, materialUpdate, separateSpecular
    float flags1[4];         // specCoef, reverseNormal, fog, texMapMode
    float texgen[4];         // projSrc, ls0, ls1, textured
    float texScaleOff[4];
    float texSize[4];        // w, h (texels), uv scale x/y (framebuffer textures)
    float fog[4];            // end, slope
    float fogColor[4];
    float texEnv[4];         // env color rgb, texture function
    float texFlags[4];       // alpha used, color doubling, clear mode, flat shading
    float alphaTest[4];      // enabled, func, ref, mask
    float colorTest[4];      // enabled, func, -, -
    float colorRef[4];
    float colorMask[4];
    float depthRange[4];     // minz, maxz, enabled
    float blendPremul[4];    // rgb factor, enabled
    float wide[4];           // x shift (pixels), stretch 480 -> full width, -, texture u offset
};

struct GlState {
    bool blend = false;
    bool alphaConst = false; // write blendColor[3] to the alpha channel (stencil value)
    uint32_t srcRGB = GL_ONE, dstRGB = GL_ZERO, eqRGB = GL_FUNC_ADD;
    float blendColor[4] = {0, 0, 0, 0};
    bool depthTest = false, depthWrite = false, depthClamp = false;
    uint32_t depthFunc = GL_ALWAYS;
    bool stencil = false;
    uint32_t stencilFunc = GL_ALWAYS, sfail = GL_KEEP, zfail = GL_KEEP, zpass = GL_KEEP;
    int stencilRef = 0, stencilMask = 0xFF, stencilWrite = 0xFF;
    bool cull = false;
    uint32_t cullFace = GL_BACK;
    bool colorMask[4] = {true, true, true, true};
    int scissor[4] = {0, 0, 480, 272}; // x, y, w, h in framebuffer pixels
    bool logic = false;
    uint32_t logicOp = GL_COPY;
    int texKind = 0; // 0 none, 1 cached texture, 2 framebuffer
    int texId = 0;
    uint32_t minFilter = GL_NEAREST, magFilter = GL_NEAREST, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    int maxLevel = 0;
};

enum OpType { OP_CLEAR_FBO, OP_CREATE_FBO, OP_BIND_FBO, OP_UPLOAD_TEX, OP_UPLOAD_FBO_RECT, OP_DRAW, OP_BLIT_FBO, OP_PRESENT, OP_DELETE_TEX };

struct Op {
    OpType type;
    int id = 0, id2 = 0;
    int x = 0, y = 0, w = 0, h = 0, x2 = 0, y2 = 0;
    uint32_t glMode = 0;
    size_t first = 0, count = 0;  // vertices (draw)
    size_t blob = 0, blobSize = 0; // pixel data offset/size in Frame::blob
    size_t ubo = 0;                // index into Frame::ubos
    GlState st;
    std::vector<std::pair<int, int>> levels; // texture upload: per-level w/h (data consecutive)
};

struct Frame {
    std::vector<Op> ops;
    std::vector<float> verts; // 12 floats per vertex
    std::vector<uint8_t> blob;
    std::vector<Ubo> ubos;
    uint64_t number = 0;
};

constexpr int kFloatsPerVertex = 12;

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<std::unique_ptr<Frame>> g_queue;
Settings g_settings;
bool g_installed = false;

// ---- recorder (guest threads) -----------------------------------------------------------

uint64_t hashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

bool isVram(uint32_t addr) { return (addr & 0x0F000000) == 0x04000000; }
uint32_t vramKey(uint32_t addr) { return addr & 0x001FFFFF; }

struct FboInfo {
    int id = 0;
    uint32_t key = 0, stride = 512, format = 3;
    int w = 480, h = 272;
    bool drawnSincePresent = false;
    bool created = false;
    uint64_t memHash = 0; // guest memory of the buffer when last synced; the GE never writes it
    int extra = 0;        // widescreen: pixels added on each side of a 480x272 display buffer
    int glW() const { return w + 2 * extra; }
};

struct TexEntry {
    int id = 0;
    uint64_t memHash = 0;
    uint64_t verifiedEpoch = ~0ull;
    int w = 0, h = 0;
};

uint32_t depthFuncGL(int f) {
    static const uint32_t t[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return t[f & 7];
}
uint32_t stencilOpGL(int o) {
    static const uint32_t t[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR, GL_KEEP, GL_KEEP};
    return t[o & 7];
}

struct TessResult { std::vector<Vertex> verts; int prim = 0; };

class Recorder : public ge::Renderer {
public:
    Recorder() { frame_ = std::make_unique<Frame>(); }

    void command(uint32_t pc, uint32_t cmd, uint32_t data, uint32_t prev, uint32_t rel) override;

    void setDisplay(uint32_t top, uint32_t stride, uint32_t fmt) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        ++g_perf.flips;
        displayAddr_ = top;
        displayStride_ = stride;
        displayFormat_ = fmt;
        present();
        vblanksSincePresent_ = 0;
    }
    void vblank() {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        // Games that draw into a single displayed buffer never call SetFrameBuf again.
        if (++vblanksSincePresent_ >= 2 && displayAddr_) {
            present();
            vblanksSincePresent_ = 0;
        }
    }
    void memoryWrite(uint32_t addr, uint32_t size) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        markMemoryWrite(addr, size);
        ++texEpoch_;
    }

private:
    std::recursive_mutex mutex_;
    std::unique_ptr<Frame> frame_;
    uint64_t frameNumber_ = 0;
    Matrices mtx_;
    DecodeContext dctx_;
    ClutState clut_;
    uint64_t clutHash_ = 0;
    uint32_t vaddr_ = 0, iaddr_ = 0;
    std::map<uint32_t, FboInfo> fbos_;
    int nextFboId_ = 1, nextTexId_ = 1;
    int boundFbo_ = 0;
    std::unordered_map<uint64_t, TexEntry> textures_;
    uint64_t texEpoch_ = 0;
    uint32_t displayAddr_ = 0, displayStride_ = 512, displayFormat_ = 3;
    int vblanksSincePresent_ = 0;
    bool premulBlend_ = false;
    bool alphaOverride_ = false;
    float alphaOut_ = 0;
    float premulColor_[3] = {1, 1, 1};
    // Pending CPU writes to VRAM: rectangles to refresh per framebuffer key.
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> memDirty_;

    uint32_t reg(int c) const { return ge::reg(c); }

    void markMemoryWrite(uint32_t addr, uint32_t size) {
        if (!isVram(addr)) return;
        uint32_t k = vramKey(addr);
        for (auto& [key, f] : fbos_) {
            uint32_t bpp = f.format == 3 ? 4 : 2;
            uint32_t bytes = f.stride * bpp * (uint32_t)f.h;
            if (k + size > key && k < key + bytes) memDirty_[key].push_back({std::max(k, key), std::min(k + size, key + bytes)});
        }
    }

    FboInfo& fboFor(uint32_t key, uint32_t stride, uint32_t fmt, int needW, int needH) {
        auto it = fbos_.find(key);
        if (it == fbos_.end()) {
            FboInfo f;
            f.id = nextFboId_++;
            f.key = key;
            f.stride = stride ? stride : 512;
            f.format = fmt;
            f.w = std::max(std::min<int>((int)f.stride, 480), needW);
            f.h = std::max(272, needH);
            // Display-sized buffers get extra width for wide screens (Hor+).
            if (g_settings.wideAspect > 0 && f.w == 480 && f.h == 272)
                f.extra = std::max(0, (int)std::lround((272.0 * g_settings.wideAspect - 480.0) / 2.0));
            it = fbos_.emplace(key, f).first;
        }
        FboInfo& f = it->second;
        f.stride = stride ? stride : f.stride;
        f.format = fmt;
        if (!f.created) f.memHash = memoryHash(f);
        if (!f.created || needW > f.w || needH > f.h) {
            f.w = std::max(f.w, std::min(needW, 1024));
            f.h = std::max(f.h, std::min(needH, 1024));
            Op op{OP_CREATE_FBO};
            op.id = f.id;
            op.w = f.glW();
            op.h = f.h;
            frame_->ops.push_back(op);
            f.created = true;
            if (boundFbo_ == f.id) boundFbo_ = 0;
        }
        return f;
    }

    uint64_t memoryHash(const FboInfo& f) const {
        uint32_t bytes = f.stride * (f.format == 3 ? 4 : 2) * (uint32_t)f.h;
        uint32_t a = 0x04000000 | f.key;
        if (!validAddress(a) || !validAddress(a + bytes - 1)) return 0;
        return hashBytes(hostPtr(a), bytes);
    }

    // Copies CPU-written VRAM regions into their framebuffer.
    void flushMemDirty(FboInfo& f) {
        auto it = memDirty_.find(f.key);
        if (it == memDirty_.end()) return;
        uint32_t bpp = f.format == 3 ? 4 : 2;
        uint32_t lo = ~0u, hi = 0;
        for (auto& r : it->second) { lo = std::min(lo, r.first); hi = std::max(hi, r.second); }
        memDirty_.erase(it);
        int y0 = (int)((lo - f.key) / (f.stride * bpp));
        int y1 = (int)((hi - f.key + f.stride * bpp - 1) / (f.stride * bpp));
        uploadRows(f, std::max(0, y0), std::min(f.h, y1));
    }

    void uploadRows(FboInfo& f, int y0, int y1) {
        if (y1 <= y0) return;
        const int w = f.w, h = y1 - y0;
        const uint32_t bpp = f.format == 3 ? 4 : 2;
        if (f.extra && y0 == 0 && y1 >= 272) {
            Op clr{OP_CLEAR_FBO};
            clr.id = f.id;
            frame_->ops.push_back(clr);
        }
        Op op{OP_UPLOAD_FBO_RECT};
        op.id = f.id;
        op.x = f.extra;
        op.y = y0;
        op.w = w;
        op.h = h;
        op.blob = frame_->blob.size();
        op.blobSize = (size_t)w * h * 4;
        frame_->blob.resize(op.blob + op.blobSize);
        uint32_t* dst = reinterpret_cast<uint32_t*>(frame_->blob.data() + op.blob);
        const uint32_t base = 0x04000000 | f.key;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint32_t a = base + ((uint32_t)(y0 + y) * f.stride + (uint32_t)x) * bpp;
                uint32_t c;
                switch (f.format) {
                case 0: c = convert565(read16(a)); break;
                case 1: c = convert5551(read16(a)); break;
                case 2: c = convert4444(read16(a)); break;
                default: c = read32(a); break;
                }
                dst[y * w + x] = c;
            }
        }
        frame_->ops.push_back(op);
    }

    AimDownSights ads_;
    // Debugging (CA_MAP_DEBUG): view matrices of the frame's 3D draws.
    std::map<std::vector<float>, int> dbgViews_;
    void present() {
        static const bool mapDebug = std::getenv("CA_MAP_DEBUG") != nullptr;
        if (mapDebug && !dbgViews_.empty()) {
            std::string line = "[MAPDBG]";
            for (auto& [v, n] : dbgViews_) {
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = -(v[3 * k] * v[9] + v[3 * k + 1] * v[10] + v[3 * k + 2] * v[11]);
                char b[160];
                std::snprintf(b, sizeof b, " [%d draws cam %.1f,%.1f,%.1f fwd %.2f,%.2f,%.2f]", n, c[0], c[1], c[2], v[2], v[5], v[8]);
                line += b;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            dbgViews_.clear();
        }
        if (displayAddr_ && isVram(displayAddr_)) {
            FboInfo& f = fboFor(vramKey(displayAddr_), displayStride_, displayFormat_, 480, 272);
            flushMemDirty(f);
            // The GE never writes guest memory, so a changed buffer in memory means the CPU drew
            // into it (e.g. a software-decoded frame): show the memory.
            if (!f.drawnSincePresent) {
                uint64_t h = memoryHash(f);
                if (h != f.memHash) {
                    uploadRows(f, 0, f.h);
                    f.memHash = h;
                }
            }
            Op op{OP_PRESENT};
            op.id = f.id;
            op.w = f.glW();
            op.h = 272;
            frame_->ops.push_back(op);
            for (auto& [k, fb] : fbos_) fb.drawnSincePresent = false;
        }
        frame_->number = ++frameNumber_;
        ads_.endFrame();
        minimap::endFrame();
        {
            ScopedUs waitPerf(g_perf.waitUs);
            std::unique_lock<std::mutex> lk(g_queueMutex);
            // Back-pressure: the guest waits for the renderer like for a real GPU.
            g_queueCv.wait_for(lk, std::chrono::milliseconds(200), [] { return g_queue.size() < 3; });
            g_queue.push_back(std::move(frame_));
        }
        g_queueCv.notify_all();
        frame_ = std::make_unique<Frame>();
        boundFbo_ = 0;
        ++texEpoch_;
    }

    // Target framebuffer of the next draw.
    FboInfo& prepareTarget() {
        uint32_t fbp = reg(CMD_FRAMEBUFPTR), fbw = reg(CMD_FRAMEBUFWIDTH);
        uint32_t key = (fbp & 0x1FFFF0);
        uint32_t stride = fbw & 0x7FC;
        uint32_t fmt = reg(CMD_FRAMEBUFPIXFORMAT) & 3;
        uint32_t s2 = reg(CMD_SCISSOR2);
        int needW = (int)(s2 & 0x3FF) + 1, needH = (int)((s2 >> 10) & 0x3FF) + 1;
        needW = std::min(needW, (int)std::max<uint32_t>(stride, 1));
        FboInfo& f = fboFor(key, stride, fmt, std::min(needW, 512), std::min(needH, 512));
        flushMemDirty(f);
        if (boundFbo_ != f.id) {
            Op op{OP_BIND_FBO};
            op.id = f.id;
            frame_->ops.push_back(op);
            boundFbo_ = f.id;
        }
        f.drawnSincePresent = true;
        return f;
    }

    void loadClut() {
        uint32_t addr = (reg(CMD_CLUTADDR) & 0x00FFFFF0) | ((reg(CMD_CLUTADDRUPPER) << 8) & 0x0F000000);
        uint32_t ld = reg(CMD_LOADCLUT);
        int blocks = (ld & 0x7F) == 0x40 ? 0x40 : (int)(ld & 0x3F);
        uint32_t bytes = std::min<uint32_t>(blocks * 32, sizeof clut_.data);
        if (bytes && validAddress(addr) && validAddress(addr + bytes - 1)) std::memcpy(clut_.data, hostPtr(addr), bytes);
        clutHash_ = hashBytes(clut_.data, sizeof clut_.data);
        ++texEpoch_;
    }

    bool setupTexture(GlState& st, Ubo& u, const FboInfo& target);
    void draw(uint32_t data);
    void drawPatch(uint32_t data, bool spline);
    // Tessellated patches by (control points, parameters): the level geometry is static, so
    // nearly every patch is the same every frame.
    std::unordered_map<uint64_t, std::shared_ptr<const TessResult>> patchCache_;
    void emit(std::vector<Vertex>& vin, int prim, bool through, bool hasColor);
    void transfer(uint32_t data);
    void buildUbo(Ubo& u, const FboInfo& f, bool through, bool hasColor);
    void buildState(GlState& st, const FboInfo& f, bool through, int prim);
};

void Recorder::command(uint32_t, uint32_t cmd, uint32_t data, uint32_t, uint32_t rel) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    switch (cmd) {
    case CMD_VADDR: vaddr_ = rel; break;
    case CMD_IADDR: iaddr_ = rel; break;
    case CMD_PRIM: draw(data); break;
    case CMD_WORLDMATRIXNUMBER: mtx_.worldNum = data & 0xF; break;
    case CMD_WORLDMATRIXDATA: if (mtx_.worldNum < 12) mtx_.world[mtx_.worldNum++] = float24(data); break;
    case CMD_VIEWMATRIXNUMBER: mtx_.viewNum = data & 0xF; break;
    case CMD_VIEWMATRIXDATA: if (mtx_.viewNum < 12) mtx_.view[mtx_.viewNum++] = float24(data); break;
    case CMD_PROJMATRIXNUMBER: mtx_.projNum = data & 0xF; break;
    case CMD_PROJMATRIXDATA: if (mtx_.projNum < 16) mtx_.proj[mtx_.projNum++] = float24(data); break;
    case CMD_TGENMATRIXNUMBER: mtx_.tgenNum = data & 0xF; break;
    case CMD_TGENMATRIXDATA: if (mtx_.tgenNum < 12) mtx_.tgen[mtx_.tgenNum++] = float24(data); break;
    case CMD_BONEMATRIXNUMBER: mtx_.boneNum = data & 0x7F; break;
    case CMD_BONEMATRIXDATA: if (mtx_.boneNum < 96) mtx_.bone[mtx_.boneNum++] = float24(data); break;
    case CMD_LOADCLUT: loadClut(); break;
    case CMD_TEXFLUSH: ++texEpoch_; break;
    case CMD_TRANSFERSTART: transfer(data); break;
    case CMD_BEZIER: drawPatch(data, false); break;
    case CMD_SPLINE: drawPatch(data, true); break;
    default:
        if (cmd >= CMD_MORPHWEIGHT0 && cmd < CMD_MORPHWEIGHT0 + 8) dctx_.morphWeights[cmd - CMD_MORPHWEIGHT0] = float24(data);
        break;
    }
}

void Recorder::transfer(uint32_t data) {
    const uint32_t src = (reg(CMD_TRANSFERSRC) & 0xFFFFF0) | ((reg(CMD_TRANSFERSRCW) & 0xFF0000) << 8);
    const uint32_t dst = (reg(CMD_TRANSFERDST) & 0xFFFFF0) | ((reg(CMD_TRANSFERDSTW) & 0xFF0000) << 8);
    const uint32_t srcStride = reg(CMD_TRANSFERSRCW) & 0x7F8, dstStride = reg(CMD_TRANSFERDSTW) & 0x7F8;
    const int sx = reg(CMD_TRANSFERSRCPOS) & 0x3FF, sy = (reg(CMD_TRANSFERSRCPOS) >> 10) & 0x3FF;
    const int dx = reg(CMD_TRANSFERDSTPOS) & 0x3FF, dy = (reg(CMD_TRANSFERDSTPOS) >> 10) & 0x3FF;
    const int w = (reg(CMD_TRANSFERSIZE) & 0x3FF) + 1, h = ((reg(CMD_TRANSFERSIZE) >> 10) & 0x3FF) + 1;
    const uint32_t bpp = (data & 1) ? 4 : 2;

    auto srcFbo = isVram(src) ? fbos_.find(vramKey(src)) : fbos_.end();
    auto dstFbo = isVram(dst) ? fbos_.find(vramKey(dst)) : fbos_.end();
    if (srcFbo != fbos_.end() && dstFbo != fbos_.end() && srcFbo->second.created && dstFbo->second.created) {
        Op op{OP_BLIT_FBO};
        op.id = srcFbo->second.id;
        op.id2 = dstFbo->second.id;
        op.x = sx + srcFbo->second.extra; op.y = sy; op.x2 = dx + dstFbo->second.extra; op.y2 = dy; op.w = w; op.h = h;
        frame_->ops.push_back(op);
        boundFbo_ = 0;
        dstFbo->second.drawnSincePresent = true;
    }
    // The memory copy always happens, like on the hardware.
    for (int y = 0; y < h; ++y) {
        uint32_t s = src + ((uint32_t)(sy + y) * srcStride + (uint32_t)sx) * bpp;
        uint32_t d = dst + ((uint32_t)(dy + y) * dstStride + (uint32_t)dx) * bpp;
        uint32_t n = (uint32_t)w * bpp;
        if (validAddress(s) && validAddress(s + n - 1) && validAddress(d) && validAddress(d + n - 1))
            std::memmove(hostPtr(d), hostPtr(s), n);
    }
    if (dstFbo != fbos_.end() && srcFbo == fbos_.end())
        markMemoryWrite(dst + ((uint32_t)dy * dstStride + (uint32_t)dx) * bpp, (uint32_t)h * dstStride * bpp);
    ++texEpoch_;
}

void colorToVec(uint32_t c, float* out, float a = 1.0f) {
    out[0] = (c & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = ((c >> 16) & 0xFF) / 255.0f;
    out[3] = a;
}

// 4x3 column-major GE matrix to GL mat4 (column major).
void to44(const float* m, float* out) {
    for (int c = 0; c < 4; ++c) {
        out[c * 4 + 0] = m[c * 3 + 0];
        out[c * 4 + 1] = m[c * 3 + 1];
        out[c * 4 + 2] = m[c * 3 + 2];
        out[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}

void Recorder::buildUbo(Ubo& u, const FboInfo& f, bool through, bool hasColor) {
    std::memset(&u, 0, sizeof u);
    to44(mtx_.world, u.world);
    to44(mtx_.view, u.view);
    std::memcpy(u.proj, mtx_.proj, sizeof u.proj);
    // TGEN rows are u, v, q.
    to44(mtx_.tgen, u.tgen);
    u.vpScale[0] = float24(reg(CMD_VIEWPORTXSCALE));
    u.vpScale[1] = float24(reg(CMD_VIEWPORTYSCALE));
    u.vpScale[2] = float24(reg(CMD_VIEWPORTZSCALE));
    u.vpScale[3] = through ? 1.0f : 0.0f;
    u.vpCenter[0] = float24(reg(CMD_VIEWPORTXCENTER));
    u.vpCenter[1] = float24(reg(CMD_VIEWPORTYCENTER));
    u.vpCenter[2] = float24(reg(CMD_VIEWPORTZCENTER));
    u.offsetFb[0] = (reg(CMD_OFFSETX) & 0xFFFF) / 16.0f;
    u.offsetFb[1] = (reg(CMD_OFFSETY) & 0xFFFF) / 16.0f;
    u.offsetFb[2] = (float)f.glW();
    u.wide[0] = (float)f.extra;
    u.offsetFb[3] = (float)f.h;
    colorToVec(reg(CMD_MATERIALAMBIENT), u.matAmbient, (reg(CMD_MATERIALALPHA) & 0xFF) / 255.0f);
    colorToVec(reg(CMD_MATERIALDIFFUSE), u.matDiffuse);
    colorToVec(reg(CMD_MATERIALSPECULAR), u.matSpecular);
    colorToVec(reg(CMD_MATERIALEMISSIVE), u.matEmissive, 0.0f);
    colorToVec(reg(CMD_AMBIENTCOLOR), u.ambient, (reg(CMD_AMBIENTALPHA) & 0xFF) / 255.0f);
    const bool lighting = !through && (reg(CMD_LIGHTINGENABLE) & 1);
    for (int l = 0; l < 4; ++l) {
        for (int k = 0; k < 3; ++k) {
            u.lightPos[l][k] = float24(reg(CMD_LX0 + 3 * l + k));
            u.lightDir[l][k] = float24(reg(CMD_LDX0 + 3 * l + k));
            u.lightAtt[l][k] = float24(reg(CMD_LKA0 + 3 * l + k));
        }
        colorToVec(reg(CMD_LAC0 + 3 * l + 0), u.lightAmb[l]);
        colorToVec(reg(CMD_LAC0 + 3 * l + 1), u.lightDiff[l]);
        colorToVec(reg(CMD_LAC0 + 3 * l + 2), u.lightSpec[l]);
        uint32_t lt = reg(CMD_LIGHTTYPE0 + l);
        u.lightParams[l][0] = (reg(CMD_LIGHTENABLE0 + l) & 1) ? 1.0f : 0.0f;
        u.lightParams[l][1] = (float)((lt >> 8) & 3);
        u.lightParams[l][2] = (float)(lt & 3);
        u.lightSpot[l][0] = float24(reg(CMD_LKO0 + l));
        u.lightSpot[l][1] = float24(reg(CMD_LKS0 + l));
    }
    u.flags0[0] = lighting ? 1.0f : 0.0f;
    u.flags0[1] = hasColor ? 1.0f : 0.0f;
    u.flags0[2] = (float)(reg(CMD_MATERIALUPDATE) & 7);
    u.flags0[3] = (reg(CMD_LIGHTMODE) & 1) ? 1.0f : 0.0f;
    u.flags1[0] = float24(reg(CMD_MATERIALSPECULARCOEF));
    u.flags1[1] = (reg(CMD_REVERSENORMAL) & 1) ? 1.0f : 0.0f;
    u.flags1[2] = (!through && (reg(CMD_FOGENABLE) & 1)) ? 1.0f : 0.0f;
    u.flags1[3] = (float)(reg(CMD_TEXMAPMODE) & 3);
    u.texgen[0] = (float)((reg(CMD_TEXMAPMODE) >> 8) & 3);
    u.texgen[1] = (float)(reg(CMD_TEXSHADELS) & 3);
    u.texgen[2] = (float)((reg(CMD_TEXSHADELS) >> 8) & 3);
    u.texScaleOff[0] = float24(reg(CMD_TEXSCALEU));
    u.texScaleOff[1] = float24(reg(CMD_TEXSCALEV));
    u.texScaleOff[2] = float24(reg(CMD_TEXOFFSETU));
    u.texScaleOff[3] = float24(reg(CMD_TEXOFFSETV));
    u.fog[0] = float24(reg(CMD_FOG1));
    u.fog[1] = float24(reg(CMD_FOG2));
    colorToVec(reg(CMD_FOGCOLOR), u.fogColor);
    colorToVec(reg(CMD_TEXENVCOLOR), u.texEnv);
    u.texEnv[3] = (float)(reg(CMD_TEXFUNC) & 7);
    u.texFlags[0] = (reg(CMD_TEXFUNC) >> 8) & 1 ? 1.0f : 0.0f;
    u.texFlags[1] = (reg(CMD_TEXFUNC) >> 16) & 1 ? 1.0f : 0.0f;
    u.texFlags[2] = (reg(CMD_CLEARMODE) & 1) ? 1.0f : 0.0f;
    u.texFlags[3] = (reg(CMD_SHADEMODE) & 1) ? 0.0f : 1.0f;
    uint32_t at = reg(CMD_ALPHATEST);
    u.alphaTest[0] = (reg(CMD_ALPHATESTENABLE) & 1) ? 1.0f : 0.0f;
    u.alphaTest[1] = (float)(at & 7);
    u.alphaTest[2] = (float)((at >> 8) & 0xFF);
    u.alphaTest[3] = (float)((at >> 16) & 0xFF);
    u.colorTest[0] = (reg(CMD_COLORTESTENABLE) & 1) ? 1.0f : 0.0f;
    u.colorTest[1] = (float)(reg(CMD_COLORTEST) & 3);
    uint32_t cr = reg(CMD_COLORREF), cm = reg(CMD_COLORTESTMASK);
    for (int k = 0; k < 3; ++k) {
        u.colorRef[k] = (float)((cr >> (8 * k)) & 0xFF);
        u.colorMask[k] = (float)((cm >> (8 * k)) & 0xFF);
    }
    u.depthRange[0] = (float)(reg(CMD_MINZ) & 0xFFFF);
    u.depthRange[1] = (float)(reg(CMD_MAXZ) & 0xFFFF);
    u.depthRange[2] = through ? 0.0f : 1.0f;
}

void Recorder::buildState(GlState& st, const FboInfo& f, bool through, int prim) {
    const bool clear = reg(CMD_CLEARMODE) & 1;
    uint32_t s1 = reg(CMD_SCISSOR1), s2 = reg(CMD_SCISSOR2);
    int x1 = s1 & 0x3FF, y1 = (s1 >> 10) & 0x3FF, x2 = s2 & 0x3FF, y2 = (s2 >> 10) & 0x3FF;
    st.scissor[0] = x1;
    st.scissor[1] = y1;
    st.scissor[2] = std::max(0, x2 - x1 + 1);
    st.scissor[3] = std::max(0, y2 - y1 + 1);
    st.depthClamp = !(reg(CMD_DEPTHCLIPENABLE) & 1);
    if (clear) {
        uint32_t cm = reg(CMD_CLEARMODE);
        bool colorOn = cm & 0x100, alphaOn = cm & 0x200, depthOn = cm & 0x400;
        st.colorMask[0] = st.colorMask[1] = st.colorMask[2] = colorOn;
        st.colorMask[3] = alphaOn;
        st.depthTest = depthOn;
        st.depthFunc = GL_ALWAYS;
        st.depthWrite = depthOn;
        st.stencil = alphaOn;
        st.stencilFunc = GL_ALWAYS;
        st.sfail = st.zfail = st.zpass = GL_REPLACE;
        st.stencilWrite = 0xFF;
        return;
    }
    // Blending
    if (reg(CMD_ALPHABLENDENABLE) & 1) {
        uint32_t bm = reg(CMD_BLENDMODE);
        int sf = bm & 0xF, df = (bm >> 4) & 0xF, eq = (bm >> 8) & 7;
        static const uint32_t common[10] = {GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                            GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                            GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
        static const uint32_t eqs[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD};
        float fa[4], fb[4];
        colorToVec(reg(CMD_BLENDFIXEDA), fa);
        colorToVec(reg(CMD_BLENDFIXEDB), fb);
        auto fixedFactor = [&](const float* c, float* constOut) -> uint32_t {
            if (c[0] == 0 && c[1] == 0 && c[2] == 0) return GL_ZERO;
            if (c[0] == 1 && c[1] == 1 && c[2] == 1) return GL_ONE;
            std::memcpy(constOut, c, 3 * sizeof(float));
            return GL_CONSTANT_COLOR;
        };
        st.blend = true;
        st.eqRGB = eqs[eq < 6 ? eq : 0];
        uint32_t src = sf < 10 ? common[sf] : 0, dst = df < 10 ? (df == 0 ? GL_SRC_COLOR : df == 1 ? GL_ONE_MINUS_SRC_COLOR : common[df]) : 0;
        if (sf >= 10 && df >= 10) {
            bool complementary = std::abs(fa[0] + fb[0] - 1) < 0.01f && std::abs(fa[1] + fb[1] - 1) < 0.01f && std::abs(fa[2] + fb[2] - 1) < 0.01f;
            if (complementary) {
                src = GL_CONSTANT_COLOR;
                dst = GL_ONE_MINUS_CONSTANT_COLOR;
                std::memcpy(st.blendColor, fa, 3 * sizeof(float));
            } else {
                // Both fixed and unrelated: the shader premultiplies by A, the GL constant is B.
                src = GL_ONE;
                dst = fixedFactor(fb, st.blendColor);
                premulBlend_ = true;
                std::memcpy(premulColor_, fa, 3 * sizeof(float));
            }
        } else if (sf >= 10) {
            src = fixedFactor(fa, st.blendColor);
        } else if (df >= 10) {
            dst = fixedFactor(fb, st.blendColor);
        }
        st.srcRGB = src;
        st.dstRGB = dst;
    }
    // Depth
    if (!through || true) {
        st.depthTest = reg(CMD_ZTESTENABLE) & 1;
        st.depthFunc = depthFuncGL(reg(CMD_ZTEST) & 7);
        st.depthWrite = st.depthTest && !(reg(CMD_ZWRITEDISABLE) & 1);
    }
    // Stencil
    if (reg(CMD_STENCILTESTENABLE) & 1) {
        uint32_t t = reg(CMD_STENCILTEST), o = reg(CMD_STENCILOP);
        st.stencil = true;
        st.stencilFunc = depthFuncGL(t & 7);
        st.stencilRef = (t >> 8) & 0xFF;
        st.stencilMask = (t >> 16) & 0xFF;
        st.sfail = stencilOpGL(o & 7);
        st.zfail = stencilOpGL((o >> 8) & 7);
        st.zpass = stencilOpGL((o >> 16) & 7);
        st.stencilWrite = ~reg(CMD_MASKALPHA) & 0xFF;
    }
    // Masks
    uint32_t mrgb = reg(CMD_MASKRGB);
    st.colorMask[0] = (mrgb & 0xFF) != 0xFF;
    st.colorMask[1] = ((mrgb >> 8) & 0xFF) != 0xFF;
    st.colorMask[2] = ((mrgb >> 16) & 0xFF) != 0xFF;
    // The PSP's framebuffer alpha is its stencil buffer: drawing only changes it through stencil
    // operations (PPSSPP GPUStateUtils: "not write to alpha if stencil isn't enabled").
    st.colorMask[3] = false;
    if (st.stencil && (reg(CMD_MASKALPHA) & 0xFF) != 0xFF) {
        const int zpass = (reg(CMD_STENCILOP) >> 16) & 7;
        if (zpass == 2 || zpass == 1) { // REPLACE / ZERO: the written stencil value becomes the alpha
            st.colorMask[3] = true;
            alphaOut_ = zpass == 2 ? st.stencilRef / 255.0f : 0.0f;
            alphaOverride_ = true;
        }
    }
    // Culling (never for through mode or rectangles)
    if (!through && prim != PRIM_RECTANGLES && (reg(CMD_CULLFACEENABLE) & 1)) {
        st.cull = true;
        static const bool flip = std::getenv("CA_CULL_FLIP") != nullptr;
        st.cullFace = ((reg(CMD_CULL) & 1) != 0) != flip ? GL_BACK : GL_FRONT;
    }
    if (reg(CMD_LOGICOPENABLE) & 1) {
        st.logic = true;
        st.logicOp = GL_CLEAR + (reg(CMD_LOGICOP) & 15);
    }
}

bool Recorder::setupTexture(GlState& st, Ubo& u, const FboInfo& target) {
    ScopedUs perf(g_perf.texUs);
    const uint32_t texbufw0 = reg(CMD_TEXBUFWIDTH0);
    const uint32_t addr = (reg(CMD_TEXADDR0) & 0xFFFFF0) | ((texbufw0 << 8) & 0x0F000000);
    const int format = reg(CMD_TEXFORMAT) & 0xF;
    const int w = 1 << (reg(CMD_TEXSIZE0) & 0xF), h = 1 << ((reg(CMD_TEXSIZE0) >> 8) & 0xF);
    const bool swz = reg(CMD_TEXMODE) & 1;
    const int maxLevel = (reg(CMD_TEXMODE) >> 16) & 7;
    u.texSize[0] = (float)w;
    u.texSize[1] = (float)h;
    u.texSize[2] = u.texSize[3] = 1.0f;

    uint32_t filt = reg(CMD_TEXFILTER);
    static const uint32_t minf[8] = {GL_NEAREST, GL_LINEAR, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                     GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    st.minFilter = minf[filt & 7];
    st.magFilter = (filt >> 8) & 1 ? GL_LINEAR : GL_NEAREST;
    if (g_settings.forceLinear) {
        st.magFilter = GL_LINEAR;
        st.minFilter = (st.minFilter == GL_NEAREST || st.minFilter == GL_LINEAR) ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
    }
    st.wrapS = (reg(CMD_TEXWRAP) & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    st.wrapT = (reg(CMD_TEXWRAP) & 0x100) ? GL_CLAMP_TO_EDGE : GL_REPEAT;

    // Render-to-texture: the texture is a framebuffer we rendered.
    if (isVram(addr) && format <= TFMT_8888) {
        auto it = fbos_.find(vramKey(addr));
        if (it != fbos_.end() && it->second.created) {
            FboInfo& src = it->second;
            flushMemDirty(src);
            st.texKind = 2;
            st.texId = src.id;
            st.maxLevel = 0;
            if (st.minFilter != GL_NEAREST && st.minFilter != GL_LINEAR) st.minFilter = (st.minFilter == GL_NEAREST_MIPMAP_NEAREST || st.minFilter == GL_NEAREST_MIPMAP_LINEAR) ? GL_NEAREST : GL_LINEAR;
            u.texSize[2] = (float)w / src.glW();
            u.texSize[3] = (float)h / src.h;
            u.wide[3] = (float)src.extra / src.glW();
            (void)target;
            return true;
        }
    }

    // Cached texture: keyed by everything that changes the decoded result.
    const uint32_t bufw = textureBufw(texbufw0, format);
    const bool clutFmt = format >= TFMT_CLUT4 && format <= TFMT_CLUT32;
    uint64_t key = hashBytes(reinterpret_cast<const uint8_t*>(&addr), 4);
    uint64_t parts[6] = {(uint64_t)format | ((uint64_t)w << 8) | ((uint64_t)h << 24) | ((uint64_t)swz << 40) | ((uint64_t)maxLevel << 41),
                         bufw, clutFmt ? clutHash_ : 0, clutFmt ? reg(CMD_CLUTFORMAT) : 0, addr, 0};
    key ^= hashBytes(reinterpret_cast<const uint8_t*>(parts), sizeof parts);
    TexEntry& e = textures_[key];
    bool upload = false;
    if (e.id == 0) {
        e.id = nextTexId_++;
        upload = true;
    }
    if (e.verifiedEpoch != texEpoch_ || upload) {
        uint32_t bytes = textureByteSize(format, bufw, h, swz);
        uint64_t mh = 0;
        if (validAddress(addr) && validAddress(addr + bytes - 1)) mh = hashBytes(hostPtr(addr), bytes);
        if (mh != e.memHash) upload = true;
        e.memHash = mh;
        e.verifiedEpoch = texEpoch_;
    }
    int levels = 1;
    if (maxLevel > 0) {
        int lw = w, lh = h;
        for (int l = 1; l <= maxLevel; ++l) {
            int nw = 1 << (reg(CMD_TEXSIZE0 + l) & 0xF), nh = 1 << ((reg(CMD_TEXSIZE0 + l) >> 8) & 0xF);
            if (nw != std::max(1, lw / 2) || nh != std::max(1, lh / 2)) break;
            lw = nw;
            lh = nh;
            ++levels;
        }
    }
    if (upload) {
        Op op{OP_UPLOAD_TEX};
        op.id = e.id;
        op.w = w;
        op.h = h;
        op.blob = frame_->blob.size();
        ClutState cs = clut_;
        cs.format = reg(CMD_CLUTFORMAT);
        std::vector<uint32_t> px;
        for (int l = 0; l < levels; ++l) {
            uint32_t bw = reg(CMD_TEXBUFWIDTH0 + l);
            uint32_t la = (reg(CMD_TEXADDR0 + l) & 0xFFFFF0) | ((bw << 8) & 0x0F000000);
            int lw = 1 << (reg(CMD_TEXSIZE0 + l) & 0xF), lh = 1 << ((reg(CMD_TEXSIZE0 + l) >> 8) & 0xF);
            decodeTexture(la, format, lw, lh, textureBufw(bw, format), swz, cs, px);
            if (l == 0) patchButtonGlyphs(px.data(), lw, lh);
            size_t off = frame_->blob.size();
            frame_->blob.resize(off + px.size() * 4);
            std::memcpy(frame_->blob.data() + off, px.data(), px.size() * 4);
            op.levels.push_back({lw, lh});
        }
        op.blobSize = frame_->blob.size() - op.blob;
        if (const char* dir = std::getenv("CA_TEX_DUMP")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/tex%04d_%08x_f%d_%dx%d_clut%06x.png", dir, e.id, addr, format, w, h,
                          clutFmt ? reg(CMD_CLUTFORMAT) : 0);
            stbi_write_png(path, w, h, 4, frame_->blob.data() + op.blob, w * 4);
        }
        frame_->ops.push_back(op);
        e.w = w;
        e.h = h;
    }
    st.texKind = 1;
    st.texId = e.id;
    st.maxLevel = levels - 1;
    if (levels == 1 && st.minFilter != GL_NEAREST && st.minFilter != GL_LINEAR)
        st.minFilter = (st.minFilter == GL_NEAREST_MIPMAP_NEAREST || st.minFilter == GL_NEAREST_MIPMAP_LINEAR) ? GL_NEAREST : GL_LINEAR;
    return true;
}

// CPU transform of one vertex to framebuffer pixel space (rectangles in transform mode).
void transformToScreen(const Matrices& m, const Ubo& u, const Vertex& v, float out[3]) {
    float wp[3], vp[3];
    for (int r = 0; r < 3; ++r) wp[r] = m.world[r] * v.pos[0] + m.world[3 + r] * v.pos[1] + m.world[6 + r] * v.pos[2] + m.world[9 + r];
    for (int r = 0; r < 3; ++r) vp[r] = m.view[r] * wp[0] + m.view[3 + r] * wp[1] + m.view[6 + r] * wp[2] + m.view[9 + r];
    float c[4];
    for (int r = 0; r < 4; ++r) c[r] = m.proj[r] * vp[0] + m.proj[4 + r] * vp[1] + m.proj[8 + r] * vp[2] + m.proj[12 + r];
    float iw = c[3] != 0 ? 1.0f / c[3] : 0.0f;
    out[0] = c[0] * iw * u.vpScale[0] + u.vpCenter[0] - u.offsetFb[0];
    out[1] = c[1] * iw * u.vpScale[1] + u.vpCenter[1] - u.offsetFb[1];
    out[2] = c[2] * iw * u.vpScale[2] + u.vpCenter[2];
}

// ---- Bezier / spline patches (tessellated on the CPU like PPSSPP's SoftwareTessellation) ----

namespace {
struct Basis { float b[4], d[4]; };

void bezierWeights(float t, Basis& w) {
    w.b[0] = (1 - t) * (1 - t) * (1 - t);
    w.b[1] = 3 * t * (1 - t) * (1 - t);
    w.b[2] = 3 * t * t * (1 - t);
    w.b[3] = t * t * t;
    w.d[0] = -3 * (1 - t) * (1 - t);
    w.d[1] = 9 * t * t - 12 * t + 3;
    w.d[2] = 3 * (2 - 3 * t) * t;
    w.d[3] = 3 * t * t;
}

// from PPSSPP GPU/Common/SplineCommon.cpp (Spline3DWeight): cubic B-spline with open/closed ends.
struct KnotDiv { float _3_0 = 1.0f / 3, _4_1 = 1.0f / 3, _5_2 = 1.0f / 3, _3_1 = 0.5f, _4_2 = 0.5f, _3_2 = 1.0f; };

void splineKnots(int n, int type, std::vector<float>& knots, std::vector<KnotDiv>& divs) {
    knots.assign((size_t)n + 2, 0.0f);
    divs.assign((size_t)n, KnotDiv{});
    for (int i = 0; i < n + 2; ++i) knots[i] = (float)i - 2;
    if (type & 1) {
        knots[0] = 0;
        knots[1] = 0;
        divs[0]._3_0 = 1.0f;
        divs[0]._4_1 = 0.5f;
        divs[0]._3_1 = 1.0f;
        if (n > 1) divs[1]._3_0 = 0.5f;
    }
    if (type & 2) {
        divs[n - 1]._4_1 = (n == 1 && (type & 1)) ? 1.0f : 0.5f;
        divs[n - 1]._5_2 = 1.0f;
        divs[n - 1]._4_2 = 1.0f;
        if (n > 1) divs[n - 2]._5_2 = 0.5f;
    }
}

void splineWeights(float t, const float* knots, const KnotDiv& div, Basis& w) {
    float t0 = t - knots[0], t1 = t - knots[1], t2 = t - knots[2];
    float f30 = t0 * div._3_0, f41 = t1 * div._4_1, f52 = t2 * div._5_2;
    float f31 = t1 * div._3_1, f42 = t2 * div._4_2, f32 = t2 * div._3_2;
    float a = (1 - f30) * (1 - f31), b = f31 * f41, c = (1 - f41) * (1 - f42), d = f42 * f52;
    w.b[0] = a * (1 - f32);
    w.b[1] = 1 - a - b + ((a + b + c - 1) * f32);
    w.b[2] = b + ((1 - b - c - d) * f32);
    w.b[3] = d * f32;
    float i1 = (1 - f31) * (1 - f32), i2 = f31 * (1 - f32) + (1 - f42) * f32, i3 = f42 * f32;
    float f130 = i1 * div._3_0, f241 = i2 * div._4_1, f352 = i3 * div._5_2;
    w.d[0] = 3 * (0 - f130);
    w.d[1] = 3 * (f130 - f241);
    w.d[2] = 3 * (f241 - f352);
    w.d[3] = 3 * (f352 - 0);
}

// Per-axis weights for every tessellated sample: (patch, first control point, basis).
struct AxisSample { int patch, point; Basis w; float t; };

std::vector<AxisSample> axisSamples(bool spline, int points, int tess, int type) {
    std::vector<AxisSample> out;
    if (!spline) {
        const int patches = (points - 1) / 3;
        for (int p = 0; p < patches; ++p)
            for (int i = 0; i <= tess; ++i) {
                AxisSample s;
                s.patch = p;
                s.point = 3 * p;
                s.t = p + (float)i / tess;
                bezierWeights((float)i / tess, s.w);
                out.push_back(s);
            }
        return out;
    }
    const int patches = points - 3;
    std::vector<float> knots;
    std::vector<KnotDiv> divs;
    splineKnots(patches, type, knots, divs);
    for (int p = 0; p < patches; ++p)
        for (int j = (p == 0 ? 0 : 1); j <= tess; ++j) {
            AxisSample s;
            s.patch = p;
            s.point = p;
            const int index = p * tess + j;
            s.t = (float)index / tess;
            splineWeights((float)index / tess, knots.data() + p, divs[p], s.w);
            out.push_back(s);
        }
    return out;
}
} // namespace

std::shared_ptr<const TessResult> tessellate(const std::vector<Vertex>& cp, int nu, int nv, uint32_t data,
                                                        bool spline, const VertexFormat& fmt, uint32_t div, int primType,
                                                        bool facing) {
    auto res = std::make_shared<TessResult>();
    int tessU = std::max(1, (int)(div & 0x7F)), tessV = std::max(1, (int)((div >> 8) & 0x7F));
    const int typeU = (data >> 16) & 3, typeV = (data >> 18) & 3;
    const auto su = axisSamples(spline, nu, tessU, typeU);
    const auto sv = axisSamples(spline, nv, tessV, typeV);
    const int gu = (int)su.size(), gv = (int)sv.size();

    // Evaluate the grid (row-major, v outer).
    std::vector<Vertex> grid((size_t)gu * gv);
    for (int j = 0; j < gv; ++j) {
        for (int i = 0; i < gu; ++i) {
            const AxisSample& a = su[i];
            const AxisSample& b = sv[j];
            Vertex o;
            std::memset(&o, 0, sizeof o);
            float du[3] = {0, 0, 0}, dv[3] = {0, 0, 0};
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const Vertex& c = cp[(size_t)(b.point + y) * nu + (a.point + x)];
                    const float w = a.w.b[x] * b.w.b[y];
                    for (int k = 0; k < 3; ++k) {
                        o.pos[k] += c.pos[k] * w;
                        du[k] += c.pos[k] * a.w.d[x] * b.w.b[y];
                        dv[k] += c.pos[k] * a.w.b[x] * b.w.d[y];
                    }
                    for (int k = 0; k < 4; ++k) o.color[k] += c.color[k] * w;
                    o.uv[0] += c.uv[0] * w;
                    o.uv[1] += c.uv[1] * w;
                }
            }
            if (!fmt.col) std::memcpy(o.color, cp[0].color, sizeof o.color);
            if (!fmt.tc) {
                // Generated texture coordinates: patch index plus the position within it.
                o.uv[0] = spline ? a.t : a.t;
                o.uv[1] = spline ? b.t : b.t;
            }
            float n[3] = {du[1] * dv[2] - du[2] * dv[1], du[2] * dv[0] - du[0] * dv[2], du[0] * dv[1] - du[1] * dv[0]};
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0) for (float& c : n) c /= len;
            else { n[0] = 0; n[1] = 0; n[2] = 1; }
            if (facing) for (float& c : n) c = -c;
            std::memcpy(o.nrm, n, sizeof n);
            grid[(size_t)j * gu + i] = o;
        }
    }

    std::vector<Vertex>& out = res->verts;
    if (primType == 2) {
        out = std::move(grid);
        res->prim = PRIM_POINTS;
        return res;
    }
    if (primType == 1) {
        for (int j = 0; j + 1 < gv; ++j) {
            for (int i = 0; i + 1 < gu; ++i) {
                const Vertex& v0 = grid[(size_t)j * gu + i];
                const Vertex& v1 = grid[(size_t)j * gu + i + 1];
                const Vertex& v2 = grid[(size_t)(j + 1) * gu + i];
                const Vertex& v3 = grid[(size_t)(j + 1) * gu + i + 1];
                out.push_back(v0); out.push_back(v2);
                out.push_back(v2); out.push_back(v1);
                if (i + 2 == gu) { out.push_back(v1); out.push_back(v3); }
            }
        }
        res->prim = PRIM_LINES;
        return res;
    }
    // One triangle strip: per row (top_0, bottom_0, top_1, bottom_1, ...), which gives the
    // triangles (t_i, b_i, t_i+1) and (t_i+1, b_i, b_i+1) with the winding of the hardware's
    // order; rows are joined by two degenerate vertices (an even count keeps the winding).
    if (gu < 2 || gv < 2) return res;
    out.reserve((size_t)(gv - 1) * (2 * gu + 2));
    for (int j = 0; j + 1 < gv; ++j) {
        if (j > 0) out.push_back(grid[(size_t)j * gu]);
        for (int i = 0; i < gu; ++i) {
            out.push_back(grid[(size_t)j * gu + i]);
            out.push_back(grid[(size_t)(j + 1) * gu + i]);
        }
        if (j + 2 < gv) out.push_back(out.back());
    }
    res->prim = PRIM_TRIANGLE_STRIP;
    return res;
}


void Recorder::drawPatch(uint32_t data, bool spline) {
    ScopedUs perf(g_perf.patchUs);
    ++g_perf.patches;
    VertexFormat fmt;
    fmt.set(reg(CMD_VERTEXTYPE));
    const int nu = data & 0xFF, nv = (data >> 8) & 0xFF;
    const int count = nu * nv;
    const int isz = fmt.indexSize();
    std::vector<uint32_t> indices((size_t)count);
    for (int i = 0; i < count; ++i) {
        uint32_t a = iaddr_ + (uint32_t)(i * isz);
        indices[i] = isz == 0 ? (uint32_t)i : (isz == 1 ? read8(a) : (isz == 2 ? read16(a) : read32(a)));
    }
    const uint32_t vbase = vaddr_;
    if (isz) iaddr_ += (uint32_t)(count * isz);
    else vaddr_ += (uint32_t)(count * fmt.size);
    // The hardware draws nothing with fewer than 4 points along either direction.
    if (nu < 4 || nv < 4) return;

    dctx_.mtx = &mtx_;
    std::vector<Vertex> cp((size_t)count);
    for (int i = 0; i < count; ++i) decodeVertex(fmt, dctx_, vbase, indices[i], cp[i]);

    const uint32_t div = reg(CMD_PATCHDIVISION);
    const uint32_t prim = reg(CMD_PATCHPRIMITIVE) & 3; // 0 triangles, 1 lines, 2 points
    const bool facing = reg(CMD_PATCHFACING) & 1;
    const uint64_t params[2] = {(uint64_t)data | (uint64_t)spline << 31 | (uint64_t)prim << 32 | (uint64_t)facing << 34 |
                                    (uint64_t)(fmt.col != 0) << 35 | (uint64_t)(fmt.tc != 0) << 36,
                                div};
    const uint64_t key = hashBytes(reinterpret_cast<const uint8_t*>(cp.data()), cp.size() * sizeof(Vertex)) ^
                         (hashBytes(reinterpret_cast<const uint8_t*>(params), sizeof params) * 0x9E3779B97F4A7C15ull);
    std::shared_ptr<const TessResult> tess;
    if (auto it = patchCache_.find(key); it != patchCache_.end()) {
        tess = it->second;
    } else {
        tess = tessellate(cp, nu, nv, data, spline, fmt, div, (int)prim, facing);
        if (patchCache_.size() >= 16384) patchCache_.clear();
        patchCache_[key] = tess;
    }
    if (tess->verts.empty()) return;
    if (!fmt.through)
        minimap::addDraw(key, std::shared_ptr<const std::vector<Vertex>>(tess, &tess->verts), tess->prim, mtx_.world,
                         mtx_.view, false);
    std::vector<Vertex> out = tess->verts;
    emit(out, tess->prim, fmt.through, fmt.col != 0);
}

void Recorder::draw(uint32_t data) {
    ScopedUs perf(g_perf.drawUs);
    ++g_perf.draws;
    const int count = data & 0xFFFF;
    const int prim = (data >> 16) & 7;
    VertexFormat fmt;
    fmt.set(reg(CMD_VERTEXTYPE));
    std::vector<uint32_t> indices((size_t)count);
    const int isz = fmt.indexSize();
    if (isz) {
        for (int i = 0; i < count; ++i) {
            uint32_t a = iaddr_ + (uint32_t)(i * isz);
            indices[i] = isz == 1 ? read8(a) : (isz == 2 ? read16(a) : read32(a));
        }
    } else {
        for (int i = 0; i < count; ++i) indices[i] = (uint32_t)i;
    }
    const uint32_t vbase = vaddr_;
    if (isz) iaddr_ += (uint32_t)(count * isz);
    else vaddr_ += (uint32_t)(count * fmt.size);
    if (count == 0 || prim > PRIM_RECTANGLES) return;

    dctx_.mtx = &mtx_;
    std::vector<Vertex> vin((size_t)count);
    for (int i = 0; i < count; ++i) decodeVertex(fmt, dctx_, vbase, indices[i], vin[i]);
    if (!fmt.through && prim >= PRIM_TRIANGLES && prim <= PRIM_TRIANGLE_FAN) {
        uint64_t key = hashBytes(reinterpret_cast<const uint8_t*>(vin.data()), vin.size() * sizeof(Vertex));
        minimap::addDraw(key, std::make_shared<const std::vector<Vertex>>(vin), prim, mtx_.world, mtx_.view, fmt.weight != 0);
    }
    emit(vin, prim, fmt.through, fmt.col != 0);
}

void Recorder::emit(std::vector<Vertex>& vin, int prim, bool through, bool hasColor) {
    const int count = (int)vin.size();
    static const bool mapDebug = std::getenv("CA_MAP_DEBUG") != nullptr;
    if (mapDebug && !through) {
        dbgViews_[std::vector<float>(mtx_.view, mtx_.view + 12)]++;
        const float* v = mtx_.view;
        float cpos[3];
        for (int k = 0; k < 3; ++k) cpos[k] = -(v[3 * k] * v[9] + v[3 * k + 1] * v[10] + v[3 * k + 2] * v[11]);
        if (std::fabs(cpos[0]) + std::fabs(cpos[1]) + std::fabs(cpos[2]) < 20) { // the weapon's camera
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (const Vertex& vx : vin) {
                float w[3], e[3];
                for (int r = 0; r < 3; ++r) w[r] = mtx_.world[r] * vx.pos[0] + mtx_.world[3 + r] * vx.pos[1] + mtx_.world[6 + r] * vx.pos[2] + mtx_.world[9 + r];
                for (int r = 0; r < 3; ++r) e[r] = v[r] * w[0] + v[3 + r] * w[1] + v[6 + r] * w[2] + v[9 + r];
                for (int r = 0; r < 3; ++r) { lo[r] = std::min(lo[r], e[r]); hi[r] = std::max(hi[r], e[r]); }
            }
            std::fprintf(stderr, "[VM] weights %d verts %zu tex %d x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n",
                         (int)((reg(CMD_VERTEXTYPE) >> 9) & 3), vin.size(), (int)(reg(CMD_TEXTUREMAPENABLE) & 1), lo[0], hi[0],
                         lo[1], hi[1], lo[2], hi[2]);
        }
    }
    FboInfo& target = prepareTarget();
    const bool clear = reg(CMD_CLEARMODE) & 1;

    Op op{OP_DRAW};
    Ubo ubo;
    bool transformedRect = false;
    if (!through) ads_.observe(mtx_.view, mtx_.world, vin);
    else if (ads_.hide2D(vin)) return;
    buildUbo(ubo, target, through, hasColor);
    if (!through) ads_.adjust(mtx_.view, ubo.vpScale[0], ubo.view, ubo.proj);

    premulBlend_ = false;
    alphaOverride_ = false;
    buildState(op.st, target, through, prim);
    if (alphaOverride_) {
        // The stencil value goes to the alpha channel through the blend's alpha half, so the
        // fragment alpha still drives the color blend factors.
        op.st.alphaConst = true;
        op.st.blendColor[3] = alphaOut_;
    }
    if (premulBlend_) {
        std::memcpy(ubo.blendPremul, premulColor_, 3 * sizeof(float));
        ubo.blendPremul[3] = 1.0f;
    }
    const bool textured = !clear && (reg(CMD_TEXTUREMAPENABLE) & 1);
    if (textured) setupTexture(op.st, ubo, target);
    ubo.texgen[3] = textured ? 1.0f : 0.0f;
    if (clear) {
        // Clear mode writes the vertex color and depth as they are.
        if (op.st.stencil) op.st.stencilRef = (int)(vin.back().color[3] * 255.0f + 0.5f);
    }

    // Rectangles: two corners per sprite, expanded to two triangles. In transform mode the
    // corners are projected on the CPU first and the draw continues in through mode.
    std::vector<Vertex> out;
    if (prim == PRIM_RECTANGLES) {
        if (!through) {
            transformedRect = true;
            for (auto& v : vin) {
                float s[3];
                transformToScreen(mtx_, ubo, v, s);
                v.pos[0] = s[0]; v.pos[1] = s[1]; v.pos[2] = s[2];
                if (textured && (reg(CMD_TEXMAPMODE) & 3) == 0) {
                    v.uv[0] = (v.uv[0] * ubo.texScaleOff[0] + ubo.texScaleOff[2]) * ubo.texSize[0];
                    v.uv[1] = (v.uv[1] * ubo.texScaleOff[1] + ubo.texScaleOff[3]) * ubo.texSize[1];
                }
            }
            through = true;
            ubo.vpScale[3] = 1.0f;
            ubo.flags0[0] = 0.0f;
            ubo.flags1[2] = 0.0f;
            ubo.depthRange[2] = 0.0f;
        }
        for (int i = 0; i + 1 < count; i += 2) {
            const Vertex& a = vin[i];
            const Vertex& b = vin[i + 1];
            Vertex c[4];
            for (int k = 0; k < 4; ++k) {
                c[k] = b;
                c[k].pos[0] = (k & 1) ? b.pos[0] : a.pos[0];
                c[k].pos[1] = (k & 2) ? b.pos[1] : a.pos[1];
                c[k].uv[0] = (k & 1) ? b.uv[0] : a.uv[0];
                c[k].uv[1] = (k & 2) ? b.uv[1] : a.uv[1];
            }
            const int tri[6] = {0, 1, 2, 1, 3, 2};
            for (int t : tri) out.push_back(c[t]);
        }
        op.glMode = GL_TRIANGLES;
    } else {
        out = std::move(vin);
        static const uint32_t modes[6] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
        op.glMode = modes[prim];
    }
    if (out.empty()) return;
    if (through) op.st.cull = false;

    // Widescreen, sampling a widened buffer: UVs spanning its whole 480-pixel width (screen copies
    // for glow/blur passes) take the whole widened width, so the copy holds the full picture and
    // the stretched full-screen composite puts it back in place.
    if (op.st.texKind == 2 && ubo.wide[3] > 0) {
        float minU = 1e9f, maxU = -1e9f;
        const bool texThrough = through && !transformedRect;
        for (const Vertex& v : out) {
            float u = texThrough ? v.uv[0] : v.uv[0] * ubo.texSize[0];
            if (!texThrough && (reg(CMD_TEXMAPMODE) & 3) == 0 && !transformedRect)
                u = (v.uv[0] * ubo.texScaleOff[0] + ubo.texScaleOff[2]) * ubo.texSize[0];
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
        }
        if (minU <= 0.5f && maxU >= 479.5f) {
            ubo.texSize[2] = ubo.texSize[0] / 480.0f;
            ubo.wide[3] = 0.0f;
        }
    }

    // Widescreen: 3D is shifted into the middle of the wider buffer and sees more to the sides;
    // 2D keeps its size in the middle, except full-screen quads (backgrounds, fades, clears),
    // which are stretched over the whole width.
    if (target.extra) {
        float minX = 1e9f, maxX = -1e9f;
        for (const Vertex& v : out) { minX = std::min(minX, v.pos[0]); maxX = std::max(maxX, v.pos[0]); }
        const bool fullScissor = op.st.scissor[0] == 0 && op.st.scissor[2] >= 480;
        const bool stretch = (through || clear) && minX <= 0.5f && maxX >= 479.5f && !transformedRect;
        const float k = (float)target.glW() / 480.0f;
        if (stretch) {
            ubo.wide[1] = 1.0f;
            op.st.scissor[0] = (int)std::floor(op.st.scissor[0] * k);
            op.st.scissor[2] = fullScissor ? target.glW() : (int)std::ceil(op.st.scissor[2] * k);
        } else if (!through && fullScissor) {
            op.st.scissor[2] = target.glW();
        } else {
            op.st.scissor[0] += target.extra;
        }
    }

    op.first = frame_->verts.size() / kFloatsPerVertex;
    op.count = out.size();
    for (const Vertex& v : out) {
        const float* p = &v.pos[0];
        frame_->verts.insert(frame_->verts.end(), p, p + kFloatsPerVertex);
    }
    op.ubo = frame_->ubos.size();
    frame_->ubos.push_back(ubo);
    frame_->ops.push_back(op);
}

// ---- executor (render thread) -----------------------------------------------------------

struct GlFbo {
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;      // logical
    int rw = 0, rh = 0;    // real (scaled)
};

struct Exec {
    GLuint program = 0, vao = 0, vbo = 0, ubo = 0;
    GLint texLoc = -1;
    std::unordered_map<int, GlFbo> fbos;
    std::unordered_map<int, GLuint> textures;
    GLuint feedbackTex = 0;
    int feedbackW = 0, feedbackH = 0;
    int bound = 0;
    PresentInfo last;
    int lastFbo = 0;
} g_exec;

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[GPU] shader compile failed:\n%s\n", log);
    }
    return s;
}

void createFbo(GlFbo& f, int w, int h) {
    const int scale = std::max(1, g_settings.scale);
    GlFbo n;
    n.w = w;
    n.h = h;
    n.rw = w * scale;
    n.rh = h * scale;
    glGenTextures(1, &n.color);
    glBindTexture(GL_TEXTURE_2D, n.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n.rw, n.rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &n.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, n.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, n.rw, n.rh);
    glGenFramebuffers(1, &n.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, n.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, n.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, n.depth);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glStencilMask(0xFF);
    glClearColor(0, 0, 0, 0);
    glClearDepth(0.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (f.fbo) {
        // Keep the old contents when growing.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, n.fbo);
        glBlitFramebuffer(0, 0, f.rw, f.rh, 0, 0, f.rw, f.rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers(1, &f.fbo);
        glDeleteTextures(1, &f.color);
        glDeleteRenderbuffers(1, &f.depth);
    }
    f = n;
    g_exec.bound = -1;
}

void bindTarget(int id) {
    auto it = g_exec.fbos.find(id);
    if (it == g_exec.fbos.end()) return;
    glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
    glViewport(0, 0, it->second.rw, it->second.rh);
    g_exec.bound = id;
}

void applyState(const GlState& s, const GlFbo& target) {
    const int scale = std::max(1, g_settings.scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor(s.scissor[0] * scale, s.scissor[1] * scale, s.scissor[2] * scale, s.scissor[3] * scale);
    (void)target;
    if (s.blend || s.alphaConst) {
        glEnable(GL_BLEND);
        glBlendEquationSeparate(s.blend ? s.eqRGB : GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(s.blend ? s.srcRGB : GL_ONE, s.blend ? s.dstRGB : GL_ZERO,
                            s.alphaConst ? GL_CONSTANT_ALPHA : GL_ONE, GL_ZERO);
        glBlendColor(s.blendColor[0], s.blendColor[1], s.blendColor[2], s.blendColor[3]);
    } else {
        glDisable(GL_BLEND);
    }
    if (s.depthTest || s.depthWrite) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(s.depthTest ? s.depthFunc : GL_ALWAYS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(s.depthWrite ? 1 : 0);
    if (s.depthClamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (s.stencil) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(s.stencilFunc, s.stencilRef, (GLuint)s.stencilMask);
        glStencilOp(s.sfail, s.zfail, s.zpass);
        glStencilMask((GLuint)s.stencilWrite);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (s.cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(s.cullFace);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    if (s.logic) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(s.logicOp);
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
}

GLuint textureFor(const GlState& s) {
    if (s.texKind == 1) {
        auto it = g_exec.textures.find(s.texId);
        return it == g_exec.textures.end() ? 0 : it->second;
    }
    if (s.texKind == 2) {
        auto it = g_exec.fbos.find(s.texId);
        if (it == g_exec.fbos.end()) return 0;
        if (s.texId != g_exec.bound) return it->second.color;
        // Sampling the target itself: copy it first.
        GlFbo& f = it->second;
        if (!g_exec.feedbackTex || g_exec.feedbackW != f.rw || g_exec.feedbackH != f.rh) {
            if (!g_exec.feedbackTex) glGenTextures(1, &g_exec.feedbackTex);
            glBindTexture(GL_TEXTURE_2D, g_exec.feedbackTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.rw, f.rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g_exec.feedbackW = f.rw;
            g_exec.feedbackH = f.rh;
        }
        GLuint tmp = 0;
        glGenFramebuffers(1, &tmp);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_exec.feedbackTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f.fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, f.rw, f.rh, 0, 0, f.rw, f.rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers(1, &tmp);
        glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
        return g_exec.feedbackTex;
    }
    return 0;
}

std::atomic<int> g_dumpFrames{0};
std::atomic<int> g_maxDraws{-1};
std::atomic<int> g_skipFbTex{0};

void debugFrame(const Frame& fr) {
    static int n = 0;
    static const bool on = std::getenv("CA_GPU_DEBUG") != nullptr;
    bool full = false;
    if (g_dumpFrames > 0 && fr.ops.size() > 2) { --g_dumpFrames; full = true; }
    else if (!on || (n++ % 60) != 0) return;
    std::fprintf(stderr, "[GPU] frame %llu: %zu ops, %zu verts\n", (unsigned long long)fr.number, fr.ops.size(), fr.verts.size() / kFloatsPerVertex);
    int shown = 0;
    for (const Op& op : fr.ops) {
        if (op.type == OP_DRAW && (shown < 6 || full)) {
            ++shown;
            const float* v = &fr.verts[op.first * kFloatsPerVertex];
            const Ubo& u = fr.ubos[op.ubo];
            std::fprintf(stderr, "  draw mode=%u n=%zu tex=%d/%d through=%g clear=%g sc=%d,%d,%d,%d fb=%gx%g v0=(%g,%g,%g) uv=(%g,%g) c=(%g,%g,%g,%g) mask=%d%d%d%d blend=%d depth=%d/%x\n",
                         op.glMode, op.count, op.st.texKind, op.st.texId, u.vpScale[3], u.texFlags[2], op.st.scissor[0], op.st.scissor[1],
                         op.st.scissor[2], op.st.scissor[3], u.offsetFb[2], u.offsetFb[3], v[0], v[1], v[2], v[6], v[7], v[8], v[9], v[10], v[11],
                         op.st.colorMask[0], op.st.colorMask[1], op.st.colorMask[2], op.st.colorMask[3], op.st.blend, op.st.depthTest, op.st.depthFunc);
            if (full)
                std::fprintf(stderr, "     stencil=%d func=%x ref=%d mask=%x ops=%x/%x/%x wr=%x aconst=%d(%g) blend src=%x dst=%x eq=%x const=(%g,%g,%g) premul=%g texfunc=%g alphaUse=%g dbl=%g alphatest=%g/%g/%g fog=%g light=%g\n",
                             op.st.stencil, op.st.stencilFunc, op.st.stencilRef, op.st.stencilMask, op.st.sfail, op.st.zfail, op.st.zpass, op.st.stencilWrite, op.st.alphaConst, op.st.blendColor[3],
                             op.st.srcRGB, op.st.dstRGB, op.st.eqRGB, op.st.blendColor[0], op.st.blendColor[1], op.st.blendColor[2],
                             u.blendPremul[3], u.texEnv[3], u.texFlags[0], u.texFlags[1], u.alphaTest[0], u.alphaTest[1], u.alphaTest[2], u.flags1[2], u.flags0[0]);
        } else if (op.type != OP_DRAW) {
            std::fprintf(stderr, "  op %d id=%d id2=%d %dx%d @%d,%d\n", (int)op.type, op.id, op.id2, op.w, op.h, op.x, op.y);
        }
    }
}

void execFrame(Frame& fr, PresentInfo& pinfo, bool& presented) {
    ScopedUs perf(g_perf.execUs);
    g_perf.verts += fr.verts.size() / kFloatsPerVertex;
    if (g_perfOn) perfReport();
    debugFrame(fr);
    glBindVertexArray(g_exec.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_exec.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(fr.verts.size() * sizeof(float)), fr.verts.data(), GL_STREAM_DRAW);
    glUseProgram(g_exec.program);
    g_exec.bound = -1;
    int drawIndex = 0;
    for (Op& op : fr.ops) {
        if (op.type == OP_DRAW && g_maxDraws >= 0 && drawIndex++ >= g_maxDraws) continue;
        if (op.type == OP_DRAW && g_skipFbTex && op.st.texKind == 2) continue;
        switch (op.type) {
        case OP_CREATE_FBO: {
            GlFbo& f = g_exec.fbos[op.id];
            if (f.w != op.w || f.h != op.h || !f.fbo) createFbo(f, op.w, op.h);
            break;
        }
        case OP_BIND_FBO: bindTarget(op.id); break;
        case OP_CLEAR_FBO: {
            auto it = g_exec.fbos.find(op.id);
            if (it == g_exec.fbos.end()) break;
            glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            g_exec.bound = -1;
            break;
        }
        case OP_UPLOAD_TEX: {
            GLuint& t = g_exec.textures[op.id];
            if (!t) glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            size_t off = op.blob;
            for (size_t l = 0; l < op.levels.size(); ++l) {
                auto [lw, lh] = op.levels[l];
                glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, fr.blob.data() + off);
                off += (size_t)lw * lh * 4;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)op.levels.size() - 1);
            break;
        }
        case OP_UPLOAD_FBO_RECT: {
            auto it = g_exec.fbos.find(op.id);
            if (it == g_exec.fbos.end()) break;
            GlFbo& f = it->second;
            // Upload to a temporary texture and blit it (scaled) into the framebuffer.
            GLuint tex = 0, tmp = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, op.w, op.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fr.blob.data() + op.blob);
            glGenFramebuffers(1, &tmp);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f.fbo);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            const int s = std::max(1, g_settings.scale);
            glBlitFramebuffer(0, 0, op.w, op.h, op.x * s, op.y * s, (op.x + op.w) * s, (op.y + op.h) * s, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glDeleteFramebuffers(1, &tmp);
            glDeleteTextures(1, &tex);
            g_exec.bound = -1;
            break;
        }
        case OP_BLIT_FBO: {
            auto a = g_exec.fbos.find(op.id), b = g_exec.fbos.find(op.id2);
            if (a == g_exec.fbos.end() || b == g_exec.fbos.end()) break;
            const int s = std::max(1, g_settings.scale);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, a->second.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->second.fbo);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            glBlitFramebuffer(op.x * s, op.y * s, (op.x + op.w) * s, (op.y + op.h) * s, op.x2 * s, op.y2 * s,
                              (op.x2 + op.w) * s, (op.y2 + op.h) * s, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            g_exec.bound = -1;
            break;
        }
        case OP_DRAW: {
            if (g_exec.bound <= 0) break;
            GlFbo& target = g_exec.fbos[g_exec.bound];
            GLuint tex = op.st.texKind ? textureFor(op.st) : 0;
            if (g_exec.bound == -1) break;
            applyState(op.st, target);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            if (tex) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)op.st.minFilter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)op.st.magFilter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)op.st.wrapS);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)op.st.wrapT);
                if (op.st.texKind == 1) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, op.st.maxLevel);
            }
            glBindBuffer(GL_UNIFORM_BUFFER, g_exec.ubo);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(Ubo), &fr.ubos[op.ubo], GL_STREAM_DRAW);
            glDrawArrays(op.glMode, (GLint)op.first, (GLsizei)op.count);
            break;
        }
        case OP_PRESENT: {
            ++g_perf.presents;
            auto it = g_exec.fbos.find(op.id);
            if (it == g_exec.fbos.end()) break;
            pinfo.texture = it->second.color;
            pinfo.width = it->second.rw;
            pinfo.height = it->second.rh;
            pinfo.u1 = (float)op.w / it->second.w;
            pinfo.v1 = (float)op.h / it->second.h;
            pinfo.frame = fr.number;
            g_exec.lastFbo = op.id;
            presented = true;
            break;
        }
        case OP_DELETE_TEX: break;
        }
    }
}

Recorder* g_recorder = nullptr;

} // namespace

// ---- public API ------------------------------------------------------------------------

void install(const Settings& s) {
    g_settings = s;
    if (!g_recorder) g_recorder = new Recorder();
    ge::setRenderer(g_recorder);
    g_installed = true;
}
bool installed() { return g_installed; }
void setDisplay(uint32_t top, uint32_t stride, uint32_t fmt) { if (g_recorder) g_recorder->setDisplay(top, stride, fmt); }
void vblank() { if (g_recorder) g_recorder->vblank(); }
void notifyMemoryWrite(uint32_t addr, uint32_t size) { if (g_recorder) g_recorder->memoryWrite(addr, size); }

bool renderInit() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    g_exec.program = glCreateProgram();
    glAttachShader(g_exec.program, vs);
    glAttachShader(g_exec.program, fs);
    glBindAttribLocation(g_exec.program, 0, "aPos");
    glBindAttribLocation(g_exec.program, 1, "aNrm");
    glBindAttribLocation(g_exec.program, 2, "aUV");
    glBindAttribLocation(g_exec.program, 3, "aColor");
    glLinkProgram(g_exec.program);
    GLint ok = 0;
    glGetProgramiv(g_exec.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(g_exec.program, sizeof log, nullptr, log);
        std::fprintf(stderr, "[GPU] program link failed:\n%s\n", log);
        return false;
    }
    glUseProgram(g_exec.program);
    glUniform1i(glGetUniformLocation(g_exec.program, "uTex"), 0);
    GLuint bi = glGetUniformBlockIndex(g_exec.program, "Draw");
    glUniformBlockBinding(g_exec.program, bi, 0);
    glGenBuffers(1, &g_exec.ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_exec.ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(Ubo), nullptr, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_exec.ubo);
    glGenVertexArrays(1, &g_exec.vao);
    glBindVertexArray(g_exec.vao);
    glGenBuffers(1, &g_exec.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_exec.vbo);
    const GLsizei stride = kFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(8 * sizeof(float)));
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    return true;
}

bool renderPump(PresentInfo& out, int timeoutMs) {
    bool presented = false;
    for (;;) {
        std::unique_ptr<Frame> fr;
        {
            std::unique_lock<std::mutex> lk(g_queueMutex);
            if (g_queue.empty()) {
                if (presented || timeoutMs <= 0) break;
                g_queueCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return !g_queue.empty(); });
                timeoutMs = 0;
                if (g_queue.empty()) break;
            }
            fr = std::move(g_queue.front());
            g_queue.pop_front();
        }
        g_queueCv.notify_all();
        execFrame(*fr, out, presented);
        if (presented) break;
    }
    if (presented) g_exec.last = out;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return presented;
}

bool readPresented(const PresentInfo& p, std::vector<uint32_t>& rgba, int& w, int& h) {
    auto it = g_exec.fbos.find(g_exec.lastFbo);
    if (it == g_exec.fbos.end()) return false;
    GlFbo& f = it->second;
    w = (int)(p.u1 * f.rw + 0.5f);
    h = (int)(p.v1 * f.rh + 0.5f);
    rgba.resize((size_t)w * h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data()); // row 0 = PSP top row
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return true;
}

void renderShutdown() {}

void dumpNextFrames(int n) { g_dumpFrames = n; }
void setMaxDraws(int n) { g_maxDraws = n; }
void setSkipFramebufferTextures(bool b) { g_skipFbTex = b; }

} // namespace psp::gpu
