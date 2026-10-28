#include "psp/gpu/minimap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "psp/gpu/ge_state.h"

namespace psp::gpu::minimap {

namespace {

constexpr float kCell = 2.0f; // world units per grid cell
constexpr int kTile = 64;     // cells per tile side
// Heights seen in a cell (world units, rounded); empty ranges have min > max. What is shown
// is decided when drawing, from the current camera height, so floors above or below the
// player (other storeys) are left out.
struct Cell {
    int16_t floorMin = INT16_MAX, floorMax = INT16_MIN, wallMin = INT16_MAX, wallMax = INT16_MIN;
};

struct Item {
    uint64_t key;
    std::shared_ptr<const std::vector<Vertex>> verts;
    int prim;
    float world[12], view[12];
    bool skinned;
};

struct Batch {
    std::vector<Item> items;
};

using Clock = std::chrono::steady_clock;

std::mutex g_frameMutex;   // the frame being recorded
std::vector<Item> g_frame;

std::mutex g_mapMutex;     // grid, camera, seen geometry
std::unordered_map<int64_t, std::vector<Cell>> g_tiles;
std::unordered_set<uint64_t> g_seen;
struct Camera {
    bool valid = false;
    float x = 0, y = 0, z = 0;
    float fwdX = 0, fwdZ = 1, rightX = 1, rightZ = 0;
    Clock::time_point when{};
} g_cam;
struct Mark {
    float x, z;
    Clock::time_point when;
};
std::vector<Mark> g_marks;

std::mutex g_workMutex;
std::condition_variable g_workCv;
std::deque<Batch> g_work;
std::atomic<bool> g_enabled{true};
bool g_workerStarted = false;

int64_t tileKey(int tx, int tz) { return ((int64_t)tx << 32) ^ (int64_t)(uint32_t)tz; }

// Grid access; g_mapMutex held.
Cell* cellPtr(int cx, int cz, bool create) {
    const int tx = (int)std::floor((float)cx / kTile), tz = (int)std::floor((float)cz / kTile);
    auto it = g_tiles.find(tileKey(tx, tz));
    if (it == g_tiles.end()) {
        if (!create) return nullptr;
        it = g_tiles.emplace(tileKey(tx, tz), std::vector<Cell>(kTile * kTile)).first;
    }
    return &it->second[(size_t)(cz - tz * kTile) * kTile + (cx - tx * kTile)];
}
int16_t h16(float y) { return (int16_t)std::clamp((int)std::lround(y), -32000, 32000); }
void markFloor(float x, float z, float y) {
    Cell* c = cellPtr((int)std::floor(x / kCell), (int)std::floor(z / kCell), true);
    c->floorMin = std::min(c->floorMin, h16(y));
    c->floorMax = std::max(c->floorMax, h16(y));
}
void markWall(float x, float z, float y0, float y1) {
    Cell* c = cellPtr((int)std::floor(x / kCell), (int)std::floor(z / kCell), true);
    c->wallMin = std::min(c->wallMin, h16(y0));
    c->wallMax = std::max(c->wallMax, h16(y1));
}

void transform(const float* m, const float* p, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[3 + r] * p[1] + m[6 + r] * p[2] + m[9 + r];
}

void wallLine(const float* a, const float* b, float y0, float y1) {
    const float dx = b[0] - a[0], dz = b[2] - a[2];
    const int steps = std::max(1, (int)(std::sqrt(dx * dx + dz * dz) / (kCell * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / steps;
        markWall(a[0] + dx * t, a[2] + dz * t, y0, y1);
    }
}

void floorTri(const float* a, const float* b, const float* c) {
    const float y = (a[1] + b[1] + c[1]) / 3;
    const float minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
    const float minZ = std::min({a[2], b[2], c[2]}), maxZ = std::max({a[2], b[2], c[2]});
    if (maxX - minX > 4000 || maxZ - minZ > 4000) return; // degenerate / sky
    const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (std::fabs(d) < 1e-6f) return;
    bool any = false;
    for (float z = std::floor(minZ / kCell) * kCell + kCell * 0.5f; z <= maxZ; z += kCell)
        for (float x = std::floor(minX / kCell) * kCell + kCell * 0.5f; x <= maxX; x += kCell) {
            const float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
            const float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
            if (l1 < 0 || l2 < 0 || l1 + l2 > 1) continue;
            markFloor(x, z, y);
            any = true;
        }
    if (!any) markFloor((a[0] + b[0] + c[0]) / 3, (a[2] + b[2] + c[2]) / 3, y);
}

void triangle(const float* a, const float* b, const float* c) {
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-6f) return;
    const float ny = std::fabs(n[1]) / len;
    const float minY = std::min({a[1], b[1], c[1]}), maxY = std::max({a[1], b[1], c[1]});
    if (ny > 0.8f) {
        floorTri(a, b, c); // floor or ceiling: told apart by height when drawing
    } else if (ny < 0.35f) {
        // A wall: the two vertices farthest apart in the ground plane.
        const float* p[3] = {a, b, c};
        float best = -1;
        int bi = 0, bj = 1;
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j) {
                const float dx = p[i][0] - p[j][0], dz = p[i][2] - p[j][2];
                if (dx * dx + dz * dz > best) { best = dx * dx + dz * dz; bi = i; bj = j; }
            }
        if (best < 4000 * 4000) wallLine(p[bi], p[bj], minY, maxY);
    }
}

void process(const Batch& b) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    std::vector<float> w;
    for (const Item& it : b.items) {
        const auto& vs = *it.verts;
        w.resize(vs.size() * 3);
        for (size_t i = 0; i < vs.size(); ++i) transform(it.world, vs[i].pos, &w[i * 3]);
        auto P = [&](size_t i) { return &w[i * 3]; };
        const size_t n = vs.size();
        if (it.prim == PRIM_TRIANGLES) {
            for (size_t i = 0; i + 2 < n; i += 3) triangle(P(i), P(i + 1), P(i + 2));
        } else if (it.prim == PRIM_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < n; ++i) triangle(P(i), P(i + 1), P(i + 2));
        } else if (it.prim == PRIM_TRIANGLE_FAN) {
            for (size_t i = 1; i + 1 < n; ++i) triangle(P(0), P(i), P(i + 1));
        }
    }
}

void worker() {
    for (;;) {
        Batch b;
        {
            std::unique_lock<std::mutex> lk(g_workMutex);
            g_workCv.wait(lk, [] { return !g_work.empty(); });
            b = std::move(g_work.front());
            g_work.pop_front();
        }
        process(b);
    }
}

uint64_t hashFloats(const float* f, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, &f[i], 4);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

} // namespace

void setEnabled(bool on) { g_enabled = on; }

void addDraw(uint64_t key, std::shared_ptr<const std::vector<Vertex>> verts, int prim, const float world[12],
             const float view[12], bool skinned) {
    if (!g_enabled || !verts || verts->empty()) return;
    Item it;
    it.key = key;
    it.verts = std::move(verts);
    it.prim = prim;
    std::memcpy(it.world, world, sizeof it.world);
    std::memcpy(it.view, view, sizeof it.view);
    it.skinned = skinned;
    std::lock_guard<std::mutex> lk(g_frameMutex);
    if (g_frame.size() < 20000) g_frame.push_back(std::move(it));
}

void endFrame() {
    std::vector<Item> items;
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        items.swap(g_frame);
    }
    if (items.empty()) return;
    // The main camera: the view matrix of most 3D draws.
    std::unordered_map<uint64_t, int> counts;
    uint64_t camKey = 0;
    int best = 0;
    const float* camView = nullptr;
    for (const Item& it : items) {
        const uint64_t k = hashFloats(it.view, 12);
        if (++counts[k] > best) { best = counts[k]; camKey = k; camView = it.view; }
    }
    if (best < 16) return; // not a 3D scene (menus, movies)
    const float* v = camView;
    Camera cam;
    cam.valid = true;
    cam.x = -(v[0] * v[9] + v[1] * v[10] + v[2] * v[11]);
    cam.y = -(v[3] * v[9] + v[4] * v[10] + v[5] * v[11]);
    cam.z = -(v[6] * v[9] + v[7] * v[10] + v[8] * v[11]);
    // Rows of the view rotation: right = row 0, the camera looks along -row 2.
    float fx = -v[2], fz = -v[8], rx = v[0], rz = v[6];
    float fl = std::sqrt(fx * fx + fz * fz), rl = std::sqrt(rx * rx + rz * rz);
    if (fl > 1e-4f) { cam.fwdX = fx / fl; cam.fwdZ = fz / fl; }
    if (rl > 1e-4f) { cam.rightX = rx / rl; cam.rightZ = rz / rl; }
    cam.when = Clock::now();
    // Characters (skinned meshes) drawn with the camera, merged by proximity.
    std::vector<Mark> marks;
    for (const Item& it : items) {
        if (!it.skinned || hashFloats(it.view, 12) != camKey) continue;
        float o[3] = {0, 0, 0}, p[3];
        for (const Vertex& vx : *it.verts)
            for (int k = 0; k < 3; ++k) o[k] += vx.pos[k] / (float)it.verts->size();
        transform(it.world, o, p);
        const float dx = p[0] - cam.x, dz = p[2] - cam.z;
        if (dx * dx + dz * dz < 8.0f * 8.0f) continue; // the player's own model
        bool merged = false;
        for (Mark& m : marks) {
            if ((m.x - p[0]) * (m.x - p[0]) + (m.z - p[2]) * (m.z - p[2]) < 20.0f * 20.0f) { merged = true; break; }
        }
        if (!merged) marks.push_back({p[0], p[2], cam.when});
    }

    Batch batch;
    {
        std::lock_guard<std::mutex> lk(g_mapMutex);
        // A new level: the 3D scene comes back after a pause somewhere far away.
        if (g_cam.valid && cam.when - g_cam.when > std::chrono::seconds(1)) {
            const float dx = cam.x - g_cam.x, dz = cam.z - g_cam.z;
            if (dx * dx + dz * dz > 300.0f * 300.0f) {
                g_tiles.clear();
                g_seen.clear();
            }
        }
        g_cam = cam;
        // Keep characters seen in the last half second (they flicker in and out of view).
        g_marks.erase(std::remove_if(g_marks.begin(), g_marks.end(),
                                     [&](const Mark& m) { return cam.when - m.when > std::chrono::milliseconds(500); }),
                      g_marks.end());
        for (const Mark& m : marks) {
            bool replaced = false;
            for (Mark& o : g_marks)
                if ((o.x - m.x) * (o.x - m.x) + (o.z - m.z) * (o.z - m.z) < 20.0f * 20.0f) { o = m; replaced = true; break; }
            if (!replaced) g_marks.push_back(m);
        }
        for (Item& it : items) {
            if (it.skinned || hashFloats(it.view, 12) != camKey) continue;
            if (it.key) {
                const uint64_t k = it.key ^ (hashFloats(it.world, 12) * 0x9E3779B97F4A7C15ull);
                if (!g_seen.insert(k).second) continue;
            }
            batch.items.push_back(std::move(it));
        }
    }
    if (batch.items.empty()) return;
    std::lock_guard<std::mutex> lk(g_workMutex);
    if (!g_workerStarted) {
        std::thread(worker).detach();
        g_workerStarted = true;
    }
    if (g_work.size() < 8) g_work.push_back(std::move(batch));
    g_workCv.notify_one();
}

bool render(std::vector<uint32_t>& rgba, int size, float range) {
    rgba.assign((size_t)size * size, 0);
    if (!g_enabled) return false;
    std::lock_guard<std::mutex> lk(g_mapMutex);
    if (!g_cam.valid || Clock::now() - g_cam.when > std::chrono::milliseconds(300)) return false;
    const float c = size * 0.5f, scale = range / c;
    auto put = [&](int x, int y, uint32_t col) {
        if (x >= 0 && y >= 0 && x < size && y < size) rgba[(size_t)y * size + x] = col;
    };
    auto rgbaOf = [](int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; };
    const uint32_t bg = rgbaOf(6, 18, 20, 170), floorC = rgbaOf(34, 82, 74, 215), wallC = rgbaOf(120, 245, 205, 255),
                   ring = rgbaOf(80, 230, 170, 230);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float sx = (x + 0.5f - c), sy = (c - (y + 0.5f));
            const float r = std::sqrt(sx * sx + sy * sy);
            if (r > c - 1) continue;
            if (r > c - 3) { put(x, y, ring); continue; }
            const float wx = g_cam.x + (g_cam.rightX * sx + g_cam.fwdX * sy) * scale;
            const float wz = g_cam.z + (g_cam.rightZ * sx + g_cam.fwdZ * sy) * scale;
            const Cell* cell = cellPtr((int)std::floor(wx / kCell), (int)std::floor(wz / kCell), false);
            uint32_t col = bg;
            if (cell) {
                const float cy = g_cam.y;
                if (cell->wallMin <= cy + 4 && cell->wallMax >= cy - 12) col = wallC; // at body height
                else if (cell->floorMin <= cy - 3 && cell->floorMax >= cy - 45) col = floorC; // below the eye
            }
            put(x, y, col);
        }
    // Characters: red dots, kept on the rim when out of range.
    const uint32_t foe = rgbaOf(255, 70, 60, 255);
    for (const Mark& m : g_marks) {
        const float dx = m.x - g_cam.x, dz = m.z - g_cam.z;
        float sx = (dx * g_cam.rightX + dz * g_cam.rightZ) / scale, sy = (dx * g_cam.fwdX + dz * g_cam.fwdZ) / scale;
        const float r = std::sqrt(sx * sx + sy * sy), maxR = c - 7;
        if (r > maxR) { sx *= maxR / r; sy *= maxR / r; }
        const int px = (int)(c + sx), py = (int)(c - sy);
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx2 = -3; dx2 <= 3; ++dx2)
                if (dx2 * dx2 + dy * dy <= 10) put(px + dx2, py + dy, foe);
    }
    // The player: an arrow pointing up (the view direction).
    const uint32_t me = rgbaOf(255, 255, 255, 255);
    for (int dy = -6; dy <= 6; ++dy) {
        const int half = (dy + 6) / 2;
        for (int dx = -half; dx <= half; ++dx) put((int)c + dx, (int)c + dy, me);
    }
    return true;
}

} // namespace psp::gpu::minimap
