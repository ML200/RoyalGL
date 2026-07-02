# RoyalGL

A real-time-progressive, offline-quality path tracer built on OpenGL compute
shaders. Point it at a `.glb`/`.gltf` file (or let it fall back to a built-in
Cornell-box scene), and it accumulates path-traced samples into the viewport
every frame - like the renderer preview in a modern DCC tool - with an ImGui
control panel, optional Intel Open Image Denoise integration, and PNG export.

Direct lighting uses next-event estimation through a **light tree** (a 4-wide
BVH over emissive triangles with SAOH splits and normal cones, ported from
[RoyalTracer-DX](https://github.com/ML200/RoyalTracer-DX), after Conty
Estevez & Kulla's *"Importance Sampling of Many Lights with Adaptive Tree
Splitting"*), MIS-combined with BSDF sampling via the balance heuristic - see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#direct-lighting-light-tree-nee--mis).

A toggleable **bidirectional path tracer** (Veach ch. 10) runs as three
compute passes - light subpaths with t=1 camera splats, eye subpaths with
vertex connections, splat resolve - using the O(1) recursive MIS weight
formulation (SmallVCM-style dVCM/dVC). The material model includes a delta
dielectric (Fresnel glass), and the fallback Cornell box ships a small glass
duck whose caustics only the bidirectional strategies capture efficiently -
see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#bidirectional-path-tracing-veach-ch-10-recursive-mis).

The camera can switch from a pinhole to a **physical lens** - a
reimplementation of Steinert et al., *"General Spectral Camera Lens
Simulation"* (CGF 2011): prescription tables (the paper's Tessar ships in
`assets/lenses/`), Sellmeier glass dispersion, exact per-wavelength Snell
tracing (all Seidel + chromatic aberrations), N-gon aperture bokeh,
continuous spectral sampling, GPU-precomputed per-pixel pupil sampling
(sec. 4.2) and stochastic Fresnel ghosts - see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#physical-lens-camera-steinert-et-al-2011).

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how it's built and where
the extension points are.

## Building

Requires CMake 3.21+, a C++20 compiler (tested with MSVC / Visual Studio
2022), and an internet connection on first configure (dependencies are
fetched via CMake `FetchContent`).

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

The executable lands in `build/bin/<Config>/RoyalGL.exe`. It loads shaders
and assets directly from the source tree (`shaders/`, `assets/`) at
runtime via absolute paths baked in at configure time, so you can edit a
`.glsl`/`.comp`/`.vert`/`.frag` file and just re-run the executable - no
rebuild needed. A copy of `shaders/` and `assets/` is also synced next to
the executable after every build, for anyone who wants to distribute the
`bin/<Config>` folder standalone.

Run it with an optional scene path:

```
build\bin\RelWithDebInfo\RoyalGL.exe assets\scenes\Duck.glb
```

With no argument, it loads a small built-in Cornell-box scene.

### Open Image Denoise

`ROYALGL_ENABLE_OIDN` (default `ON`) fetches a prebuilt Intel Open Image
Denoise 2.5.0 Windows binary package and links against it. If that fails
(no network, non-Windows, package not found), the build still succeeds and
the "Denoise" button in the UI is simply disabled - nothing else depends on
OIDN being present. Set `-DROYALGL_ENABLE_OIDN=OFF` to skip it entirely.

## Controls

- **Left-drag**: orbit camera
- **Right/middle-drag**: pan camera
- **Scroll**: dolly (zoom)
- The ImGui panel exposes bounce count, exposure, background color/
  intensity, a sample cap, a bidirectional/unidirectional toggle, an NEE
  on/off toggle (unidirectional only), denoise, and PNG export.
- A separate "Material Editor" window lists every material in the current
  scene and lets you edit base color, emissive (HDR) color, type
  (diffuse/glass), IOR, metallic, and roughness live.

Moving the camera or changing a render setting resets progressive
accumulation; otherwise every frame adds one more path-traced sample.

## Third-party libraries

All pinned and fetched via `cmake/FetchLibs.cmake` - see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#third-party-libraries) for the
full list and versions: GLFW, GLEW (glew-cmake), GLM, Dear ImGui (docking),
cgltf, stb, [tinybvh](https://github.com/jbikker/tinybvh), and Intel Open
Image Denoise.
