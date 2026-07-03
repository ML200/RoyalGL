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
        glm::vec4 baseColor; // .rgb albedo (Diffuse) / tint (Glass), .a unused
        glm::vec4 emissive;  // .rgb emissive radiance, .a unused
        glm::vec4 params;    // x=metallic, y=roughness, z=ior, w=type (0=diffuse, 1=glass)
    };

    // Uniform buffer uploaded once per PathTracer::Render() call (binding 0).
    struct GPUFrameUBO
    {
        glm::vec4 camPos;      // xyz, w unused
        glm::vec4 camForward;  // xyz, w unused
        glm::vec4 camRight;    // xyz, w unused
        glm::vec4 camUp;       // xyz, w unused
        glm::vec4 cameraParams;// x=tanHalfFovY, y=aspect, z/w unused
        glm::vec4 background;  // .rgb sky color, .a intensity multiplier
        glm::uvec4 frameInfo;  // x=width, y=height, z=sampleIndex, w=maxBounces
        glm::vec4 renderParams;// x=exposure, y=total light power, z/w reserved
        glm::uvec4 lightInfo;  // x=light triangle count, y=NEE enabled (0/1), z=BDPT light path count,
                               // w=lens flare samples per t=1 connection
        glm::vec4 lensParams;  // x=sensor half width mm, y=sensor half height mm,
                               // z=front vertex z mm, w=pupil plane z mm (lens camera mode)
        glm::vec4 lensParams2; // x=camera mode (0=pinhole, 1=lens), y=flare enabled (0/1),
                               // z=rear element semi-diameter mm, w=front element semi-diameter mm
        glm::vec4 lensParams3; // x=diffraction enabled (0/1), y=diffraction intensity,
                               // z=diffraction edge width mm, w unused

        // Previous frame's pinhole camera, for ReSTIR temporal reprojection
        // (motion vectors, temporal shift domains). Equal to the current
        // camera on the very first ReSTIR frame.
        glm::vec4 prevCamPos;      // xyz, w unused
        glm::vec4 prevCamForward;  // xyz, w unused
        glm::vec4 prevCamRight;    // xyz, w unused
        glm::vec4 prevCamUp;       // xyz, w unused
        glm::vec4 prevCameraParams;// x=tanHalfFovY, y=aspect, z/w unused
        glm::uvec4 restirParams;   // x=debug view index, y=flags (bit0 active, bit1 temporal,
                                   // bit2 spatial, bit3 accumulate, bit4 light tracing),
                                   // z=frame counter (never reset), w=ping-pong parity

        // Instance transforms for OBJECT-SPACE reservoir / G-buffer surface
        // storage: positions that persist across frames (G-buffer halves,
        // reservoir reconnection vertices, cached light-subpath ends, NEE
        // light points) are stored in instance object space and converted
        // with the CURRENT matrices on load - so stored surfaces track
        // moving objects automatically instead of ghosting at their old
        // placement. instInfo.x = instance count (0 disables the machinery,
        // also the >kMaxRestirInstances fallback); instTable = first
        // triangle index per instance (hit triangle -> instance lookup).
        static constexpr int kMaxRestirInstances = 16;
        glm::uvec4 instInfo;
        glm::uvec4 instTable[4];
        glm::mat4 instToWorld[kMaxRestirInstances];
        glm::mat4 instToObject[kMaxRestirInstances];
    };

    // One lens surface in walk order (rear -> front), uploaded to SSBO
    // binding 13 by LensSystem::Derive(). Media carry Sellmeier (mode 1:
    // B1..3 / C1..3) or Cauchy (mode 0: coef.x=B, coef2.w=A) dispersion,
    // evaluated per wavelength in shaders/lens_common.glsl.
    struct GPULensSurface
    {
        glm::vec4 geo;      // x=vertex z mm, y=signed radius mm (paper convention), z=semi-diameter mm, w=isAperture
        glm::vec4 mediumA;  // image-side medium coefficients, w=mode
        glm::vec4 mediumA2;
        glm::vec4 mediumB;  // object-side medium coefficients, w=mode
        glm::vec4 mediumB2;
        glm::vec4 aperture; // x=blade count (0=circular), y=blade rotation rad, z=stop radius mm (aperture row)
    };

    // One 4-wide light tree node, uploaded to SSBO binding 5 by
    // LightTree::Build(). Interior nodes reference a contiguous block of
    // children; leaves reference a contiguous run of GPULightTriangle
    // entries. Interior nodes also carry their subtree's triangle range so
    // the pdf evaluation can re-descend deterministically (see
    // shaders/light_tree.glsl).
    struct GPULightTreeNode
    {
        glm::vec4 bminPower; // xyz=aabb min, w=total emitted power of the subtree
        glm::vec4 bmaxCosO;  // xyz=aabb max, w=cos(theta_o) of the merged normal cone
        glm::vec4 axisCosE;  // xyz=normal cone axis, w=cos(theta_e) emission falloff angle
        glm::uvec4 meta;     // x=firstChild, y=childCount (0=leaf), z=triFirst, w=triCount
    };

    // One emissive triangle, uploaded to SSBO binding 6 by LightTree::Build()
    // in leaf-list order (so a leaf's triFirst/triCount index this array
    // directly, and an interior node's range test is a simple interval check).
    struct GPULightTriangle
    {
        glm::vec4 p0, p1, p2;       // world-space position; p0.w = source scene-triangle
                                    // index (exact float), p1.w/p2.w unused
        glm::vec4 normalArea;       // xyz=geometric normal, w=triangle area
        glm::vec4 emissionWeight;   // rgb=emitted radiance, w=selection weight (area * luminance)
    };
}
