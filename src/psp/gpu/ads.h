#pragma once
// Aim down sights and field of view (features of the PC port, not in the original game).
//
// Contagion draws the player's weapon and arms with a camera of their own (the identity view
// matrix, with the same projection as the level but a nearer near plane) and the level with
// the player's camera. While the player aims (host::g_ads), the renderer moves the weapon and
// the arms together so that the weapon's sights sit on the centre of the screen, where the
// game's shots go, with the barrel along the line of sight; narrows the field of view of the
// level; and hides the crosshair. The game logic is untouched.
//
// The arms are the skinned draws of the weapon camera, the weapon the rest. Its sights are
// its top edge, and the barrel direction is fitted through that edge. The alignment is
// measured when aiming starts and then kept, so the weapon's own animations (recoil) still
// show. The field of view is scaled relative to the game's (its sniper scope zooms on top).
// Settings: host::g_adsSettings.
#include <cstdint>
#include <vector>

#include "psp/gpu/vertex_decoder.h"

namespace psp::gpu {

class AimDownSights {
public:
    // A 3D draw (4x3 GE matrices, model-space vertices): classifies it and measures the weapon.
    void observe(const float view[12], const float world[12], bool skinned, const std::vector<Vertex>& verts);
    // Adjusts the uniform matrices (4x4 column-major) of that draw.
    // `viewportScaleX`: the draw's viewport half width (240 full screen).
    void adjust(const float view[12], float viewportScaleX, float view44[16], float proj44[16]) const;
    // A 2D (through mode) draw: true to drop it (the crosshair, while aiming).
    bool hide2D(const std::vector<Vertex>& verts) const;
    void endFrame(); // once per presented frame

    static bool isWeaponCamera(const float view[12]);

private:
    float t_ = 0;                  // 0 = hip, 1 = aiming
    double lastUs_ = 0;
    std::vector<float> gun_;        // weapon vertices this frame (view space, xyz)
    bool aligned_ = false;          // an alignment was measured for this aim
    size_t alignedVerts_ = 0;       // weapon vertices when it was measured
    int changed_ = 0;               // frames with a different weapon
    float sight_[3] = {0, 0, 0};    // sight point (view space)
    float yaw_ = 0, pitch_ = 0;     // rotation that puts the barrel along -z
    float roll_ = 0;                // and the weapon upright (the hip pose is canted)
    std::vector<size_t> topIdx_;    // the weapon's top-edge vertices (offsets into gun_)
    float refTop_[2] = {0, 0};      // their mean x, y when aiming started
    float sway_[2] = {0, 0};        // their smoothed movement since then
    float adsMat_[16];              // current weapon camera adjustment (column-major)
    float fovScale_ = 1;            // level projection scale (> 1 narrows the view)
};

} // namespace psp::gpu
