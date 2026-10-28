#pragma once
// Aim down sights (a feature of the PC port, not in the original game).
//
// The game draws the player's weapon and arms with a camera of their own (a view matrix that
// is almost the identity, near the origin) and the level with the player's camera. While the
// player aims (host::g_ads), the renderer moves the weapon model so that its sights sit on the
// centre of the screen, where the game's shots go, with the barrel along the line of sight;
// narrows the field of view of the level; and hides the crosshair. The game logic is untouched.
//
// The weapon is told apart from the arms by their meshes (the same seven with every weapon), or
// failing that by its shape in its camera's space: narrow, or wholly in front of the camera. Its
// sights are the top edge of the weapon, and the barrel direction is fitted through that edge.
// The alignment is measured when aiming starts and then kept, so the weapon's own animations
// (recoil) still show.
#include <cstdint>
#include <vector>

#include "psp/gpu/vertex_decoder.h"

namespace psp::gpu {

class AimDownSights {
public:
    // A 3D draw (4x3 GE matrices, model-space vertices): classifies it and measures the weapon.
    void observe(const float view[12], const float world[12], const std::vector<Vertex>& verts);
    // Adjusts the uniform matrices (4x4 column-major) of that draw.
    // `viewportScaleX`: the draw's viewport half width (240 full screen).
    void adjust(const float view[12], float viewportScaleX, float view44[16], float proj44[16]) const;
    // A 2D (through mode) draw: true to drop it (the crosshair, while aiming).
    bool hide2D(const std::vector<Vertex>& verts) const;
    void endFrame(); // once per presented frame

private:
    static bool isWeaponCamera(const float view[12]);

    float t_ = 0;                  // 0 = hip, 1 = aiming
    double lastUs_ = 0;
    std::vector<float> gun_;        // weapon vertices this frame (view space, xyz)
    std::vector<float> front_;      // all weapon-camera vertices ahead of the hands (fallback)
    std::vector<float> notArms_;    // weapon-camera vertices not of the arms' meshes
    bool armsSeen_ = false;         // the arms were recognised this frame
    bool aligned_ = false;          // an alignment was measured for this aim
    size_t alignedVerts_ = 0;       // weapon vertices when it was measured
    int changed_ = 0;               // frames with a different weapon
    float sight_[3] = {0, 0, 0};    // sight point (view space)
    float yaw_ = 0, pitch_ = 0;     // rotation that puts the barrel along -z
    std::vector<size_t> topIdx_;    // the weapon's top-edge vertices (offsets into gun_)
    float refTop_[2] = {0, 0};      // their mean x, y when aiming started
    float sway_[2] = {0, 0};        // their smoothed movement since then
    float adsMat_[16];              // current weapon camera adjustment (column-major)
};

} // namespace psp::gpu
