#version 460 core
#include "common.glsl"

// Attribute-less draw (like shaders/tonemap.vert): reads splat records
// straight from SplatBufferSSBO (binding 8) via gl_VertexID, no VBO needed.

uniform ivec2 uImageSize;

out vec3 vRadiance;

void main()
{
    Splat s = splats[gl_VertexID];
    bool valid = s.radianceBValid.y > 0.5;
    vRadiance = valid ? vec3(s.pixelRadianceRG.zw, s.radianceBValid.x) : vec3(0.0);

    vec2 pixelF = s.pixelRadianceRG.xy;
    // Same pixel-coordinate convention pathtrace.comp's imageStore calls use
    // (pixel.y=0 = top of the rendered scene): texel-row addressing is
    // identical between compute image-store and this FBO-attached raster
    // write, so no extra Y-flip is needed here - deliberately different
    // from tonemap.frag's flip, which exists only to reconcile with the
    // default window framebuffer's bottom-left origin when displaying to
    // the screen (not applicable to this off-screen accumulation write).
    // See docs/ARCHITECTURE.md.
    vec2 ndc = (pixelF / vec2(uImageSize)) * 2.0 - 1.0;

    // Push invalid/unused splat slots off-clip-space so they are rejected
    // before rasterization at negligible cost (LensFlare draws a fixed
    // uMaxSplats-sized batch every frame rather than reading the atomic
    // splat counter back to the CPU, which would stall the pipeline).
    gl_Position = valid ? vec4(ndc, 0.0, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 1.0;
}
