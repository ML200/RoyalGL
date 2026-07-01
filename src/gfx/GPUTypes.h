#pragma once

#include <glm/glm.hpp>

// GPU-side (std140 / std430) mirrors of CPU data, uploaded verbatim via
// Buffer::Upload(). Field order and types here MUST match the corresponding
// struct declarations in shaders/common.glsl exactly.
//
// Every member is a 16-byte-aligned vec4/uvec4 specifically so std140/std430
// layout matches the C++ struct layout with zero implicit padding surprises;
// if you add a field, add it as (part of) a vec4 in both this file and
// common.glsl, at the same position.
namespace RoyalGL
{
    // One flattened, world-space triangle. Indexed directly by the original
    // triangle index (see BVHBuilder - the BVH's triangle-index permutation
    // buffer stores indices into this array, this array itself is never
    // reordered).
    struct GPUTriangle
    {
        glm::vec4 p0, p1, p2;   // world-space position, .w unused
        glm::vec4 n0, n1, n2;   // shading normal, .w unused
        glm::vec4 uv0_uv1;      // uv0.xy, uv1.xy
        glm::vec4 uv2_material; // uv2.xy, materialIndex (stored as an exact float), unused
    };

    struct GPUMaterial
    {
        glm::vec4 baseColor; // .rgb albedo, .a unused
        glm::vec4 emissive;  // .rgb emissive radiance, .a unused
        glm::vec4 params;    // x=metallic, y=roughness, z/w reserved
    };

    // Uniform buffer uploaded once per PathTracer::Render() call (binding 0).
    struct GPUFrameUBO
    {
        glm::vec4 camPos;      // xyz, w unused
        glm::vec4 camForward;  // xyz, w unused
        glm::vec4 camRight;    // xyz, w unused
        glm::vec4 camUp;       // xyz, w unused
        glm::vec4 lensParams;  // x=tanHalfFovY, y=aspect (pinhole mode);
                                // x=tanHalfFovY, y=aspect, z=sensorWidthMm, w=lensModeFlag (0/1) (lens mode)
        glm::vec4 background;  // .rgb sky color, .a intensity multiplier
        glm::uvec4 frameInfo;  // x=width, y=height, z=sampleIndex, w=maxBounces
        glm::vec4 renderParams;// x=exposure, y/z/w reserved
    };

    // One lens surface, uploaded to SSBO binding 5 by LensSystem::Upload().
    // See docs/ARCHITECTURE.md for the full lens-camera data flow.
    struct GPULensSurface
    {
        glm::vec4 geometry;     // x=signed radius mm, y=thickness-to-next mm, z=semiDiameter mm, w=isAperture (0/1)
        glm::vec4 iorRGB_z;     // xyz=IOR(R,G,B) of the medium AFTER this surface (air=1,1,1); w=cumulative
                                 //   axial position (mm) from the sensor, precomputed once per lens change
        glm::vec4 coatingRGB;   // xyz=per-channel Fresnel-reflectance multiplier from AR coating (1,1,1=none), w unused
        glm::vec4 apertureData; // x=bladeCount (0=circular), y=bladeRotationRad, z=apertureRadiusMm, w unused
                                 //   (only meaningful on the isAperture row)
    };

    // One emissive triangle, uploaded to SSBO binding 6 by LightList::Build(),
    // used only by the flare/ghost light-tracing pass.
    struct GPULightTriangle
    {
        glm::vec4 p0, p1, p2;  // world-space position, .w unused
        glm::vec4 normal_area; // xyz geometric normal, w = triangle area
        glm::vec4 emissive;    // .rgb radiance, .a unused
    };

    // One flare/ghost splat record, written by shaders/lens_flare.comp
    // (SSBO binding 8) and read by shaders/lens_flare_splat.vert.
    struct GPUSplat
    {
        glm::vec4 pixelRadianceRG; // x=pixelX, y=pixelY, z=radiance.r, w=radiance.g
        glm::vec4 radianceBValid;  // x=radiance.b, y=validFlag (0/1), z/w unused
    };
}
