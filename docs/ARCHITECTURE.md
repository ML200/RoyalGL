# RoyalGL Architecture

RoyalGL is a real-time-progressive, offline-quality path tracer: an OpenGL
compute shader accumulates path-traced samples into an HDR image every frame
while you look at it, exactly like a renderer viewport in a DCC tool. This
document describes the v1 base architecture: what each module owns, how data
flows from a `.glb` file to pixels on screen, and where the deliberate
simplifications live so they're easy to find and replace later.

## Module map

```
src/
  core/        Window (GLFW + GL context + GLEW), Application (owns everything,
               runs the main loop), Types.h, Log.h
  gfx/         Thin, dumb GL wrappers: Shader, Buffer (SSBO/UBO), Texture,
               FullscreenPass (tonemap blit). GPUTypes.h is the shared
               CPU/GPU data contract (see below).
  scene/       Camera, Material, Mesh (Vertex/Triangle), Scene (flattened
               world-space triangle soup + materials + camera), GLTFLoader
               (cgltf-based .gltf/.glb -> Scene).
  bvh/         BVHBuilder: builds a tinybvh::BVH over Scene::triangles and
               uploads everything the compute shader's leaf code needs
               (nodes, triangle index permutation, triangle data, materials).
  optics/      Glass (Cauchy-from-Abbe dispersion + built-in glass catalog),
               LensSurface, LensSystem (prescription + aperture/focus/sensor
               state, GPU upload), LensPrescription (.lens file parser +
               built-in Tessar preset). See "Physical lens camera" below.
  pathtracer/  RenderSettings (UI-tunable knobs), PathTracer (compute shader
               dispatch + progressive accumulation image + per-frame UBO),
               LightList (emissive-triangle list for the flare pass),
               LensFlare (forward light-tracing flare/ghost/diffraction pass
               + additive-blend splat draw).
  denoise/     Denoiser: optional Intel Open Image Denoise wrapper (pimpl'd
               so the rest of the app never needs to #ifdef around it).
  io/          ImageExport: HDR accumulation buffer -> tonemapped PNG.
  ui/          UILayer: Dear ImGui panels (stats, camera, camera model/lens
               editor, render settings, flare/diffraction, denoise, export).
shaders/
  common.glsl       Struct declarations shared by all shaders (mirrors
                    gfx/GPUTypes.h field-for-field) + shared RNG/Ray/Hit/BVH
                    traversal, used by every compute shader.
  lens_common.glsl  Shared lens-surface geometry (sphere/plane intersection,
                    exact Snell's law, aperture N-gon test, Fresnel
                    reflectance, world<->lens-space transforms).
  pathtrace.comp    Megakernel path tracer: ray gen (pinhole formula, or
                    real lens-surface tracing in lens mode), BVH traversal,
                    shading, accumulate.
  lens_flare.comp   Forward light-tracing pass: flare/ghosts (stochastic
                    Fresnel reflect/refract) + aperture diffraction.
  lens_flare_splat.vert/frag  Additive-blend point-splat draw, reads
                    lens_flare.comp's output straight from an SSBO.
  tonemap.vert/frag  Full-screen blit: exposure + ACES tonemap + gamma.
```

## Data flow, one frame

1. `Application` polls GLFW, feeds mouse/keyboard into `Camera` (orbit/dolly/
   pan) unless ImGui wants the input.
2. If the camera, scene, `RenderSettings`, `CameraSettings`, or `LensSystem`
   changed since the last frame, `LensSystem::Upload()` re-derives its GPU
   buffer (only when the lens itself changed) and `PathTracer::Reset()`
   clears the accumulation image and the sample counter.
3. `PathTracer::Render()`:
   - uploads a `GPUFrameUBO` (camera basis vectors, FOV or sensor size,
     background, exposure, sample index, max bounces, lens-mode flag) to
     UBO binding 0,
   - binds the BVH node / triangle-index / triangle / material SSBOs
     (bindings 1-4, owned by `BVHBuilder`), the accumulation image (image
     unit 0), and, in lens mode, the lens-surface SSBO (binding 5, owned by
     `LensSystem`),
   - dispatches `pathtrace.comp` with one thread per pixel. Each thread
     generates a primary ray either via the pinhole formula, or (lens mode)
     by tracing up to 3 rays (R/G/B representative wavelengths) back-to-
     front through the physical lens surfaces (`TraceThroughLens`, exact
     Snell's law - see "Physical lens camera" below), then traces **one
     full path** per successful ray (up to `maxBounces` diffuse bounces
     with Russian roulette) and adds the result to the accumulation image
     (`imageLoad` + `imageStore`; safe without atomics because each pixel
     is written by exactly one invocation per channel per dispatch).
4. If lens mode and flare are both enabled and the scene has emissive
   triangles, `LensFlare` runs a second, independent pass: a compute
   dispatch (`lens_flare.comp`) light-traces from emissive triangles
   through the lens front-to-back with stochastic Fresnel reflect/refract
   (ghosts) and optional Keller-cone aperture diffraction, emitting splat
   records; a tiny vert/frag pair then additively blends those splats
   directly into the same accumulation texture via an FBO attachment
   (`GL_ONE, GL_ONE` blending - the portable way to accumulate many
   invocations writing the same pixel, which plain compute image-store
   cannot do safely). See "Lens flare / ghosts" below for the required
   `glMemoryBarrier` sequence around this step.
5. `FullscreenPass` draws a full-screen triangle sampling the accumulation
   image, dividing by the sample count, applying exposure + ACES + gamma,
   into the default framebuffer.
6. `UILayer` draws ImGui panels on top; if "Denoise" or "Export PNG" was
   clicked, `Application` reads the accumulation image back
   (`Texture::ReadPixelsFloat`) and calls `Denoiser`/`ImageExport`.
7. `Window::SwapBuffers()`.

## GPU binding table

| Binding | Type | Owner | Contents |
|---|---|---|---|
| UBO 0 | Uniform | `PathTracer` | `GPUFrameUBO` |
| Image 0 | image2D | `PathTracer` | accumulation image (also used as an FBO color attachment by `LensFlare`) |
| SSBO 1 | Storage | `BVHBuilder` | BVH nodes |
| SSBO 2 | Storage | `BVHBuilder` | triangle index permutation |
| SSBO 3 | Storage | `BVHBuilder` | triangles |
| SSBO 4 | Storage | `BVHBuilder` | materials |
| SSBO 5 | Storage | `LensSystem` | lens surfaces (lens mode only) |
| SSBO 6 | Storage | `LightList` | emissive triangles (flare pass only) |
| SSBO 7 | Storage | `LightList` | power-proportional sampling CDF |
| SSBO 8 | Storage | `LensFlare` | splat output buffer |
| SSBO 9 | Storage | `LensFlare` | atomic splat counter |

## The CPU/GPU data contract

`src/gfx/GPUTypes.h` and `shaders/common.glsl` declare the *same* structs
twice, once in C++ and once in GLSL, deliberately built entirely out of
16-byte-aligned `vec4`/`uvec4` fields. That sidesteps every `std140`/`std430`
padding footgun: what you `memcpy` from a `std::vector<GPUTriangle>` on the
CPU is byte-for-byte what the shader's `std430` layout expects. If you add a
field, add it as a `vec4` (or pack 4 floats into one) on both sides, and add
it in the same position in both files.

The one exception is the BVH node buffer: `tinybvh::BVH::BVHNode` (the
"Wald" 32-byte layout: `vec3 aabbMin + uint leftFirst`, `vec3 aabbMax + uint
triCount`) is uploaded to the GPU **unmodified** - its C++ byte layout
already matches the `std430` layout of the equivalent GLSL struct, which is
exactly why tinybvh calls this layout GPU-friendly. See `BVHBuilder.cpp`.

## Why a triangle soup instead of BLAS/TLAS

v1 flattens the entire glTF scene graph into one `std::vector<Triangle>` in
world space at load time (`GLTFLoader`) and builds a single `tinybvh::BVH`
over it. This is the simplest possible thing that lets you drop in a `.glb`
and path trace it correctly, and it's enough for one static scene of
moderate complexity.

It does **not** support: moving/instancing individual meshes without a full
rebuild, or scenes so large that redundant per-instance triangle expansion
matters. When that's needed, the natural extension is: build one
`tinybvh::BVH` per glTF mesh (BLAS), keep per-node world transforms, and
build a `tinybvh::BVH` of instances (TLAS) using tinybvh's native
`BLASInstance`/TLAS support (`BVH::Build(BLASInstance*, ...)`), then extend
the traversal shader with a second, outer stack. `BVHBuilder` is the only
class that would need to change.

## Shading model

v1 ships a single Lambertian diffuse BRDF with cosine-weighted hemisphere
sampling plus emissive materials as area lights (no explicit light list -
any triangle whose material has non-zero emissive contributes radiance when
a path hits it, and is also directly visible). `GPUMaterial::params` already
carries `metallic`/`roughness` so the extension point for a full
metallic-roughness GGX BRDF is `shaders/pathtrace.comp`'s `ShadeHit()`
function plus next-event-estimation (explicit light sampling) for faster
convergence - both are out of scope for the base setup.

## Material editor

`UILayer::Draw` opens a second, separate top-level ImGui window ("Material
Editor", distinct from the main "RoyalGL" panel) listing every entry in
`Scene::materials` with live `baseColor`/`emissive` (HDR-capable
`ColorEdit3`) /`metallic`/`roughness` widgets. It only edits existing
entries in place - no add/remove/rename, matching `RenderSettings`-style
"simple" scope.

Materials are consumed by **two independent GPU-side paths**, both of
which must be kept in sync on edit: `BVHBuilder`'s `MaterialsSSBO`
(binding 4, read by the main path tracer and the lens ray tracer) and
`LightList`'s emissive-triangle list + power-proportional sampling CDF
(bindings 6/7, read only by the flare/ghost pass - see "Lens flare /
ghosts" below). `Application::Run()`'s dirty-check gained a
`materialsDirty = (m_scene->materials != m_lastMaterials)` snapshot
(mirroring the existing camera/settings/lens snapshots), and on a hit
calls both `BVHBuilder::UpdateMaterials()` (re-derives+re-uploads just
the materials SSBO, skipping the full BVH rebuild) *and*
`LightList::Build()` again - forgetting the latter would leave the flare
pass sampling with a stale CDF any time an edit changes which triangles
are emissive (or by how much), since `LightList::Build()` was originally
only ever called once, at startup.

## Physical lens camera (Steinert et al. 2011)

`CameraSettings::mode` switches `pathtrace.comp`'s ray generation between
the original pinhole formula and real ray tracing through a physical lens
prescription, implementing "General Spectral Camera Lens Simulation"
(Steinert, Dammertz, Hanika & Lensch, CGF 2011) minus its continuous-
spectrum Monte Carlo integration, which is approximated here with 3
representative wavelengths (R/G/B, `optics/Glass.h`'s
`LensWavelengths::kNm` / `shaders/lens_common.glsl`'s `kLensWavelengthsNm` -
keep these in sync).

- **Prescription data model** (`optics/LensSurface.h`, `optics/LensSystem.h`):
  a `LensSurface` is one row of the paper's table (signed radius, thickness
  to the next surface, semi-diameter, glass name, aperture-stop flag),
  ordered front element (index 0) -> sensor-side (index N-1). `LensSystem`
  layers aperture blade count/rotation/f-stop, focus distance, sensor size
  and an overall scale factor on top, and owns the `GPULensSurface[]` GPU
  upload (SSBO binding 5) - mirrors `BVHBuilder`'s build/bind lifecycle.
  `LensSurface::radiusMm` follows the standard optical-prescription sign
  convention (same as the paper's table / any external lens patent: z
  increases object->image, positive radius = center of curvature toward
  the image side), so prescriptions can be pasted in verbatim. The GPU ray
  tracer's internal z axis runs the opposite way (sensor at z=0, front
  element at the largest z, matching the sensor->front traversal direction
  of the primary ray), which inverts the meaning of a sphere's radius sign
  - `LensSystem::BuildGPUSurfaces()` negates it exactly once, at the
  CPU->GPU boundary. This was found via an independent Python paraxial
  ray-trace check: without the negation, the 8-surface Tessar's per-surface
  bends nearly cancel end-to-end (~0.0003 rad net, vs. ~0.005-0.01 rad from
  any single surface alone) and a sensor-side ray bundle converges *behind*
  the lens (a virtual image); with it, parallel rays converge ~29.6m out
  (correctly near-infinity) and a 6500mm focus target converges at
  ~6487mm.
- **Glass dispersion** (`optics/Glass.h`): two-term Cauchy dispersion
  (`n(lambda) = A + B/lambda^2`) derived from each glass's catalog `(nd,
  Vd)` pair, not a 6-coefficient Sellmeier fit - this is the same
  simplification Johannes Hanika (a co-author of the paper) uses in his own
  lens-tracing renderer, and the `(nd, Vd)` values for the built-in Tessar's
  three glasses (LAK9, LLF7, SF7) are transcribed from his own published
  lens data file, not fabricated. `LensSystem::Upload()` bakes each
  surface's IOR at the 3 representative wavelengths once per lens change;
  the shader never touches raw dispersion coefficients.
- **Prescription file format & presets** (`optics/LensPrescription.h`): a
  small line-based `.lens` text format (header key/values + a whitespace
  surface table, directly mirroring the paper's own Figure 4 layout) - see
  `assets/lenses/tessar.lens`. Only the Tessar ships as a preset, verified
  against the paper's own numbers; the format itself is fully general, so
  pasting in any other prescription is how "fully customizable" is met
  rather than shipping many presets.
- **Ray tracing** (`shaders/lens_common.glsl`, `shaders/pathtrace.comp`):
  exact vector-form Snell's law (no paraxial approximation - this alone is
  what makes spherical/coma/astigmatism/field-curvature/distortion and
  chromatic aberration emerge from geometry, with zero special-case code
  per aberration), sphere/plane surface intersection with a uniform root-
  selection rule for either curvature sign or traversal direction, and a
  regular N-gon aperture-blade test (for bokeh shape) instead of a plain
  circle. `TraceThroughLens` walks `lensSurfaces` **back-to-front** (sensor
  -> front element, since the primary ray travels sensor -> scene); the
  flare pass's front-to-back walk is a separate function using the same
  shared primitives. All lens math stays in local millimeter space (sensor
  at local z=0, optical axis = local +z); the mm -> m conversion to world
  space happens exactly once, on the final ray only, to keep sphere-
  intersection subtractions well-conditioned (the paper's own stated reason
  for this scale separation).
- **Dispersion cost**: lens mode traces up to 3 full scene paths per pixel-
  sample (one per wavelength channel, since R/G/B rays refract to
  physically different directions), with each channel's `IntersectScene`
  call skipped entirely when its lens trace is vignetted/blocked/TIR - a
  real perf win at aggressive apertures, not just tidiness.
- **Aperture sampling**: v1 samples uniformly on the rearmost element's own
  clear-aperture disk (a correctness-preserving superset of every
  reachable point, at the cost of some wasted/rejected samples - the
  paper's own documented trade-off, as low as ~10% pass rate for extreme
  fisheye/small-aperture cases in their measurements). The paper's "pixel
  pupil" importance-sampling optimization (a precomputed per-pixel aperture
  disk) is a deliberately deferred follow-up, not required for correctness.
- **Sensor image inversion**: a real lens forms an inverted (180°-rotated)
  image - basic optics for any converging system - so `pathtrace.comp`'s
  pixel->`sensorPosMM` mapping negates both axes relative to the pinhole
  formula's direct (un-inverted) projection, pre-inverting to compensate;
  the lens re-inverts it a second time on the way through, landing
  right-side-up. `lens_flare.comp`'s inverse mapping (sensor hit ->
  pixel, used to emit ghost/flare splats) negates the same way for
  consistency. Found by rendering a known off-center scene feature (a box
  at negative world X, below eye height) through the uncorrected mapping
  and observing it land point-reflected (positive-X, above eye height) in
  the output.

## Lens flare / ghosts + aperture diffraction

`pathtracer/LightList` + `pathtracer/LensFlare` implement the paper's
Sec 4.3 (lens flare via internal Fresnel reflections) and Sec 4.4 (aperture
diffraction via the geometrical theory of diffraction / Keller cone), as a
second pass layered on top of the primary path-traced image - architecturally
independent and summed, exactly as the paper's own reference renderer does it
(see its Figure 18: three separately-rendered layers, summed).

- **Light list**: `LightList::Build()` scans `Scene::triangles` for non-zero-
  emissive materials once (alongside `BVHBuilder::Build()`) and uploads a
  power-proportional sampling CDF (SSBO 6/7) - uniform selection would waste
  nearly all samples on large dim emitters while starving the rare bright
  small lights that actually produce visible ghosts/streaks.
- **Connecting to the lens**: per the paper's "deterministically connecting
  [light samples] to the lens", `lens_flare.comp` doesn't hemisphere-sample
  a direction from the light and hope it hits the lens (astronomically
  unlikely) - it samples a point on the light and a point on the front
  element's clear-aperture disk and connects them directly, weighted by the
  standard bidirectional area-to-area vertex-connection formula
  (`Le * G(P,Q) / (pdfArea(P) * pdfArea(Q))`, `G = cosP*cosQ/dist^2`),
  after a scene-occlusion shadow-ray check.
- **Stochastic Fresnel walk**: the connected ray then walks the lens
  **front-to-back** (opposite of the primary tracer). At every refractive
  surface it computes unpolarized dielectric Fresnel reflectance per RGB
  channel (times any AR-coating multiplier), collapses that to one scalar
  branch probability via luminance, and stochastically reflects or
  refracts with the correct `1/p` Monte Carlo throughput correction either
  way (unbiased). A reflection reverses the walk's traversal direction -
  this is the entire mechanism that produces the classic 2-internal-
  reflection "aperture ghost" pattern; reflections are capped at 2 for v1
  (the paper's own stated dominant mechanism). A path that reaches the
  physical sensor rectangle (past the last surface, projected onto the
  local z=0 plane) emits a splat record.
- **Splatting, not image-store**: many light-traced paths can land on the
  same pixel within one dispatch, which plain `imageLoad`/`imageStore`
  cannot safely accumulate (no portable atomic-float-image op on every
  target GPU). Splats are written to a fixed-capacity SSBO via `atomicAdd`
  on a plain uint counter (core GL, no extension), then drawn as 1-pixel
  `GL_POINTS` with `(GL_ONE, GL_ONE)` additive blending into the
  accumulation texture via `gfx/Framebuffer` (an FBO attachment) - GL's
  blend hardware guarantees correct per-pixel accumulation ordering.
  `LensFlare` always draws its full fixed capacity (never reads the atomic
  counter back to the CPU, which would stall the pipeline); unused/invalid
  splat slots are pushed off-clip-space in the vertex shader so the extra
  draws cost effectively nothing.
- **Barriers**: `Application::Run()` issues `GL_SHADER_STORAGE_BARRIER_BIT`
  between the trace dispatch and the splat draw (compute SSBO write ->
  vertex shader SSBO read), and `GL_FRAMEBUFFER_BARRIER_BIT` (plus
  `GL_SHADER_IMAGE_ACCESS_BARRIER_BIT` afterward) around the transition
  between `PathTracer`'s compute image-store and `LensFlare`'s FBO-attached
  raster write to the *same* accumulation texture - mixing image-load-store
  and framebuffer-attachment access to one texture within a frame is a
  less common GL pattern worth re-verifying (e.g. with RenderDoc) if flare
  rendering ever looks unstable across frames.
- **Radiometric normalization**: each dispatch fires a fixed `M` light
  samples (`RenderSettings::flareSamplesPerFrame`) and divides every
  splat's throughput by `M`; since that per-frame quantity is itself an
  unbiased estimator of the flare radiance at each pixel, and
  `PathTracer`'s existing tonemap pass already divides the whole shared
  accumulation buffer by the frame count, no further scaling is needed -
  the average of `S` unbiased per-frame estimates is itself unbiased.
  **Bug found and fixed**: the connection formula (`Le * G(P,Q) /
  (pdfArea(P) * pdfArea(Q))`) is a Monte-Carlo estimate of *flux* (Watts)
  transported through the light-to-lens-aperture connection - it was being
  splatted directly as if it were a per-pixel radiance/irradiance value,
  with no conversion between the two. The missing factor is the physical
  area of one sensor pixel (`shaders/lens_flare.comp` now divides by
  `pixelWidthM * pixelHeightM`, derived from `sensorWidthMm`/image
  resolution): a pixel on a real sensor is a tiny fraction of a mm²,
  so omitting this made every splat roughly seven orders of magnitude too
  dim to see against the noise floor - not "flares are just faint", they
  were architecturally invisible. Confirmed by reading back the splat
  counter (tens of thousands of valid splats were being generated and
  correctly rasterized every frame - the trace/splat/blend pipeline itself
  was never the problem) and by temporarily multiplying throughput by a
  large constant, which immediately revealed correctly-shaped ghost
  patterns (mirrored copies of the light layout) and a glow halo. Even
  with the pixel-area fix, the *absolute* scale still isn't universally
  correct (scene-specific factors - light brightness/size, distance,
  aperture size - shift it further), so `RenderSettings::flareIntensity`
  (default 60) exists as the same kind of artist calibration knob
  `diffractionIntensity` already used, tuned empirically against the
  built-in 25-light fallback scene.
- **Stale splat buffer**: `LensFlare::ResetSplatBuffer()` originally only
  zeroed the atomic counter, not the splat records themselves. Since the
  compute kernel only ever writes indices `[0, count)` via `atomicAdd`,
  any slot beyond a given frame's `count` still held whatever a *previous*
  frame's write left there - including a `valid=true` flag - and the splat
  count fluctuates frame to frame (Monte Carlo variance in how many paths
  survive vignetting/TIR/blade-blocking). A frame with fewer accepted
  paths than the previous one would silently redraw that previous frame's
  leftover splats on top of its own, over-accumulating. Fixed by
  `glClearNamedBufferData`-zeroing the whole splat buffer every frame in
  `ResetSplatBuffer()` (a single GPU-side driver call, no CPU stall,
  keeping the "never read the atomic counter back" design intact).
- **Aperture diffraction**: a third stochastic branch specifically at the
  aperture-stop surface, active only when the light-traced ray's hit point
  is within `RenderSettings::diffractionEdgeEpsilonMM` of a blade edge
  (found by an analytic nearest-edge test against the same
  `(bladeCount, rotation, radius)` triple the primary tracer's aperture
  test reads - no separate blade-geometry buffer). On diffracting, it
  samples a direction on the Keller cone (axis = local edge tangent,
  half-angle = angle of incidence) and weights by the paper's Eq. 1
  wedge-diffraction coefficient, simplified to `alpha=0` (infinitely thin
  aperture, per the paper's own stated assumption). **Known uncertainty**:
  the coefficient's `cos(pi/4)` prefactor was cross-checked against two
  independent PDF text extractions of the paper (both agree), but its
  absolute radiometric scale is not independently verified - hence
  `RenderSettings::diffractionIntensity` as an artist calibration knob,
  standard practice for any physically-approximate glare effect. The naive
  (non-transition-corrected) Keller coefficient also has a known
  singularity near geometric shadow/reflection boundaries, band-aided here
  with an epsilon clamp rather than a full UTD transition-function fix.

## Extension points (deliberately not built yet)

- **Textures**: `Material`/`GPUMaterial` carry factors only, no texture
  indices/samplers yet. `GLTFLoader` already parses `cgltf_texture_view` for
  base color - wiring it to a bindless/array texture SSBO is the next step.
- **Multiple importance sampling / NEE**: needed once scenes have small,
  bright lights (the current pure-BSDF-sampling path tracer converges
  slowly for those). Note `LightList` already exists for the flare pass and
  could be reused for NEE in the main path tracer too.
- **Pixel-pupil aperture sampling**: see "Physical lens camera" above - a
  documented, deliberately deferred performance optimization.
- **More lens presets / additional glasses**: `optics/GlassCatalog` only
  ships the 3 glasses the built-in Tessar needs; adding presets like a
  Petzval or Cooke Triplet needs a separately-sourced, verified public-
  domain prescription (patents and classic optics textbooks are good
  sources) rather than invented numbers.
- **BLAS/TLAS + instancing**: see above.
- **Multi-scene / hot reload**: `Application::LoadScene()` is the single
  entry point; hot-reloading a `.glb` mid-session is mostly a matter of
  re-running `GLTFLoader::Load` + `BVHBuilder::Build` and calling
  `PathTracer::Reset()`.

## Third-party libraries

All fetched via CMake `FetchContent` in `cmake/FetchLibs.cmake`, pinned to
an exact tag/commit:

| Library | Role | Notes |
|---|---|---|
| GLFW 3.4 | window, input, GL context | |
| glew-cmake 2.3.1 | OpenGL function loading | `libglew_static` target |
| GLM 1.0.1 | math | header-only |
| Dear ImGui v1.92.8-docking | UI | no upstream CMake support; vendored as a plain library target in `FetchLibs.cmake` |
| cgltf v1.15 | `.gltf`/`.glb` parsing | single header |
| stb (pinned commit) | `stb_image`/`stb_image_write` | PNG export, texture decode |
| tinybvh (pinned commit) | CPU BVH build | GPU traversal is hand-written in `pathtrace.comp` |
| Intel Open Image Denoise 2.5.0 | denoising | optional, prebuilt Windows binaries fetched by URL; `ROYALGL_ENABLE_OIDN` can turn it off, and the build degrades gracefully if the package isn't found |

See the root `README.md` for build instructions.
