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
               world-space triangle soup + materials + camera, grouped into
               movable SceneInstances with UI-editable TRS transforms),
               GLTFLoader (cgltf-based .gltf/.glb -> Scene).
  bvh/         BVHBuilder: two-level acceleration structure - one cached
               tinybvh BLAS per SceneInstance plus a small TLAS over the
               instance AABBs, flattened into the single-level Wald BVH2
               layout the GPU traversal consumes (TLAS leaves are copies of
               the BLAS root nodes with offset pointers, so shaders needed
               no changes). Moving an instance rebuilds only its BLAS + the
               TLAS, ASYNCHRONOUSLY on a worker thread (the build is CPU-
               side); the main thread applies the finished result and
               re-uploads (nodes, triangle index permutation, triangle
               data, materials). Frame-persistent ReSTIR surface data -
               both G-buffer halves, reservoir reconnection vertices,
               cached light-subpath ends, NEE light points - is stored in
               instance OBJECT SPACE plus an instance id and converted
               with the CURRENT matrices on load (RestirLoadGBuf /
               RestirObjToWorld* in restir_common.glsl; matrices uploaded
               per frame from the BVH builder's effective transforms, up
               to 16 instances). Stored surfaces therefore TRACK moving
               objects: reconnections target the vertex on the moved
               instance instead of a phantom at its old placement (the
               cause of the moving-object brightening transient), and
               temporal anchors follow the geometry. Static scenes
               round-trip exactly. Residual staleness (cached suffix
               radiance from the old configuration) is bounded and washes
               out within ~confidence-cap frames. See the "Instances" UI
               window; ROYALGL_MOVE=<rad/s> exercises the pipeline
               headlessly.
  pathtracer/  RenderSettings (UI-tunable knobs), PathTracer (compute shader
               dispatch + progressive accumulation image + per-frame UBO +
               the BDPT pass buffers), LightTree (4-wide SAOH light BVH over
               emissive triangles for next-event estimation, plus the power
               CDF the bidirectional kernels sample from - see "Direct
               lighting" and "Bidirectional path tracing" below).
  denoise/     Denoiser: optional Intel Open Image Denoise wrapper (pimpl'd
               so the rest of the app never needs to #ifdef around it).
  io/          ImageExport: HDR accumulation buffer -> tonemapped PNG.
  ui/          UILayer: Dear ImGui panels (stats, camera, render settings,
               denoise, export).
shaders/
  common.glsl       Struct declarations shared by all shaders (mirrors
                    gfx/GPUTypes.h field-for-field) + shared RNG/Ray/Hit/BVH
                    traversal (closest-hit and shadow-ray variants) + the
                    shared BSDF layer (Lambertian + delta dielectric).
  light_tree.glsl   Light tree sampling for NEE: stochastic descent, leaf
                    triangle pick, pdf re-descent for MIS.
  pathtrace.comp    Unidirectional megakernel: ray gen (pinhole formula),
                    BVH traversal, light-tree NEE + MIS shading, accumulate.
  bdpt_common.glsl  BDPT shared layer: light-vertex/splat SSBOs, camera
                    projection/pdf helpers, power-CDF light sampling, and
                    the recursive MIS quantity documentation.
  bdpt_light.comp   BDPT pass 1: light subpaths - vertex storage + t=1
                    camera connections (fixed-point atomic splats).
  bdpt_eye.comp     BDPT pass 2: eye subpaths - s=0/s=1/s>=2 strategies
                    with recursive MIS weights, accumulate.
  bdpt_resolve.comp BDPT pass 3: drain splat buffer into the accumulation
                    image, clear it.
  tonemap.vert/frag  Full-screen blit: exposure + ACES tonemap + gamma.
```

## Data flow, one frame

1. `Application` polls GLFW, feeds mouse/keyboard into `Camera` (orbit/dolly/
   pan) unless ImGui wants the input.
2. If the camera, scene, or `RenderSettings` changed since the last frame,
   `PathTracer::Reset()` clears the accumulation image and the sample
   counter.
3. `PathTracer::Render()`:
   - uploads a `GPUFrameUBO` (camera basis vectors, FOV, background,
     exposure, sample index, max bounces, light count + NEE flag) to UBO
     binding 0,
   - binds the BVH node / triangle-index / triangle / material SSBOs
     (bindings 1-4, owned by `BVHBuilder`), the light tree SSBOs
     (bindings 5-7, owned by `LightTree`) and the accumulation image
     (image unit 0),
   - dispatches `pathtrace.comp` with one thread per pixel. Each thread
     generates a primary ray via the pinhole formula, traces **one full
     path** (up to `maxBounces` diffuse bounces with Russian roulette),
     at each vertex adding MIS-weighted direct light from one light-tree
     NEE sample (see "Direct lighting" below), and adds the result to the
     accumulation image (`imageLoad` + `imageStore`; safe without atomics
     because each pixel is written by exactly one invocation per
     dispatch).
4. `FullscreenPass` draws a full-screen triangle sampling the accumulation
   image, dividing by the sample count, applying exposure + ACES + gamma,
   into the default framebuffer.
5. `UILayer` draws ImGui panels on top; if "Denoise" or "Export PNG" was
   clicked, `Application` reads the accumulation image back
   (`Texture::ReadPixelsFloat`) and calls `Denoiser`/`ImageExport`.
6. `Window::SwapBuffers()`.

## GPU binding table

| Binding | Type | Owner | Contents |
|---|---|---|---|
| UBO 0 | Uniform | `PathTracer` | `GPUFrameUBO` |
| Image 0 | image2D | `PathTracer` | accumulation image |
| SSBO 1 | Storage | `BVHBuilder` | BVH nodes |
| SSBO 2 | Storage | `BVHBuilder` | triangle index permutation |
| SSBO 3 | Storage | `BVHBuilder` | triangles |
| SSBO 4 | Storage | `BVHBuilder` | materials |
| SSBO 5 | Storage | `LightTree` | light tree nodes (4-wide) |
| SSBO 6 | Storage | `LightTree` | emissive triangles, leaf-list order |
| SSBO 7 | Storage | `LightTree` | scene-triangle -> light-triangle map |
| SSBO 8 | Storage | `PathTracer` | BDPT light subpath vertices |
| SSBO 9 | Storage | `PathTracer` | BDPT t=1 splats (fixed-point RGB, atomicAdd) |
| SSBO 10 | Storage | `LightTree` | power-proportional light CDF |
| SSBO 11 | Storage | `PathTracer` | BDPT per-path stored-vertex counts |
| SSBO 12 | Storage | `PathTracer` | BDPT camera-anchored light-selection pdf cache |
| SSBO 13 | Storage | `LensSystem` | lens surfaces, walk order, Sellmeier/Cauchy media |
| SSBO 14 | Storage | `PathTracer` | per-pixel pupil discs (lens mode) |

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

Two material models, shared by all kernels through common.glsl's
`SampleBsdf`/`EvalBsdf` (`Material::type`, `GPUMaterial::params.w`):

- **Diffuse**: two-sided Lambertian with cosine-weighted hemisphere
  sampling. Emissive materials double as area lights.
- **Glass** (delta dielectric): perfect Fresnel reflection/refraction
  (exact unpolarized dielectric Fresnel, `params.z` = IOR), the
  reflect/refract choice made proportional to the reflectance so the
  discrete probability cancels out of the throughput. Delta vertices
  never take part in NEE or BDPT connections; their pdf-0 marker is what
  routes MIS weight 1 to the strategies that can reach them. Refraction
  is non-symmetric under transport (Veach 5.3.2): the (etaI/etaT)^2
  solid-angle-compression factor applies to radiance (eye paths) only,
  never to importance (light paths) - `SampleBsdf`'s `isLightPath` flag.

The fallback scene includes a small glass duck (assets/scenes/Duck.glb,
merged via `Scene::MergeInstance`) in the middle of the Cornell box as a
standing test case for the delta paths and their caustics.

`GPUMaterial::params` still carries `metallic`/`roughness`, so the
extension point for a full metallic-roughness GGX BRDF is the shared
`SampleBsdf`/`EvalBsdf` pair - every kernel picks the change up from
there (BDPT additionally relies on `pdfRev` being filled in correctly).

## Direct lighting: light tree NEE + MIS

Every path vertex estimates direct lighting with two MIS-combined
strategies (balance heuristic, both pdfs in solid-angle measure):
next-event estimation - sample a point on an emissive triangle via the
light tree, trace a shadow ray - and regular BSDF sampling, whose emitter
hits are weighted by the probability that NEE *would* have sampled that
same triangle from the previous vertex. Camera rays and backfacing emitter
hits have no competing NEE strategy, so their MIS weight is 1 (emitters
emit two-sided, but NEE only samples the front side defined by the
triangle winding).

The light tree is a port of RoyalTracer-DX's implementation (rdn/
LightTree.h + shaders/LightTree_v8.hlsli, itself after Conty Estevez &
Kulla, "Importance Sampling of Many Lights with Adaptive Tree Splitting"),
collapsed from that engine's two-level TLAS/BLAS to a single level because
RoyalGL's scene is one flattened world-space triangle soup:

- **Build** (`pathtracer/LightTree.cpp`, CPU, rebuilt on scene load and on
  any material edit): 4-wide BVH over emissive triangles, binned SAOH
  splits (cost = power x surface area x normal-cone orientation measure,
  with a short-axis regularizer), normal cones merged per the paper's
  Algorithm 1. Each triangle's weight is `area * luminance(emission)`.
  Leaves hold a single triangle (the DX source uses 16, but it has ReSTIR
  on top to clean up candidate quality; without it, the tree's geometric
  importance should drive every selection), so the power-weighted in-leaf
  pick only matters for degenerate-split fallback leaves.
- **Sampling** (`shaders/light_tree.glsl`): stochastic descent picking each
  child proportionally to a trig-free importance estimate (power /
  squared-distance x cone orientation x receiver cosine, all widened by
  the cluster's subtended uncertainty angle, with 0.01 relative floors so
  boundary regions stay sampleable), then a power-weighted pick inside the
  leaf, then a uniform point on the triangle. One random number drives the
  whole descent via rescaling. Child probabilities are Q-smoothed
  (near-balanced children move toward uniform) to remove banding at
  cluster boundaries - applied identically on the pdf side, so MIS stays
  unbiased.
- **Pdf for MIS** (`LT_PdfSelectTriangle`): deterministic re-descent to the
  child whose triangle range contains the target (the GPU triangle array
  is emitted in leaf-list order, so every node's subtree is a contiguous
  index interval), multiplying the same smoothed probabilities the sampler
  used.

`RenderSettings::enableNEE` toggles the whole scheme at runtime (off =
pure BSDF sampling); both settings converge to the same image, which makes
the toggle a useful bias check. The light tree only serves the
unidirectional pipeline - see the note on receiver dependence below.

## Bidirectional path tracing (Veach ch. 10, recursive MIS)

`RenderSettings::enableBidir` (default on) switches `PathTracer::Render`
from the unidirectional megakernel to a three-pass bidirectional pipeline.
Both pipelines converge to the same image (verified against each other),
but caustics cast by the glass duck - light -> glass -> ... -> diffuse -
are only sampled efficiently by the bidirectional strategy set.

**Passes** (all compute, separated by `glMemoryBarrier`):

1. **bdpt_light.comp** - one light subpath per invocation (up to 262144
   paths per frame, hash-assigned to pixels): emit from a CDF-picked light
   triangle, bounce up to the path-length cap, and at every non-delta
   vertex (a) store position/normal/throughput/dVCM/dVC/incoming-direction
   into the light-vertex SSBO and (b) connect to the camera (the t=1
   "light tracing" strategy), splatting through a fixed-point `atomicAdd`
   buffer since core GL has no float image atomics.
2. **bdpt_eye.comp** - one eye subpath per pixel: s=0 emitter hits, s=1
   light sampling, and s>=2 connections against every stored vertex of one
   light subpath, everything balance-heuristic weighted.
3. **bdpt_resolve.comp** - drains the splat buffer into the accumulation
   image and clears it.

**Recursive MIS weights**: instead of enumerating every (s,t) strategy's
pdf per connection (O(path length) each), every subpath vertex carries two
running quantities dVCM and dVC - the SmallVCM / agraphicsguynotes.com
formulation of Veach's weights, documented in detail at the top of
`shaders/bdpt_common.glsl`. Each connection's balance-heuristic weight is
then O(1): `1 / (wLight + 1 + wCamera)` with both terms read straight off
the two endpoint vertices. Delta vertices zero dVCM and scale dVC by the
bounce cosine (their forward/reverse delta pdfs cancel), which
simultaneously encodes "no strategy can connect here".

**Why a CDF and not the light tree**: the recursive quantities bake the
light-selection pdf into every light-subpath vertex at generation time,
before the eventual connection partner is known. That is only exact when
light selection is receiver-independent, so the bidirectional kernels
sample emitters from a plain power-proportional CDF (SSBO 10, built by
`LightTree::Build` alongside the tree). The adaptive light tree keeps
serving the unidirectional pipeline, where the pdf is evaluated at
connection time.

**Bounds**: total path length is capped at `min(maxBounces+1, 8)` segments
(8 = the light-vertex storage stride); no Russian roulette in the
bidirectional kernels, so the cap is exact and every strategy for a given
path length exists - the MIS weight denominators stay a true partition of
unity. Light-vertex storage tops out at ~130 MB (262144 paths x 8 vertices
x 64 B).

## Material editor

`UILayer::Draw` opens a second, separate top-level ImGui window ("Material
Editor", distinct from the main "RoyalGL" panel) listing every entry in
`Scene::materials` with live `baseColor`/`emissive` (HDR-capable
`ColorEdit3`) /`metallic`/`roughness` widgets. It only edits existing
entries in place - no add/remove/rename, matching `RenderSettings`-style
"simple" scope.

`Application::Run()`'s dirty-check keeps a
`materialsDirty = (m_scene->materials != m_lastMaterials)` snapshot
(mirroring the existing camera/settings snapshots), and on a hit calls
`BVHBuilder::UpdateMaterials()` (re-derives+re-uploads just the materials
SSBO, skipping the full BVH rebuild) *and* `LightTree::Build()` - an
emissive edit re-weights or adds/removes light tree leaves, so the tree
built at startup goes stale - before resetting accumulation.

## Emitter semantics and fireflies

Emitters are one-sided (winding-defined front) and terminate paths in
every sampler of both pipelines. Both properties are load-bearing:
two-sided emission is invisible to the light tree's normal cones and to
NEE (weight-1 unMIS'd hits), and non-terminating paths bounce inside the
2 cm gap between the fallback scene's light panels and the ceiling,
stacking repeated emission adds into fireflies. With both in place the
accumulation buffer's maximum equals the emitter radiance exactly and
per-sample noise dropped ~3.6x (measured via `ROYALGL_STATS=1`, which
logs luminance tail percentiles, a local-residual noise metric, and GPU
per-pass timings; `ROYALGL_BIDIR/NEE/LENS` override settings for scripted
A/B runs).

The unidirectional MIS weights use the deterministic light-tree pdf
re-descent on BOTH sides of the partition (not the stochastic descent's
own pdf): weights only need one consistent function to sum to 1, and the
contribution itself still divides by the true sampling pdf. BDPT
separates the same two roles explicitly (see shaders/bdpt_common.glsl):
light subpaths start from a light-tree descent anchored at the camera
(fixed per frame, so the recursive MIS quantities stay exact, and light
subpaths concentrate on view-relevant emitters in large scenes), with the
per-light eval pdf cached once per frame by bdpt_lightsel.comp. The eye
pass also subsamples s>=2 connections (one random valid light vertex per
eye vertex, scaled by the count).

## Physical lens camera (Steinert et al. 2011)

`RenderSettings::cameraMode` switches ray generation from the pinhole
formula to the paper's full lens simulation, in BOTH pipelines:

- **Prescription** (`optics/LensSystem`, `assets/lenses/*.lens`): the
  paper's Fig. 4 table format verbatim - signed radius (positive = center
  of curvature on the image side), thickness, Schott material, semi-
  diameter, front element first; uniform scale changes the focal length;
  the f-number scales the stop radius from the prescription's design
  f-number (Eq. 5 via linear paraxial pupil imaging).
- **Dispersion** (`optics/Glass.h`): Sellmeier coefficients from the
  Schott catalog (sec. 3.2); discontinued glasses (LLF7, SF7) fall back
  to a two-term Cauchy fitted to their (nd, vd) entry.
- **Tracing** (`shaders/lens_common.glsl`): exact per-wavelength Snell
  refraction at every spherical cap, walked rear -> front in millimeters
  with the mm -> m conversion applied once on the exit ray (sec. 4.1);
  N-gon blade test at the stop; all five Seidel aberrations plus
  chromatic aberration emerge from the geometry.
- **Spectral rendering**: one wavelength per path, importance-sampled
  from a truncated Gaussian matched to the CIE color-matching functions
  (uniform sampling makes the CIE->RGB weight swing ~[-3, +10] and shows
  up as colored fireflies), converted to RGB through CIE Gaussian fits -
  scene shading stays RGB, the lens disperses continuously.
- **Pixel pupils** (`shaders/lens_pupil.comp`, sec. 4.2): per-pixel disc
  approximation of the visible aperture image, precomputed on the GPU
  (as the paper suggests) whenever lens/sensor geometry changes; path
  generation samples the disc instead of the full rear element (the
  paper measured ~10% -> ~80% ray passage), and zero-radius pupils mark
  fully vignetted pixels.
- **BDPT through the lens**: eye subpaths use the same lens walk (the
  sensor factor becomes the initial eye throughput); the t=1 strategy
  samples a point on the front element and traces the reverse walk
  scene -> sensor, splatting carried FLUX - invariant through the
  deterministic walk, so no analytic lens pdf or Jacobian is needed.
  This is also the paper's sec. 4.3 light-traced flare architecture,
  and it is what renders lens-mode caustics efficiently. MIS weights
  use an effective-pinhole camera pdf (paraxial EFL, computed by
  `LensSystem::Derive`) evaluated from the path's FIRST SCENE VERTEX on
  both strategies - a deterministic function of the path, so the
  partition of unity holds even though the pdf is approximate (the
  approximation costs weight optimality, not unbiasedness). Known
  residual: lens-mode unidir and bidir means agree to ~3%, not exactly -
  candidates are the splat's negative-spectral-lobe clamp (fixed-point
  splats are unsigned) and the flux normalization constant; pinhole-mode
  parity remains exact.
- **Flare** (sec. 4.3, partial): the t=1 lens walk is where ghost paths
  belong; an optional stochastic Fresnel reflect/refract branch in the
  eye-side walk (`LensSettings::enableFlare`, off by default - rare
  branches read as fireflies) adds two-bounce ghosts of in-view sources.
  Sec. 4.4 Keller-cone aperture diffraction is not implemented.

## Extension points (deliberately not built yet)

- **Textures**: `Material`/`GPUMaterial` carry factors only, no texture
  indices/samplers yet. `GLTFLoader` already parses `cgltf_texture_view` for
  base color - wiring it to a bindless/array texture SSBO is the next step.
- **BLAS/TLAS + instancing**: see above. Would also restore the light
  tree's second (TLAS) level from the DX source.
- **Adaptive tree splitting**: the paper's variance-driven descent into
  *multiple* subtrees per sample; the DX source's Q-smoothing is a cheap
  stand-in for it, ported here as-is.
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
