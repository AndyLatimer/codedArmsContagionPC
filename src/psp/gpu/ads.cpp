#include "psp/gpu/ads.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psp/host.h"

namespace psp::gpu {

namespace {

constexpr float kZoom = 1.3f;       // field of view narrowed by this factor while aiming
constexpr float kTransitionS = 0.15f;
constexpr float kCloser = 0.8f;     // the weapon comes this much towards the eye
constexpr float kSteady = 0.85f;    // share of the weapon's sway taken out while aiming

using Mat = float[16]; // column-major

void identity(float* m) { std::memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1; }
void mul(const float* a, const float* b, float* out) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = v;
        }
    std::memcpy(out, r, sizeof r);
}
void translate(float* m, float x, float y, float z) { identity(m); m[12] = x; m[13] = y; m[14] = z; }
void rotX(float* m, float a) { identity(m); m[5] = std::cos(a); m[6] = std::sin(a); m[9] = -std::sin(a); m[10] = std::cos(a); }
void rotY(float* m, float a) { identity(m); m[0] = std::cos(a); m[2] = -std::sin(a); m[8] = std::sin(a); m[10] = std::cos(a); }

void xform(const float* m12, const float* p, float* out) { // GE 4x3 (columns of 3)
    for (int r = 0; r < 3; ++r) out[r] = m12[r] * p[0] + m12[3 + r] * p[1] + m12[6 + r] * p[2] + m12[9 + r];
}

double nowUs() {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// The weapon's camera: near the origin, axes along the view's (this game's is turned 180 degrees).
bool AimDownSights::isWeaponCamera(const float* v) {
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = -(v[3 * k] * v[9] + v[3 * k + 1] * v[10] + v[3 * k + 2] * v[11]);
    return std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[2]) < 20 && std::fabs(v[0]) > 0.95f && v[4] > 0.95f && std::fabs(v[8]) > 0.95f;
}

void AimDownSights::observe(const float view[12], const float world[12], const std::vector<Vertex>& verts) {
    // Below a dozen vertices: sprites and particles (muzzle flashes, flames), not the weapon.
    if (!isWeaponCamera(view) || verts.size() < 12) return;
    std::vector<float> pts;
    pts.reserve(verts.size() * 3);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const Vertex& vx : verts) {
        float w[3], e[3];
        xform(world, vx.pos, w);
        xform(view, w, e);
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], e[k]); hi[k] = std::max(hi[k], e[k]); pts.push_back(e[k]); }
    }
    // Flat (billboards) or wholly far ahead (shots, rockets): effects, not the weapon.
    if (hi[2] - lo[2] < 0.01f || hi[2] < -9.0f) return;
    // The player's arms are the same seven meshes with every weapon (verified with all 23 in
    // dev mode), drawn after the weapon; the rest of the weapon camera's draws are the weapon.
    static constexpr size_t kArmMeshes[] = {101, 186, 305, 85, 143, 61, 279};
    const bool arm = std::find(std::begin(kArmMeshes), std::end(kArmMeshes), verts.size()) != std::end(kArmMeshes);
    if (arm) armsSeen_ = true;
    else notArms_.insert(notArms_.end(), pts.begin(), pts.end());
    // Fallback when the arms are not recognised: narrow, or wholly in front of the camera (the
    // arms are wide and reach back); failing that, everything in front of the hands' reach.
    const bool gun = hi[0] - lo[0] < 1.2f || hi[2] < -2.0f;
    if (gun) gun_.insert(gun_.end(), pts.begin(), pts.end());
    static const bool dbg = std::getenv("CA_ADS_DEBUG") && std::atoi(std::getenv("CA_ADS_DEBUG")) >= 2;
    if (dbg && host::g_ads.load())
        std::fprintf(stderr, "[ADS] draw %zu verts x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f%s\n", verts.size(), lo[0], hi[0], lo[1],
                     hi[1], lo[2], hi[2], arm ? " arm" : "");
    for (size_t i = 0; i < pts.size(); i += 3)
        if (pts[i + 2] < -2.0f) front_.insert(front_.end(), pts.begin() + (long)i, pts.begin() + (long)i + 3);
}

void AimDownSights::endFrame() {
    const double now = nowUs();
    const float dt = lastUs_ ? (float)std::min(0.1, (now - lastUs_) / 1e6) : 0.0f;
    lastUs_ = now;
    const bool want = host::g_ads.load();


    // Measure the weapon when aiming starts (from hip), then keep that alignment; measure again
    // when a different weapon appears (its vertex count changes for a few frames).
    if (want && t_ == 0) aligned_ = false;
    if (armsSeen_ && notArms_.size() >= 3 * 12) gun_.swap(notArms_);
    else if (gun_.size() < 3 * 12) gun_.swap(front_);
    front_.clear();
    notArms_.clear();
    armsSeen_ = false;
    const size_t gunVerts = gun_.size() / 3;
    if (want && aligned_ && gunVerts) {
        const bool other = gunVerts * 4 < alignedVerts_ * 3 || gunVerts * 3 > alignedVerts_ * 4;
        changed_ = other ? changed_ + 1 : 0;
        if (changed_ >= 5) { aligned_ = false; changed_ = 0; }
    }
    if (want && !aligned_ && gun_.size() >= 3 * 12) {
        float maxY = -1e9f, minY = 1e9f;
        for (size_t i = 0; i < gun_.size(); i += 3) { maxY = std::max(maxY, gun_[i + 1]); minY = std::min(minY, gun_[i + 1]); }
        const float edge = maxY - 0.12f * (maxY - minY);
        // Least squares over the top edge: x = a + b z, y = c + d z.
        double n = 0, sz = 0, szz = 0, sx = 0, sxz = 0, sy = 0, syz = 0;
        topIdx_.clear();
        for (size_t i = 0; i < gun_.size(); i += 3) {
            if (gun_[i + 1] < edge) continue;
            topIdx_.push_back(i);
            const double x = gun_[i], y = gun_[i + 1], z = gun_[i + 2];
            n += 1; sz += z; szz += z * z; sx += x; sxz += x * z; sy += y; syz += y * z;
        }
        if (n >= 3) {
            const double den = n * szz - sz * sz;
            const double b = den > 1e-6 ? (n * sxz - sx * sz) / den : 0, d = den > 1e-6 ? (n * syz - sy * sz) / den : 0;
            sight_[0] = (float)(sx / n);
            sight_[1] = maxY;
            sight_[2] = (float)(sz / n);
            // Turn the barrel (b, d, 1) onto the z axis: yaw first, then pitch.
            yaw_ = (float)std::atan(-std::clamp(b, -0.5, 0.5));
            pitch_ = (float)std::atan2(std::clamp(d, -0.5, 0.5), std::sqrt(1 + b * b));
            aligned_ = true;
            alignedVerts_ = gunVerts;
            refTop_[0] = (float)(sx / n);
            refTop_[1] = (float)(sy / n);
            sway_[0] = sway_[1] = 0;
        }
    } else if (want && aligned_ && gunVerts == alignedVerts_ && !topIdx_.empty()) {
        // Steady aim: the same top-edge vertices as when aiming started; most of the weapon's
        // own sway and bob since then is taken out (smoothed, so it does not shake).
        double mx = 0, my = 0;
        for (size_t i : topIdx_) { mx += gun_[i]; my += gun_[i + 1]; }
        const float dx = (float)(mx / topIdx_.size()) - refTop_[0], dy = (float)(my / topIdx_.size()) - refTop_[1];
        sway_[0] += (dx - sway_[0]) * 0.5f;
        sway_[1] += (dy - sway_[1]) * 0.5f;
    }
    static const bool dbg = std::getenv("CA_ADS_DEBUG") != nullptr;
    if (dbg && want)
        std::fprintf(stderr, "[ADS] gun %zu aligned %d t %.2f sight %.2f,%.2f,%.2f yaw %.3f pitch %.3f sway %.3f,%.3f\n",
                     gun_.size() / 3, (int)aligned_, t_, sight_[0], sight_[1], sight_[2], yaw_, pitch_, sway_[0], sway_[1]);
    gun_.clear();

    const float target = (want && aligned_) ? 1.0f : 0.0f;
    // Towards the target, without overshooting (stepping away from it and back made the
    // weapon and the view shake every frame).
    const float step = dt / kTransitionS;
    t_ = target > t_ ? std::min(target, t_ + step) : std::max(target, t_ - step);
    if (target == 0 && t_ == 0) aligned_ = false;

    // M(t) = T(S + (S' - S) t) * Rx(pitch t) * Ry(yaw t) * T(-S), S' = the sight on the axis.
    float a[16], b[16], c[16], d[16];
    const float s = t_ * t_ * (3 - 2 * t_); // smoothstep
    translate(a, sight_[0] * (1 - s), sight_[1] * (1 - s), sight_[2] + kCloser * s);
    rotX(b, pitch_ * s);
    rotY(c, yaw_ * s);
    translate(d, -sight_[0] - sway_[0] * kSteady * s, -sight_[1] - sway_[1] * kSteady * s, -sight_[2]);
    mul(a, b, adsMat_);
    mul(adsMat_, c, adsMat_);
    mul(adsMat_, d, adsMat_);
}

void AimDownSights::adjust(const float view[12], float viewportScaleX, float view44[16], float proj44[16]) const {
    if (t_ <= 0) return;
    if (isWeaponCamera(view)) {
        mul(adsMat_, view44, view44);
    } else if (std::fabs(viewportScaleX) > 200) { // the player's view (full screen; the radar is small)
        const float s = t_ * t_ * (3 - 2 * t_);
        const float z = 1 + (kZoom - 1) * s;
        proj44[0] *= z;
        proj44[5] *= z;
    }
}

bool AimDownSights::hide2D(const std::vector<Vertex>& verts) const {
    if (t_ < 0.25f || verts.empty()) return false;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const Vertex& v : verts)
        for (int k = 0; k < 2; ++k) { lo[k] = std::min(lo[k], v.pos[k]); hi[k] = std::max(hi[k], v.pos[k]); }
    // The crosshair: small pieces wholly inside the middle of the screen (480x272); while the
    // player moves it opens into separate ticks around the centre.
    return lo[0] > 240 - 56 && hi[0] < 240 + 56 && lo[1] > 136 - 56 && hi[1] < 136 + 56 && hi[0] - lo[0] < 80 &&
           hi[1] - lo[1] < 80;
}

} // namespace psp::gpu
