#include "GltfToMesh.hpp"

#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/AccessorUtility.h>
#include <CesiumGltf/VertexAttributeSemantics.h>
#include <CesiumGltfContent/GltfUtilities.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>

namespace reactnativecesium {

// Double-precision Bowring iterative geodetic height.
// Both subtracted terms are ~6.4e6 m, but in double (52-bit mantissa) the
// catastrophic cancellation leaves ~1 nm residual — effectively exact.
static double ellipsoidHeightDouble(const glm::dvec3& p) {
  constexpr double a   = 6378137.0;
  constexpr double b   = 6356752.31424518;
  constexpr double e2  = 1.0 - (b * b) / (a * a);
  constexpr double ep2 = (a * a - b * b) / (b * b);

  const double pxy = std::sqrt(p.x * p.x + p.y * p.y);
  if (pxy < 1e-10) return std::abs(p.z) - b;

  const double theta = std::atan2(p.z * a, pxy * b);
  const double st = std::sin(theta), ct = std::cos(theta);
  const double lat = std::atan2(p.z + ep2 * b * st * st * st,
                                pxy - e2  * a * ct * ct * ct);
  const double sinLat = std::sin(lat);
  const double N = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
  return pxy / std::cos(lat) - N;
}

std::vector<TilePrimitive>
GltfToMesh::convert(const CesiumGltf::Model& model,
                    const glm::dmat4& tileTransform) {
  std::vector<TilePrimitive> results;

  // Apply CESIUM_RTC and glTF Y-up adjustment.
  glm::dmat4 rootTransform = tileTransform;
  rootTransform = CesiumGltfContent::GltfUtilities::applyRtcCenter(model, rootTransform);
  rootTransform = CesiumGltfContent::GltfUtilities::applyGltfUpAxisTransform(model, rootTransform);

  model.forEachPrimitiveInScene(
      -1,
      [&results, &rootTransform](
          const CesiumGltf::Model& gltf,
          const CesiumGltf::Node& /*node*/,
          const CesiumGltf::Mesh& /*mesh*/,
          const CesiumGltf::MeshPrimitive& primitive,
          const glm::dmat4& nodeTransform) {

        switch (primitive.mode) {
        case CesiumGltf::MeshPrimitive::Mode::TRIANGLES:
        case CesiumGltf::MeshPrimitive::Mode::TRIANGLE_STRIP:
        case CesiumGltf::MeshPrimitive::Mode::TRIANGLE_FAN:
          break;
        default:
          return;
        }

        const CesiumGltf::PositionAccessorType posAccessor =
            CesiumGltf::getPositionAccessorView(gltf, primitive);
        if (posAccessor.status() != CesiumGltf::AccessorViewStatus::Valid ||
            posAccessor.size() <= 0)
          return;

        const glm::dmat4 worldTransform = rootTransform * nodeTransform;
        const int64_t    vertexCount    = posAccessor.size();

        // RTC centre = translation column of the world transform.
        // After applyRtcCenter this is the tile's ECEF centre embedded by
        // Cesium Native (the RTC_CENTER GLTF extension value).
        const glm::dvec3 rtcCenter(worldTransform[3]);

        TilePrimitive prim;
        prim.rtcCenter = rtcCenter;
        prim.localPositions.reserve(static_cast<size_t>(vertexCount));
        prim.altitudes.reserve(static_cast<size_t>(vertexCount));

        for (int64_t i = 0; i < vertexCount; ++i) {
          const auto&      p    = posAccessor[i];
          const glm::dvec3 gltfLocal(p.value[0], p.value[1], p.value[2]);
          // Absolute ECEF in double precision.
          const glm::dvec3 ecef(worldTransform * glm::dvec4(gltfLocal, 1.0));
          // Tile-local position (small float, retains full float32 precision
          // because it is relative to the nearby tile centre, not the origin).
          prim.localPositions.push_back(glm::vec3(ecef - rtcCenter));
          // Altitude computed in double — sub-millimetre accuracy.
          prim.altitudes.push_back(static_cast<float>(ellipsoidHeightDouble(ecef)));
        }

        // Overlay UV coordinates (_CESIUMOVERLAY_0).
        const auto uvIt = primitive.attributes.find("_CESIUMOVERLAY_0");
        if (uvIt != primitive.attributes.end() && uvIt->second >= 0) {
          CesiumGltf::AccessorView<CesiumGltf::AccessorTypes::VEC2<float>> uvView(gltf, uvIt->second);
          if (uvView.status() == CesiumGltf::AccessorViewStatus::Valid &&
              uvView.size() == vertexCount) {
            prim.uvs.reserve(static_cast<size_t>(vertexCount));
            for (int64_t i = 0; i < vertexCount; ++i) {
              prim.uvs.push_back(glm::vec2(uvView[i].value[0], uvView[i].value[1]));
            }
          }
        }

        // Expand index buffer → flat uint32 triangle list.
        const CesiumGltf::IndexAccessorType idxAccessor =
            CesiumGltf::getIndexAccessorView(gltf, primitive);

        const int64_t srcIdxCount =
            (primitive.indices >= 0)
                ? std::visit(CesiumGltf::CountFromAccessor{}, idxAccessor)
                : vertexCount;

        int64_t faceCount = 0;
        switch (primitive.mode) {
        case CesiumGltf::MeshPrimitive::Mode::TRIANGLES:
          faceCount = srcIdxCount / 3;
          break;
        case CesiumGltf::MeshPrimitive::Mode::TRIANGLE_STRIP:
        case CesiumGltf::MeshPrimitive::Mode::TRIANGLE_FAN:
          faceCount = std::max<int64_t>(0, srcIdxCount - 2);
          break;
        default:
          break;
        }

        prim.indices.reserve(static_cast<size_t>(faceCount * 3));
        for (int64_t fi = 0; fi < faceCount; ++fi) {
          const std::array<int64_t, 3> face = std::visit(
              CesiumGltf::IndicesForFaceFromAccessor{
                  fi, vertexCount, primitive.mode},
              idxAccessor);
          if (face[0] < 0 || face[1] < 0 || face[2] < 0 ||
              face[0] >= vertexCount || face[1] >= vertexCount ||
              face[2] >= vertexCount)
            continue;
          prim.indices.push_back(static_cast<uint32_t>(face[0]));
          prim.indices.push_back(static_cast<uint32_t>(face[1]));
          prim.indices.push_back(static_cast<uint32_t>(face[2]));
        }

        if (!prim.indices.empty()) {
          results.push_back(std::move(prim));
        }
      });

  return results;
}

} // namespace reactnativecesium
