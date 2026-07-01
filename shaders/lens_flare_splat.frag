#version 460 core

in vec3 vRadiance;
out vec4 FragColor;

void main()
{
    // Additive blending (GL_ONE, GL_ONE), set up by LensFlare::SplatToAccumulation,
    // does the accumulation into PathTracer's accumulation texture. Alpha=1
    // matches the same per-sample weight convention pathtrace.comp's
    // imageStore(prev + vec4(radiance, 1.0)) uses, so the shared
    // accumulation buffer's .a channel stays a correct running sample count
    // for the tonemap pass's divide.
    FragColor = vec4(vRadiance, 1.0);
}
