#version 460 core

// Displays the path tracer's HDR accumulation image: average by sample
// count, apply exposure, ACES filmic tonemap, gamma-correct.

layout(binding = 0) uniform sampler2D uAccum;
uniform float uExposure;
uniform float uSampleCount;

out vec4 FragColor;

vec3 ACESFilm(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    ivec2 size = textureSize(uAccum, 0);
    // The compute shader writes pixel row 0 = top of the rendered scene
    // (see shaders/pathtrace.comp), while gl_FragCoord.y = 0 is the bottom
    // of the screen under OpenGL's default window-space convention - flip
    // here so the displayed image is right-side up.
    ivec2 texel = ivec2(int(gl_FragCoord.x), size.y - 1 - int(gl_FragCoord.y));
    vec4 accum = texelFetch(uAccum, texel, 0);

    vec3 color = accum.rgb / max(uSampleCount, 1.0);
    color *= uExposure;
    color = ACESFilm(color);
    color = pow(color, vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
