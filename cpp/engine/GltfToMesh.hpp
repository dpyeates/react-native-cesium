#pragma once

#include "../renderer/RenderTypes.hpp"

#include <CesiumGltf/Model.h>
#include <glm/glm.hpp>

#include <vector>

namespace reactnativecesium {

class GltfToMesh {
public:
  // Convert a glTF model + tile world transform into a list of primitives.
  // All vertex positions are transformed to absolute ECEF in double precision.
  static std::vector<TilePrimitive>
  convert(const CesiumGltf::Model& model, const glm::dmat4& tileTransform);
};

} // namespace reactnativecesium
