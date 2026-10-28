#pragma once
// Minimap built from the level geometry the game draws (a feature of the PC port).
//
// The recorder hands over the 3D draws of each frame; those drawn with the frame's main camera
// (the player's view) are merged, on a worker thread, into a top-down grid of the level in the
// game's own coordinates: floors (triangles facing up, below the eye) and walls (vertical
// triangles at body height). Static geometry is processed once. The front end asks for a
// rotated, player-centred picture of the grid.
#include <cstdint>
#include <memory>
#include <vector>

#include "psp/gpu/vertex_decoder.h"

namespace psp::gpu::minimap {

// Recorder side (guest threads) ----------------------------------------------------------
// One 3D draw: model-space vertices, primitive (PRIM_*), the 4x3 world and view matrices.
// `key` identifies static geometry (0 = always process).
void addDraw(uint64_t key, std::shared_ptr<const std::vector<Vertex>> verts, int prim, const float world[12],
             const float view[12], bool skinned);
void endFrame(); // the frame was presented

// Front end ------------------------------------------------------------------------------
// Paints the map around the camera into a size x size RGBA image (row 0 = top). `range` is the
// world distance from the centre to the edge. Returns false when there is nothing to show.
bool render(std::vector<uint32_t>& rgba, int size, float range);
void setEnabled(bool on);

} // namespace psp::gpu::minimap
