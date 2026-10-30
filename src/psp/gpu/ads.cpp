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

constexpr float kGameFovDeg = 80.0f; // the game's own horizontal field of view (projection 1.192)
constexpr float kDegToRad = 3.14159265f / 180.0f;

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
void rotZ(float* m, float a) { identity(m); m[0] = std::cos(a); m[1] = std::sin(a); m[4] = -std::sin(a); m[5] = std::cos(a); }

void xform(const float* m12, const float* p, float* out) { // GE 4x3 (columns of 3)
    for (int r = 0; r < 3; ++r) out[r] = m12[r] * p[0] + m12[3 + r] * p[1] + m12[6 + r] * p[2] + m12[9 + r];
}

double nowUs() {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float smoothstep(float t) { return t * t * (3 - 2 * t); }

// Projection scale that turns the game's field of view into `deg`.
float fovScale(float deg) {
    deg = std::clamp(deg, 20.0f, 150.0f);
    return std::tan(kGameFovDeg * 0.5f * kDegToRad) / std::tan(deg * 0.5f * kDegToRad);
}

} // namespace

// The weapon's camera: the identity view.
bool AimDownSights::isWeaponCamera(const float* v) {
    static const float kId[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    for (int i = 0; i < 12; ++i)
        if (std::fabs(v[i] - kId[i]) > 1e-3f) return false;
    return true;
}

void AimDownSights::observe(const float view[12], const float world[12], bool skinned, const std::vector<Vertex>& verts) {
    muzzleEffect_ = false;
    if (!isWeaponCamera(view)) {
        // The muzzle flash and the shot's trail are small quads drawn with the level's camera
        // from the weapon's muzzle (eye space x ~0.1, y ~-0.1, starting ~0.6 ahead): while
        // aiming they move with the weapon.
        if (t_ <= 0 || verts.empty() || verts.size() > 16) return;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (const Vertex& vx : verts) {
            float w[3], e[3];
            xform(world, vx.pos, w);
            xform(view, w, e);
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], e[k]); hi[k] = std::max(hi[k], e[k]); }
        }
        muzzleEffect_ = lo[0] > -0.5f && hi[0] < 0.5f && lo[1] > -0.5f && hi[1] < 0.3f && hi[2] < 0 && hi[2] > -1.5f;
        static const bool dbg = std::getenv("CA_ADS_DEBUG") != nullptr;
        if (dbg && muzzleEffect_) {
            const float c[4] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[2], 1};
            float o[3];
            for (int r = 0; r < 3; ++r) o[r] = adsMat_[r] * c[0] + adsMat_[4 + r] * c[1] + adsMat_[8 + r] * c[2] + adsMat_[12 + r];
            std::fprintf(stderr, "[ADS] muzzle effect %zu verts near end (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)\n", verts.size(), c[0], c[1],
                         c[2], o[0], o[1], o[2]);
        }
        return;
    }
    // The arms (skinned) move with the weapon but are not measured; below a dozen vertices:
    // sprites and particles, not the weapon.
    if (skinned || verts.size() < 12) return;
    std::vector<float> pts;
    pts.reserve(verts.size() * 3);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const Vertex& vx : verts) {
        float w[3], e[3];
        xform(world, vx.pos, w);
        xform(view, w, e);
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], e[k]); hi[k] = std::max(hi[k], e[k]); pts.push_back(e[k]); }
    }
    // Flat (billboards) or behind the eye: effects, not the weapon.
    if (hi[2] - lo[2] < 1e-3f || lo[2] > 0) return;
    gun_.insert(gun_.end(), pts.begin(), pts.end());
    static const bool dbg = std::getenv("CA_ADS_DEBUG") && std::atoi(std::getenv("CA_ADS_DEBUG")) >= 2;
    if (dbg && host::g_ads.load())
        std::fprintf(stderr, "[ADS] draw %zu verts x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f\n", verts.size(), lo[0], hi[0], lo[1],
                     hi[1], lo[2], hi[2]);
}

void AimDownSights::endFrame() {
    const auto& cfg = host::g_adsSettings;
    const double now = nowUs();
    const float dt = lastUs_ ? (float)std::min(0.1, (now - lastUs_) / 1e6) : 0.0f;
    lastUs_ = now;
    const bool want = host::g_ads.load() && cfg.enabled.load();

    if (want && t_ == 0) aligned_ = baseValid_ = false;
    const size_t gunVerts = gun_.size() / 3;
    // The weapon is measured every frame while aiming. The alignment taken when aiming starts
    // (`base`) puts the sights on the centre and is then kept, so the game's own sway, recoil
    // and bob still move the weapon naturally; WeaponSway (0..1) blends towards the current
    // frame's measurement, which holds the sights still (0 = locked on the centre). Applied to
    // the same frame's weapon draws (gpu.cpp), never a frame late.
    bool measured = false;
    if (want && gunVerts >= 12) {
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
            const double b = den > 1e-9 ? (n * sxz - sx * sz) / den : 0, d = den > 1e-9 ? (n * syz - sy * sz) / den : 0;
            // The sight picture: the top of the front sight (the front quarter of the top
            // edge) on the centre, framed by the rear sight's notch, which stands higher.
            float zFront = 1e9f, zBack = -1e9f;
            for (size_t i : topIdx_) { zFront = std::min(zFront, gun_[i + 2]); zBack = std::max(zBack, gun_[i + 2]); }
            const float zCut = zFront + 0.25f * (zBack - zFront);
            double fn = 0;
            float fy = -1e9f, fz = 0;
            for (size_t i : topIdx_) {
                if (gun_[i + 2] > zCut) continue;
                fn += 1;
                if (gun_[i + 1] > fy) { fy = gun_[i + 1]; fz = gun_[i + 2]; }
            }
            sight_[1] = fn > 0 ? fy : maxY;
            sight_[2] = fn > 0 ? fz : (float)(sz / n);
            // Horizontally the weapon is centred on its upper part's centre line (slide,
            // receiver): the middle between its left and right sides in slices along the
            // barrel, fitted as x = a + b z. The top edge alone is lopsided where the rear
            // sight is, which turned the weapon and showed one side of the slide.
            const float band = maxY - 0.35f * (maxY - minY);
            double a = (sx - b * sz) / n, bc = b;
            {
                float z0 = 1e9f, z1 = -1e9f;
                for (size_t i = 0; i < gun_.size(); i += 3)
                    if (gun_[i + 1] >= band) { z0 = std::min(z0, gun_[i + 2]); z1 = std::max(z1, gun_[i + 2]); }
                constexpr int kSlices = 8;
                float lo[kSlices], hi[kSlices];
                double zs[kSlices] = {}, zn[kSlices] = {};
                std::fill(std::begin(lo), std::end(lo), 1e9f);
                std::fill(std::begin(hi), std::end(hi), -1e9f);
                for (size_t i = 0; i < gun_.size() && z1 > z0; i += 3) {
                    if (gun_[i + 1] < band) continue;
                    const int k = std::min(kSlices - 1, (int)((gun_[i + 2] - z0) / (z1 - z0) * kSlices));
                    lo[k] = std::min(lo[k], gun_[i]);
                    hi[k] = std::max(hi[k], gun_[i]);
                    zs[k] += gun_[i + 2];
                    zn[k] += 1;
                }
                double cn = 0, cz = 0, czz = 0, cx = 0, cxz = 0;
                for (int k = 0; k < kSlices; ++k) {
                    if (zn[k] < 2 || hi[k] <= lo[k]) continue;
                    const double z = zs[k] / zn[k], x = 0.5 * (lo[k] + hi[k]);
                    cn += 1; cz += z; czz += z * z; cx += x; cxz += x * z;
                }
                const double cden = cn * czz - cz * cz;
                if (cn >= 2 && cden > 1e-12) {
                    bc = (cn * cxz - cx * cz) / cden;
                    a = (cx - bc * cz) / cn;
                } else if (cn >= 1) {
                    bc = 0;
                    a = cx / cn;
                }
            }
            sight_[0] = (float)(a + bc * sight_[2]);
            // Turn the barrel (bc, d, 1) onto the z axis: yaw first, then pitch.
            yaw_ = (float)std::atan(-std::clamp(bc, -0.5, 0.5));
            pitch_ = (float)std::atan2(std::clamp(d, -0.5, 0.5), std::sqrt(1 + bc * bc));
            // Cant: the upper part of the weapon, with its centre line taken out, leans by
            // dx/dy; turning about the line of sight by atan(dx/dy) stands it upright.
            const double b2 = bc;
            double m = 0, sy2 = 0, syy = 0, sr = 0, sry = 0;
            for (size_t i = 0; i < gun_.size(); i += 3) {
                if (gun_[i + 1] < band) continue;
                const double y = gun_[i + 1], r = gun_[i] - (a + b2 * gun_[i + 2]);
                m += 1; sy2 += y; syy += y * y; sr += r; sry += r * y;
            }
            const double dy = m * syy - sy2 * sy2;
            roll_ = m >= 6 && dy > 1e-12 ? (float)std::atan(std::clamp((m * sry - sr * sy2) / dy, -0.5, 0.5)) : 0.0f;
            aligned_ = true;
            measured = true;
        }
    }
    if (measured) {
        // A different weapon (its vertex count changes) takes a new base alignment.
        if (baseValid_ && (gunVerts * 4 < baseVerts_ * 3 || gunVerts * 3 > baseVerts_ * 4)) baseValid_ = false;
        if (!baseValid_) {
            std::copy(sight_, sight_ + 3, baseSight_);
            baseYaw_ = yaw_; basePitch_ = pitch_; baseRoll_ = roll_;
            baseVerts_ = gunVerts;
            baseValid_ = true;
        }
        const float k = std::clamp(cfg.weaponSway.load(), 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i) sight_[i] += k * (baseSight_[i] - sight_[i]);
        yaw_ += k * (baseYaw_ - yaw_);
        pitch_ += k * (basePitch_ - pitch_);
        roll_ += k * (baseRoll_ - roll_);
    }
    static const bool dbg = std::getenv("CA_ADS_DEBUG") != nullptr;
    if (dbg && want)
        std::fprintf(stderr, "[ADS] gun %zu aligned %d t %.2f sight %.3f,%.3f,%.3f yaw %.3f pitch %.3f roll %.3f\n",
                     gunVerts, (int)aligned_, t_, sight_[0], sight_[1], sight_[2], yaw_, pitch_, roll_);
    gun_.clear();

    // Towards the target at a constant rate (independent of the frame rate), without
    // overshooting (stepping away from it and back made the weapon and the view shake).
    const float target = (want && aligned_) ? 1.0f : 0.0f;
    const float step = dt / std::max(0.01f, cfg.transitionS.load());
    t_ = target > t_ ? std::min(target, t_ + step) : std::max(target, t_ - step);
    if (target == 0 && t_ == 0) aligned_ = false;
    const float s = smoothstep(t_);

    // The level's field of view: the normal one, narrowing to the aiming one.
    const float hip = fovScale(cfg.normalFov.load()), aim = fovScale(cfg.adsFov.load());
    fovScale_ = hip + (aim - hip) * s;

    // M(t) = T(S + (S' - S) t) * Rz(roll t) * Rx(pitch t) * Ry(yaw t) * T(-S), S' = the sight
    // on the axis, brought `weaponCloser` of its distance towards the eye.
    const float sx = sight_[0] + cfg.sightX.load(), sy = sight_[1] + cfg.sightY.load();
    const float closer = std::clamp(cfg.weaponCloser.load(), 0.0f, 0.6f);
    float a[16], r[16], b[16], c[16], d[16];
    translate(a, sx * (1 - s), sy * (1 - s), sight_[2] * (1 - closer * s));
    rotZ(r, roll_ * s);
    rotX(b, pitch_ * s);
    rotY(c, yaw_ * s);
    translate(d, -sx, -sy, -sight_[2]);
    mul(a, r, adsMat_);
    mul(adsMat_, b, adsMat_);
    mul(adsMat_, c, adsMat_);
    mul(adsMat_, d, adsMat_);
}

void AimDownSights::applyWeapon(float view44[16]) const {
    if (t_ > 0) mul(adsMat_, view44, view44);
}

void AimDownSights::adjust(const float view[12], float viewportScaleX, float view44[16], float proj44[16]) const {
    (void)view44;
    if (isWeaponDraw(view)) return; // applyWeapon() at the end of the frame
    if (std::fabs(viewportScaleX) > 200 && fovScale_ != 1) { // the level (full screen; the radar is small)
        proj44[0] *= fovScale_;
        proj44[5] *= fovScale_;
    }
}

bool AimDownSights::hide2D(const std::vector<Vertex>& verts) const {
    if (t_ < 0.25f || verts.empty() || !host::g_adsSettings.hideCrosshair.load()) return false;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const Vertex& v : verts)
        for (int k = 0; k < 2; ++k) { lo[k] = std::min(lo[k], v.pos[k]); hi[k] = std::max(hi[k], v.pos[k]); }
    // The crosshair: small pieces wholly inside the middle of the screen (480x272).
    return lo[0] > 240 - 40 && hi[0] < 240 + 40 && lo[1] > 136 - 40 && hi[1] < 136 + 40 && hi[0] - lo[0] < 48 &&
           hi[1] - lo[1] < 48;
}

} // namespace psp::gpu
