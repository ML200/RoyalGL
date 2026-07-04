# ReSTIR BDPT Implementation Plan for RoyalGL

Based on: *Hedstrom, Kettunen, Lin, Wyman, Li — "ReSTIR BDPT: Bidirectional ReSTIR
Path Tracing with Caustics", ACM TOG 2025.*

This document maps the paper's algorithm onto RoyalGL's existing OpenGL 4.6 compute
BDPT and lays out a phased implementation plan.

---

## 1. What the paper does (condensed)

ReSTIR BDPT = BDPT initial sampling + GRIS spatiotemporal reuse, with four key ideas:

1. **Extended path space.** Reservoirs store *path–technique pairs* (x̄, τ) where
   τ = (s, t) is the BDPT strategy that produced the path. The RIS target function is
   `p̂(x̄, τ) = ω_τ(x̄) · luminance(f(x̄))` — the technique MIS weight is folded into the
   target. Shift mappings never change τ (Eq. 18–26). This makes random replay
   well-defined: knowing τ tells you which subpath seeds to replay and where the
   connection sits.

2. **Technique-specific bidirectional hybrid shift** (Sec. 5):
   - **t ≥ 2** (camera-side techniques: PT hit s=0, NEE s=1, vertex connection s≥2):
     camera subpath uses ReSTIR PT's hybrid shift (random replay until two consecutive
     *connectable* vertices, then reconnect); light subpath is shifted by pure random
     replay; the subpath connection edge is re-evaluated. Connections only occur
     between connectable (rough) vertices, so the reconnection at the join always
     succeeds. Shift fails if any vertex changes rough/non-rough classification
     (bijectivity).
   - **t ≤ 1, non-caustic** (light tracing where secondary hit x₂ is rough): *reverse
     hybrid shift* — random replay from the light until x₂, then reconnect x₂ to the new
     pixel's primary hit x₁′. Fails if x₁′ is not rough.
   - **t ≤ 1, caustic** (x₂ non-rough): pure random replay from the light; the shifted
     path lands wherever its new x₁ projects on screen. Temporal only; the shift
     redistributes caustic samples into pixels (Appendix A justifies this in GRIS).

3. **Caustic reservoirs** (Sec. 5.1): a second per-pixel reservoir that only holds
   caustic t≤1 paths. Never spatially reused (spatial neighbors would starve
   low-probability caustics). Temporally accumulated with confidence updated by a
   *proxy* `c_i += c_v` where v is the motion-vector-mapped previous pixel (confidence
   must not depend on realized samples). Final image = regular estimate + caustic
   estimate.

4. **Recursive reconnection MIS** (Sec. 6): after a shift, ω_τ must be recomputed for
   the shifted path. Building on VCM-style recursive MIS (per-vertex d^p ≡ dVCM and
   d^VC), the paper caches four extra scalars per stored camera suffix
   (γ̄, λ̄^VC, λ̄^P, σ̄ — Eq. 42–45) plus a geometry ratio, so d^p_{t−1} and d^VC_{t−1}
   can be reconstructed at the reconnection vertex without walking the suffix
   (Eq. 46–47). Two cheaper alternatives: **copy ω_τ from the base path** (biased,
   small darkening near corners, bounded by Sec. 6.4) and **lightweight BDPT**
   (only s∈{0,1} and t=1 — no vertex connections — so the expensive general case
   never arises).

Implementation shape in the paper (Sec. 7, Algorithms 1–3):

- **Algorithm 1 — light pass:** trace N_L light subpaths (N_L ≈ pixel count); append
  connectable vertices to a **Light Vertex Cache (LVC)**; connect each to the camera
  (t=1) and insert the resulting one-sample reservoir into a **Light Reservoir Map
  (LRM)** — a GPU multimap keyed by pixel (insert + sort, to avoid atomic reservoir
  merges).
- **Algorithm 2 — camera pass:** per pixel, trace a camera subpath; at each vertex
  generate s=0 / s=1(NEE) / s≥2 (uniform LVC pick, p_L = N_L/|LVC|) candidates and
  stream-RIS them into the pixel reservoir with initial MIS weights m = 1/N_L for
  t≤1 and m = 1 otherwise (Eq. 26); then merge this pixel's LRM entries, routing
  caustic ones into the caustic reservoir. Confidence of the result is always 1.
- **Algorithm 3 — shift:** used by both temporal and spatial reuse; retraces
  light subpath (replay), camera prefix (replay+reconnect), connection edge.
- Temporal reuse: balance heuristic, 2 candidates. Spatial reuse: pairwise MIS,
  a few neighbors. Confidence cap 20. Integration in solid-angle measure with the
  Jacobians of Appendix B (replay: pdf ratios; reconnection: geometry-term ratios).
- Stability fix: reject non-caustic t≤1 candidates whose reconnection to the pixel's
  V-buffer primary hit fails (tiny darkening bias in penumbras, big stability win).

Paper stats to calibrate against: reservoir 244 B (160 B without recursive-MIS cache),
LVC vertex 112 B, ~50 ms/frame at 1080p on a 4090, N_L = 2M, max 20 bounces /
8 "diffuse" bounces, connectable-roughness threshold 0.08, RR
p_terminate = 0.2·roughness, confidence cap 20.

---

## 2. Mapping onto RoyalGL — assets, gaps, simplifications

### What we already have (big head start)

| Paper ingredient | RoyalGL status |
|---|---|
| BDPT with light + camera passes | ✅ `bdpt_light.comp` / `bdpt_eye.comp`, 3-pass pipeline in `PathTracer.cpp` |
| Recursive MIS (d^p/dVCM, d^VC/dVC) | ✅ `bdpt_common.glsl` implements the van Antwerpen/Georgiev recursion — the exact foundation Sec. 6 builds on |
| LVC-style shared light vertices | ✅ `lightVerts[pathIdx*8+i]` SSBO (binding 8) + `lightVertCount` (11); eye pass connects to a random light path's vertices |
| Receiver-independent light selection | ✅ camera-anchored power CDF + `bdpt_lightsel.comp` — replayable, exactly what shift PDF re-evaluation needs |
| Per-pixel scatter from light pass | ✅ fixed-point splat buffer (binding 9) + `bdpt_resolve.comp` — the same binning problem the LRM solves, currently solved with additive atomics |
| Rough/non-rough classification | ✅ trivially: material type 0 (diffuse) = connectable, type 1 (glass delta) = non-connectable. No roughness threshold needed until GGX lands |
| Solid-angle integration | ✅ current code folds geometry terms into dVCM/dVC in solid angle; Appendix B Jacobians are the natural fit |

### What's missing

1. **Reservoirs / GRIS machinery** — nothing exists (no RIS, no UCW, no confidence).
2. **Temporal infrastructure** — no G-buffer/V-buffer, no previous-frame camera, no
   motion vectors, no ping-pong state. `Application.cpp` *resets* accumulation on any
   camera move — the opposite of what ReSTIR needs.
3. **Replayable RNG** — current shaders advance a stateful WangHash/xorshift stream.
   Shift mappings need *counter-based* random numbers: `rand(pathSeed, bounce, dim)`
   so any suffix can be replayed from a stored 32-bit seed.
4. **LRM** — light-tracing candidates must be *reservoirs* per pixel, not additive
   splats. Needs a binning structure (paper: insert + prefix-sum sort).
5. **Per-frame full-frame dispatch** — the adaptive 30 ms tiling
   (`m_rowsPerTile`/`m_pathsPerChunk`) slices one sample across frames; ReSTIR needs
   the whole pass sequence to complete every frame.
6. **Reconnection data in reservoirs** — reconnection vertex, partial throughputs,
   MIS cache (γ̄, λ̄^VC, λ̄^P, σ̄, d^p_r, d^VC_r, geometry ratio).

### RoyalGL-specific simplifications (v1 scope)

- **Materials are binary** (Lambert diffuse / delta glass). The hybrid shift's
  "two consecutive rough vertices" test is a material-type check. Caustic
  classification (`x₂ non-rough`) = "x₂ is glass". This removes most of the paper's
  roughness-threshold subtlety.
- **Scenes are static** (no animation system). Diffuse motion vectors are pure camera
  reprojection; light-subpath temporal replay of an unchanged scene reproduces the
  identical subpath, so temporal light-subpath shifts are near-free (still implement
  the replay path — it validates the machinery and future-proofs animation).
- **Pinhole camera only for ReSTIR mode (v1).** The physical lens (stochastic pupil
  samples, spectral wavelength per path, sensor splatting) makes the primary hit a
  random variable per pixel — that's Area ReSTIR territory (Zhang et al. 2024), out
  of scope. Lens mode falls back to the existing accumulation BDPT. Revisit later.
- **t = 0 omitted** (paper does the same; zero probability for pinhole).
- **Russian roulette**: paper requires p_terminate to not depend on camera-side info.
  Use a material-only rule (e.g. terminate diffuse bounces with fixed prob after
  bounce k, never on glass) applied identically in light and camera tracing and in
  replay — RR decisions must be reproducible from the counter-based RNG.

---

## 3. Key design decisions

1. **Separate ReSTIR frame path.** Add `enableRestir` next to `enableBidir` in
   `RenderSettings`. ReSTIR mode uses its own dispatch sequence in
   `PathTracer::Render()` (full-frame, fixed pass graph, resolution-scale option)
   and its own output semantics: per-frame estimate written to the accum image
   (optionally EMA-blended or progressively averaged when camera is static).
   Existing PT/BDPT paths untouched.
2. **Counter-based RNG first.** Replace stateful RNG in the BDPT shaders with
   `pcg/philox-style hash(seed, bounce, dim)`; a subpath is fully determined by one
   32-bit seed. This lands as its own refactor (Phase 0) and is verified by
   image-diff against the old renderer (same convergence, different noise pattern).
3. **LRM as count + prefix-sum + scatter** (paper-faithful), implemented as three
   small compute passes over a `(pixelCount+1)`-entry counter buffer and a compact
   entry array (entry = light-tracing candidate reservoir, ~48–64 B). A per-pixel
   linked list (atomic head exchange) is the fallback if the scan is a pain —
   correctness-equivalent, worse coherence. The same structure is reused for
   temporal caustic re-binning.
4. **MIS weight strategy is staged:** Phase A uses **copied ω_τ** (biased, Sec. 6.4)
   to get end-to-end images, Phase B implements **recursive reconnection MIS**
   (Eq. 42–47) behind a toggle, so bias can be A/B tested exactly like the paper's
   Fig. 5. **Lightweight BDPT** (s≤1, t≤1 only) ships in between as a permanently
   useful low-cost mode.
5. **Two reservoir buffers per pixel (regular + caustic), ping-ponged** across frames
   (4 buffers total). Target ≤ 192 B per regular reservoir at v1 (we can be smaller
   than the paper's 244 B: no wavelength, binary materials, fewer flags).
6. **Solid-angle Jacobians** (Appendix B): replay = ratio of solid-angle pdfs
   (Eq. 53–54), reconnection = ratio of geometry terms (Eq. 55–56), including the
   connection edge for s≥1, t≥2.

---

## 4. New GPU data structures

> **Phase 0 status: DONE.** Counter-based RNG (`RngStream(vertex, purpose)` in
> common.glsl), `enableRestir` plumbing + UI + `ROYALGL_RESTIR`/`ROYALGL_RESTIR_DEBUG`
> env overrides, G-buffer pass, prev-camera reprojection + debug views, reservoir
> allocation. Verified: unidirectional / BDPT / ReSTIR-mode converged means agree
> (0.116647 / 0.116592 / 0.116592); motion debug view shows exactly zero motion for a
> static camera.
>
> **Binding scheme changed from this section's original sketch:** common
> hardware (Intel iGPUs) caps SSBO bindings at 16, so paired Cur/Prev bindings
> 16–24 would not be portable. (The actual dev GPU turned out to be an RTX
> 5090 — see the Phase 6 status — which exposes 96 bindings; the 16-binding
> budget is kept anyway for portability.) Actual scheme (shaders/restir_common.glsl): reservoirs (PathReservoir +
> CausticReservoir combined in one 272 B `PixelReservoirs` struct) at **binding 15**,
> G-buffer at **binding 0**, each buffer holding BOTH ping-pong halves
> (2 × pixelCount), the halves selected by a parity flag in `restirParams.w`.
> Bindings 13/14 (lens-only) remain reclaimable for the Phase 2 LRM in shaders that
> skip lens_common.glsl.

Bindings 15+ are free. All structs are std430; exact packing finalized in-code.

```glsl
// binding 15/16 (ping/pong): regular path reservoirs, one per pixel
struct PathReservoir {
    // --- GRIS state ---
    float W;              // unbiased contribution weight
    float pHat;           // target value at this domain = omega_tau * lum(f)
    float c;              // confidence
    uint  techFlags;      // s:8 | t:8 | flags:16 (caustic, rcValid, deltaSuffix...)

    vec3  f;              // RGB contribution of the selected path (for shading)
    uint  camSeed;        // camera subpath replay seed

    uint  lightSeed;      // light subpath replay seed (t<=1 or s>=1)
    uint  lightPathIdx;   // this-frame LVC path index (spatial reuse fast path)
    float lightSelPdf;    // cached p_L terms
    uint  rcIndex;        // r = index of reconnection vertex on camera subpath

    // --- reconnection vertex x_r (camera side) ---
    vec4  rcPosMat;       // xyz pos, w material id
    vec4  rcNormal;       // xyz normal
    vec4  rcWoPdf;        // xyz outgoing dir on base path, w forward pdf
    vec4  tputToRc;       // rgb prefix throughput, w unused
    vec4  tputAfterRc;    // rgb suffix contribution (radiance arriving via wo)

    // --- light subpath end y_{s-1} (connection endpoint) ---
    vec4  lyPosMat;       // xyz pos, w material id
    vec4  lyNormalLen;    // xyz normal, w = s (vertex count)
    vec4  lyTput;         // rgb light-side throughput

    // --- recursive reconnection MIS cache (Sec. 6.2) ---
    vec4  misA;           // dP_r, dVC_r, geomRatio (Eq. 46), gammaBar
    vec4  misB;           // lambdaVC, lambdaP, sigmaBar, omega_tau (base)
};  // 208 B — trim during implementation
```

```glsl
// binding 17/18 (ping/pong): caustic reservoirs, one per pixel
// Same layout minus reconnection/MIS fields (pure replay): ~96 B.
struct CausticReservoir {
    float W; float pHat; float c; uint flags;
    vec3  f; uint lightSeed;          // path fully determined by light replay
    uint  sLen; float misOmega; vec2 subpixel; // landing position for filter h_i
};
```

```glsl
// binding 19: compacted global LVC (replaces per-path fixed slots for s>=2 picks)
//   existing LightVertex (64 B) + one extra vec4:
//     rcMis: dP, dVC at vertex, pathSeed (bits), vertexIndexInPath
//   => 80 B per vertex, atomic-append counter in binding 20.

// binding 21: LRM entry array  (uint pixel key + light-tracing candidate, ~64 B)
// binding 22: LRM per-pixel counts / offsets (prefix sum)
// binding 23: G-buffer current  (vec4 posDepth, vec4 normalMat)  [or RGBA32F images]
// binding 24: G-buffer previous
// UBO additions: previous-frame camera matrices for reprojection; restir params
//   (N_L, spatialCount, spatialRadius, confidenceCap, flags).
```

Memory at 1080p (2.07 MP): regular 2×208 B + caustic 2×96 B + G-buffers 2×32 B
≈ **1.4 GB** + LVC (2M × 80 B = 160 MB) + LRM (~2× N_L × 64 B ≈ 256 MB) ≈ **1.8 GB**.
Comfortable on ≥8 GB GPUs; add a resolution scale and N_L slider regardless.

---

## 5. Frame graph (ReSTIR mode)

```
per frame:
 0. UBO upload (current + previous camera)                     [PathTracer.cpp]
 1. bdpt_lightsel.comp        camera-anchored light pdfs        (existing, unchanged)
 2. restir_light.comp         N_L light subpaths:
                                - append connectable verts to LVC (atomic)
                                - t=1 connect-to-camera -> candidate reservoir
                                  -> LRM count pass (atomicAdd per pixel key)
                                  (entries staged in a raw array with pixel keys)
 3. lrm_scan.comp             prefix sum over per-pixel counts
 4. lrm_scatter.comp          scatter staged entries into sorted slots
 5. restir_camera.comp        per pixel:
                                - write G-buffer (primary hit)
                                - trace camera subpath (counter-based RNG)
                                - candidates: s=0 emitter hits, s=1 NEE,
                                  s>=2 uniform LVC connections
                                - stream-RIS into regular reservoir (Eq. 24-26)
                                - merge LRM[pixel]: caustic -> caustic reservoir
                                  (with t<=1 V-buffer rejection heuristic),
                                  else -> regular reservoir
                                - confidence := 1
 6. restir_temporal.comp      regular reservoirs:
                                - motion vector via prev-camera reprojection of
                                  G-buffer position + depth/normal validation
                                - shift prev reservoir into current pixel
                                  (Algorithm 3), balance heuristic 2-way merge,
                                  c := min(c_prev + 1, cap)
 7. restir_caustic_temporal   caustic reservoirs:
       (.comp)                  - replay prev caustic paths in current frame,
                                  project new x1 -> pixel, re-bin via LRM passes
                                - merge into caustic reservoir (Eq. 51-52)
                                - c_i += c_v (motion-vector proxy), capped
 8. restir_spatial.comp       1-2 iterations, 3-5 neighbors in ~30 px radius,
                                pairwise MIS, bidirectional hybrid shift;
                                caustic reservoirs are NOT touched
 9. restir_resolve.comp       color = f_reg * W_reg + f_cau * W_cau
                                -> accum image (direct, EMA, or progressive-if-static)
10. tonemap                    (existing)
```

Passes 2–4 and 7 share the LRM helpers. Passes 6–8 all call the same
`ShiftPath()` implemented in a new `restir_shift.glsl` include.

### The shift kernel (heart of the system, `restir_shift.glsl`)

Faithful port of Algorithm 3 specialized to our two-material world:

```
ShiftPath(Reservoir base, newPrimaryHit, mode):
  1. light subpath: replay from base.lightSeed (static scene: fast path may reuse
     this frame's LVC entry when spatial-in-frame); recompute dP/dVC along it
  2. if caustic t<=1: connect y_{s-1} to camera; landing pixel = project(x1');
     Jacobian = reverse-reconnection (Eq. 56, i=1); done
  3. if non-caustic t<=1: replay light path to x2', reconnect x2' -> newPrimaryHit;
     fail if primary hit is glass; Jacobian Eq. 56 (i=2)
  4. else (t>=2): trace camera prefix from newPrimaryHit replaying base.camSeed dims;
     at first diffuse vertex pair, reconnect to base.rc vertex (occlusion test);
     replay suffix direction rcWoPdf onward if r < t-1;
     connect z_{t-1} to y_{s-1} (occlusion test) if s > 0
  5. recompute omega_tau:  v1 = copy base omega (biased)
                           v2 = Eq. 46/47 from misA/misB cache (unbiased)
  6. return pHat' = omega * lum(f'), f', Jacobian (solid-angle, Eq. 53-56)
```

Shift failure ⇒ contribution 0 (m_i handles it, Eq. 15–17).

---

## 6. Phased milestones

Each phase compiles, runs, and has an explicit acceptance test. Roughly in order of
increasing risk.

### Phase 0 — Infrastructure (no visual change)
- **0.1 Counter-based RNG.** New `Rand(seed, bounce, dim)` in `common.glsl`; migrate
  `bdpt_light.comp` + `bdpt_eye.comp` (and RR) to it. *Test:* converged renders match
  old ones (image diff < noise floor).
- **0.2 Full-frame ReSTIR dispatch mode + settings plumbing.** `enableRestir`,
  resolution scale, N_L, UI toggles in `UILayer.cpp`; frame path skeleton in
  `PathTracer.cpp` that for now just runs 1spp BDPT full-frame per frame.
- **0.3 G-buffer + previous camera + motion vectors.** New pass writes pos/normal/mat;
  debug view visualizing reprojection error. *Test:* reprojected previous position
  matches current within epsilon for a static camera; sensible vectors when orbiting.
- **0.4 Ping-pong reservoir buffers allocated; debug heatmap views** (W, c, technique
  index) wired into the tonemap pass or a debug output switch.

### Phase 1 — GRIS core with camera-side techniques only (≈ ReSTIR PT lite)

> **Phase 1 status: DONE.** restir_camera.comp (initial RIS over s=0/s=1 with
> extended-path-space target ω_τ·lum(f), reconnection-vertex + L_suf + replay-pdf
> caching), restir_shift.glsl (hybrid shift: replay + reconnection, solid-angle
> Jacobians Eq. 53/55, copied ω_τ), restir_temporal.comp (2-way generalized balance
> heuristic with forward+backward shifts, confidence cap 20, null-history domains
> still weighted), restir_spatial.comp (sequential 2-way merges over disk
> neighbors), restir_resolve.comp (ω·f·W). Soak results (fallback Cornell+duck,
> locked camera): RIS-only 0.116483, +temporal 0.116483, +spatial 0.116484,
> +both 0.116482 — all bias-free vs. the deterministic-V-buffer baseline; the
> 0.14% offset vs. the jittered unidirectional reference (0.116647) is primary-
> visibility aliasing, not transport bias. Confidence saturates at cap (temporal
> live); spatial cuts accumulated relNoise 0.0526→0.0428 @256spp.
> Implementation notes: f/pdf convention folds delta Fresnel choice probabilities
> into both (BsdfSample.choicePdf); reservoir regions rotate scratch/final/history
> inside one SSBO (3×pixelCount); ROYALGL_LOCK_CAMERA added for deterministic
> soaks.
- **1.1 Initial RIS in `restir_camera.comp`:** candidates from s=0 and s=1 only
  (ignore LVC and t=1 for now); reservoir stores path, seeds, reconnection vertex.
  Resolve pass shades `f·W`. *Test:* accumulated output (temporal/spatial OFF, average
  many independent frames) matches plain BDPT-with-s≤1 reference. This validates
  UCW bookkeeping — the single most bug-prone piece.
- **1.2 Temporal reuse** with hybrid shift on camera subpaths (replay + reconnect,
  copied ω_τ for now). *Test:* static camera → variance drops hard, no brightness
  drift over 1000 frames (unbiasedness soak: long-average vs reference).
- **1.3 Spatial reuse** with pairwise MIS. *Test:* same soak; inspect for
  correlation blotches; verify confidence capping.

### Phase 2 — Light tracing (t=1) + LRM  → "Lightweight ReSTIR BDPT"

> **Phase 2 status: DONE.** restir_light.comp (Alg. 1: one replayable light
> subpath per invocation, t=1 candidates with m=1/N_L, V-buffer rejection at
> creation), restir_lrm.glsl (LRM as per-pixel LINKED LIST — atomic head
> exchange on binding 14, entry pool on binding 13, the sec.-3 fallback
> instead of count/scan/scatter; lens-only bindings reclaimed since ReSTIR
> is pinhole-only), LRM merge + caustic split in restir_camera.comp (caustic
> reservoir written straight to the final region, per-frame RIS only),
> reverse hybrid shift in restir_shift.glsl (light replay under the
> DESTINATION domain's sampler — dst-anchored tree descent — priced by the
> Eq. 53 pdf ratio; reconnection y_{s-2}→x₁′ with the Eq. 56 endpoint
> geometry ratio; ω copied), caustic term in restir_resolve.comp, debug
> views 7 (caustic W) / 8 (LRM occupancy), `restirLightTracing` toggle +
> `ROYALGL_RESTIR_LIGHT` override.
>
> **Lightweight MIS:** with technique set {s=0, s=1, t=1}, both classic
> recursions are restricted by dropping the interior dVCM additions (those
> are the s≥2 connections): light side keeps the seed terms + the addition
> at the first scatter (`dLW` in restir_light.comp), eye side carries the
> single dVCM₁-seeded term (`dT1` in restir_camera.comp). The s=0/s=1
> weights gained the t=1 competitor (including through delta chains, where
> the s=1 term vanishes but t=1 doesn't).
>
> Soak results (fallback Cornell+duck, locked camera): light-off RIS
> 0.116481 (= Phase 1 baseline), light-on RIS 0.116468 (−0.011%: the
> V-buffer rejection darkening + caustic-on-miss-pixel drops), +temporal
> 0.116466, +spatial 0.116442, +both 0.116431–0.116434 (−0.03%: copied-ω_τ
> darkening — t=1 ω now embeds camera-pdf geometry that changes under
> spatial shifts; the Sec. 6.4-bounded bias Phase 5 removes; temporal-only
> shows none because static-camera shifts are near-identity). Light tracing
> cuts accumulated relNoise: 0.0327 @ 3968 spp vs 0.0341 @ 5120 spp without.
> LRM occupancy ~0.45 entries/pixel/frame at N_L=262144; caustic reservoirs
> populated but sparse until Phase 3 accumulates them.

- **2.1 LRM passes** (count/scan/scatter) replacing additive splatting *in ReSTIR
  mode only*. `restir_light.comp` emits candidate reservoirs with m = 1/N_L.
- **2.2 Merge LRM into pixel reservoirs** in `restir_camera.comp`; caustic/non-caustic
  split on x₂ material; caustic reservoir populated but temporally naive at first.
- **2.3 Reverse hybrid shift** for non-caustic t=1 in temporal + spatial passes;
  V-buffer rejection heuristic. *Test:* glass-bulb scene — lightweight mode (s≤1,t=1)
  resolves filament lighting that ReSTIR-PT-lite can't; soak test again.
- Milestone: **LW BDPT mode** from the paper, a shippable quality tier.

### Phase 3 — Caustic reservoirs

> **Phase 3 status: DONE.** restir_caustic.glsl (pure-replay of a caustic
> path under an explicit camera: dst-anchored tree descent, classification
> checks, deterministic camera connection + landing-pixel projection),
> restir_caustic_shift.comp (per PREVIOUS pixel: shift the caustic history
> into the current frame, Eq. 53 replay-ratio Jacobian, re-bin via the LRM
> linked list - cleared a second time after restir_camera consumed the
> light-pass entries), restir_caustic_merge.comp (per pixel: canonical +
> landed entries under a 2-source generalized balance heuristic - each
> caustic path has exactly ONE possible history source, the prev pixel its
> inverse replay lands in, so the canonical side does one backward replay
> through the prev camera to find it; support checks mirrored in both
> directions), proxy confidence `c := min(c_can + c_mv, 20)` from the
> motion-vector-mapped prev pixel (sample-independent), debug view 9
> (caustic confidence). Gated on restirTemporal && restirLightTracing.
>
> Soak results (fallback Cornell+duck, locked camera): accumulated means
> unchanged vs Phase 2 (temporal 0.116467 vs 0.116466; both 0.116436 vs
> 0.116431-0.116434) - no energy gain/loss. Caustic reservoirs accumulate:
> confidence saturates at the cap (debug-9 p50 = 20/20), caustic-path
> occupancy rises >10x (view-7 p99: 0 -> 6.4e-5), max caustic W drops
> 18.0 -> 3.3 (firefly energy spread over accumulated reservoirs). The
> per-frame estimate (accumulation off) drops relNoise 0.306 -> 0.0995
> with matching means. The orbiting-camera sharpening test needs manual
> driving (soaks lock the camera); motion robustness rests on
> shift-failure => zero contribution, which is bias-safe.

- **3.1 Temporal caustic replay + re-binning** (pass 7): replay previous frame's
  caustic paths, project, LRM re-bin, merge with Eq. 51–52 weights.
- **3.2 Proxy confidence** `c_i += c_v` via motion vectors, cap 20.
- *Test:* glass egg / Cornell caustic scene: caustics sharpen over frames while
  orbiting camera slowly; no energy gain/loss in soak test; toggling caustic
  reservoirs off shows the paper's Fig. 8-style degradation.

### Phase 4 — Full BDPT: s ≥ 2 vertex connections

> **Phase 4 status: DONE.** Compacted global LVC without new bindings: the
> classic lightVerts buffer (binding 8) reused as a flat atomic-append array
> in ReSTIR mode with the allocator in lightVertCount[0] (cleared per frame)
> and ReSTIR-specific field semantics — tputMat.xyz = NUMERATOR fLightNum
> (RIS needs the true f for the shared target p̂; storing classic throughput
> would leak sampling pdfs into the target), tputMat.w = matId<<8|pathLength,
> wiLen.w = light-prefix pdf pLight, posDvcm.w/normalDvc.w = classic
> dVCM/dVC. restir_camera.comp draws ONE uniform LVC pick per eye vertex
> (RNG_CONNECT; p_L = N_L/|LVC| in the candidate pdf; the pick subsamples
> subpath AND technique index, unbiased like bdpt_eye's nValid trick).
>
> MIS: both passes now carry the FULL classic dVCM/dVC recursion alongside
> the Phase 2 restrictions (dT1 eye / dLW light); the `restirConnections`
> toggle (+ `ROYALGL_RESTIR_CONN`) selects which set weights every
> technique, so the partition of unity tracks the active technique set in
> all four toggle combinations (lightTracing kill switch = zero dVCM seed).
>
> Shifts: s≥2 winners whose camera prefix has a reconnection vertex need NO
> new code — the cached L_suf transparently covers the (fixed) connection
> edge + light subpath. Winners without a connectable camera pair carry
> RESTIR_FLAG_LIGHTRC + the cached light subpath end (lyPosMat/lyNormal/
> lyTput = rho_y·fLightNum): the shift replays the whole prefix and
> re-evaluates the connection edge — pure f change, NO Eq. 55 Jacobian,
> because y_{s-1} is parametrized by light-side solid angle the shift never
> touches (unlike the s=1 case, where the light point is area-sampled and
> counted in solid angle at the eye vertex). Deviation from the paper: the
> light side is CACHED, not replayed, in s≥2 shifts — exact for static
> scenes (identity-on-light-side is a valid GRIS shift), cheaper, and it
> survives temporal light-tree-anchor changes; revisit if animation lands.
>
> Soak results (fallback Cornell+duck, locked camera, RIS-only unless
> noted): conn OFF reproduces Phase 2 exactly (0.116468); full BDPT
> 0.116472; conn ON + light OFF 0.116480 (≈ no-rejection baseline
> 0.116481) — all technique-set combinations conserve energy. Reuse:
> temporal 0.116472 (= RIS, bias-free static shifts), spatial 0.116448 /
> both 0.116443 (the same −0.02% copied-ω_τ darkening as Phases 2–3,
> Phase 5's target). Technique debug view confirms s≥2 reservoir winners
> (small share — the Cornell scene is direct-light dominated; a
> Veach-Bidir-style scene would exercise them harder and is still worth
> adding to assets).

- **4.1 Compacted global LVC** (atomic append, uniform selection, p_L = N_L/|LVC|)
  with per-vertex dP/dVC + replay metadata.
- **4.2 s≥2 candidates in initial RIS** (Eq. 25 UCW incl. 1/p_L); connection-edge
  reconnection Jacobian (Eq. 55 note).
- **4.3 Shift support:** light-subpath replay inside `ShiftPath`, double reconnection
  case (x_r then y_{s-1}). *Test:* Veach-Bidir-style scene; compare against plain
  full BDPT accumulation.

### Phase 5 — Unbiased MIS: recursive reconnection MIS

> **Phase 5 status: DONE (machinery), default OFF (measured).**
>
> **Implementation.** In our two-material world the paper's per-suffix cache
> (γ̄, λ̄^VC, λ̄^P, σ̄) collapses further: Lambertian forward pdfs depend only
> on the outgoing direction, so every suffix pdf is fixed under a shift and
> the technique-MIS denominator of the shifted path is AFFINE in the prefix
> state at the reconnection vertex:
> `denom' = C0 + C1·(dVCM'_r + p⃖'_r·dVC'_r)` with `p⃖'_r = rcCos'/π`.
> restir_camera.comp maintains the affine pair (misK1/misK2) incrementally
> from x_r onward (the paper's per-bounce updates) and folds each
> candidate's fixed terms into `misCache` (C0, C1) — with two special
> layouts: rc = terminal emitter (s=0, r=t−1: directPdfA/emissionPdfW,
> no p⃖ pairing) and rc = NEE light point (full split-vertex recompute).
> LIGHTRC caches the light side's ratio sum. Pure-replay paths (specular
> s=0 chains, t=1, caustics) recompute ω by carrying the dVCM/dVC/dT1(/dLW)
> recursions along the replay — restir_shift.glsl seeds them from the
> DESTINATION anchor and camera. All ReSTIR MIS quantities switched from
> the camera-anchored tree eval pdf to the power-CDF eval
> (`RestirLightPdfs`, binding 10) so recomputed ω is a frame-independent
> function of the path (required for exact temporal partitions under
> motion; costs weight optimality only).
>
> **Validation & the surprise.** Camera-side recompute is EXACT: temporal /
> spatial / temporal+spatial soaks without light tracing all sit on the RIS
> baseline (0.116481/0.116486/0.116488 vs 0.116480) in both MIS modes, and
> temporal identity shifts reproduce stored ω (0.116472 = RIS with light).
> But the residual spatial darkening that Phases 2–4 attributed to copied
> ω_τ turned out NOT to be: spatial+light gives 0.116449 in BOTH modes
> (copied ω is near shift-invariant for Lambert — no Fig. 5 effect exists
> to fix in this scene). The true source is the **t=1 re-anchoring
> approximation** (we collapse free-landing x₁ onto the V-buffer anchor
> with a point Jacobian instead of the paper's continuous landing + pixel
> filter h_i): −0.023% under spatial, compounding to −0.026% under
> temporal+spatial (copied), and recompute mode AMPLIFIES it to −0.048%
> (deterministic, reproducible) because ω evaluated at the anchor
> re-weights exactly the paths the approximation mis-measures — the t=1
> reverse shift therefore always copies ω (code comment in
> restir_shift.glsl), caustic shifts recompute (pure replay = exact
> identity).
>
> **Decision:** `restirRecomputeMis` defaults to false; flip it when GGX
> lands (glossy reconnections are where copied ω actually darkens corners,
> Fig. 5) and revisit the t=1 machinery with a proper h_i-filter treatment
> at the same time. Env override `ROYALGL_RESTIR_MISFIX`.

- **5.1 Cache γ̄, λ̄^VC, λ̄^P, σ̄, geometry ratio** during camera subpath sampling
  (incremental updates per bounce as in the paper's supplemental Python).
- **5.2 Recompute ω_τ at shift time** via Eq. 46/47 (r = t−1, r = t−2, and general
  cases). Toggle: copied vs recomputed. *Test:* reproduce Fig. 5 — biased-vs-unbiased
  difference image shows the corner darkening disappearing; soak test converges to
  reference within noise.

### Phase 6 — Performance & polish

> **Phase 6 status: instrumentation + first wins DONE.**
>
> Per-pass GL timers now cover the whole ReSTIR frame graph
> (`ROYALGL_STATS=1` logs avg ms per pass every 128 frames; slots:
> gbuffer/light/camera/temporal/caustic-shift/caustic-merge/spatial/
> resolve). Profile — **RTX 5090** (GL_RENDERER confirms the context lands
> on it via main.cpp's NvOptimusEnablement export; earlier "Intel UHD dev
> GPU" references in this doc described an assumption, not this machine),
> 1600×900 window, all reuse on, full BDPT: **spatial 51-60 ms (≈60%)**,
> temporal 18-19 ms, camera 10-12 ms, caustic-shift 4-5 ms, light 2-3 ms,
> gbuffer+merge+resolve ≈1 ms; ~86-110 ms total (spread is boost-clock
> behavior). These numbers are expected for GL compute: no RT cores are
> reachable from GLSL, so every shift evaluation walks the BVH2 in
> software — the spatial pass performs 6 full-frame shift evaluations
> (3 neighbors × forward+backward at ~8.5 ms each), so the neighbor-count
> slider IS the perf dial.
>
> Applied: PathReservoir trimmed 208→160 B (dropped the three unused
> reserved vec4s; PixelReservoirs 272→224 B) — ~18% less reservoir
> bandwidth in every reuse pass and ~290 MB saved at 1080p; soaks
> unchanged (RIS 0.116473, temporal+spatial 0.116431).
>
> **Occupancy round (Nsight-driven):** profiling showed ≤9% theoretical /
> ~2% achieved occupancy — the megakernels kept whole 160 B PathReservoir
> structs live in registers across BVH traversals, and the 64-entry
> traversal stacks burned 256 B of per-thread local memory. Fixes:
> (1) shift kernels take a buffer SLOT and read base-reservoir fields on
> demand (`RS_BASE` in restir_shift.glsl); the spatial aggregate and the
> camera pass's RIS winner live in their output buffer slot (`SEL`),
> written field-wise on wins — stale unrelated fields are intentional and
> guarded by the tech flags; (2) BVH stacks 64→32 entries
> (`BVH_STACK_SIZE`, fine to ~2*log2(N) depth — bump for multi-million-tri
> scenes). Result at 1600×900, all reuse + full BDPT on the RTX 5090:
> **101.6 → 24 ms** (~4.2×): spatial 60→13.9, temporal 19→3.6, camera
> 12→3.1, caustic-shift 5.4→1.5, light 3.4→1.0. Soak means unchanged
> (RIS 0.116473, temporal 0.116473, spatial 0.116447).
>
> **Debug tooling:** `ROYALGL_GL_DEBUG=1` = debug GL context + KHR_debug
> message callback; object labels on every buffer/program and
> glPushDebugGroup markers around every pass are always on, so Nsight /
> RenderDoc captures show a named frame graph.
>
> Deferred: splitting the shift mega-kernel by technique class (the
> paper's known divergence win — the next lever if Nsight still shows
> divergence-bound warps), fp16 packing of normals/throughputs, workgroup
> size tuning (8x8 kept; retest once register counts settle), the
> resolution-scale slider, a Vulkan/RT-core backend as the ultimate
> answer to the software-traversal cost, and the "spatial-only offline
> converge" preset (already reachable via the existing temporal/spatial
> toggles — temporal correlation shows up in soaks as slower
> accumulated-relNoise decay). The N_L slider shipped later (see the
> editor-settings round after Phase 10.3): RenderSettings::
> restirLightPaths (env ROYALGL_RESTIR_NL, UI log-slider 4096-262144),
> PathTracer::EnsureLightPathBuffers reallocates the LVC/count/LRM
> buffers live and Reset()s; N_L=65536 soaks unbiased (0.09158).
> The same round promoted every remaining env-only setting to the UI:
> volumetric shift mode combo, fog-parallax pairing, stratified spatial
> picks, and caustic temporal reuse (moved from PathTracer's
> m_causticPassesEnabled into RenderSettings::restirCausticReuse; the
> ROYALGL_RESTIR_CAUSTIC env now sets the setting in Application).

- Profile with existing `ROYALGL_STATS=1` GL timer queries per pass.
- Reservoir packing (fp16 normals/throughputs where safe), register pressure in the
  shift kernel (consider splitting caustic vs non-caustic shifts into separate
  dispatches — the paper lists this as a known win).
- Progressive accumulation when camera static (ReSTIR frame estimates averaged after
  reservoirs warm up; note temporal correlation — offer spatial-only offline mode
  like the paper's Fig. 12).
- Optional: lens-mode ReSTIR investigation (Area ReSTIR), GGX materials (real
  roughness threshold 0.08 semantics kick in here).

### Phase 7 — Wavefront camera RIS + shift passes

> **Phase 7 status: DONE.** The camera RIS pass and the temporal/spatial
> replay passes are converted to a wavefront architecture with streaming
> compaction (`shaders/restir_wf_*.comp|glsl`); the megakernels remain as
> the A/B + low-binding fallback, selected per pass by
> `ROYALGL_RESTIR_WAVEFRONT` (0 = all megakernel, 1/unset = all wavefront,
> else bitmask: bit0 camera, bit1 temporal, bit2 spatial).
>
> **Architecture.** Every ray cast is hoisted out of the bookkeeping
> kernels so BVH traversal stacks never share registers with RIS/shift
> state: camera = caminit → [camshade → camtrace + camshadow] × maxLen →
> camfinal; shifts (one generic "job" = one RestirShiftPath evaluation,
> shared by temporal and spatial) = tinit/sinit → [shiftstep → shifttrace
> + shiftshadow] × maxLen → tmerge/smerge. Work-item queues are compacted
> by atomic append; dispatches are GPU-driven (glDispatchComputeIndirect
> reads the appenders' counters — no readbacks; appenders maintain the
> group count with atomicMax). Candidate visibility is resolved by lean
> any-hit kernels; the RIS acceptance draw is DEFERRED one round so the
> per-pixel RestirRand order is exactly the megakernel's — the wavefront
> estimator is the same estimator (all path sampling replays the
> counter-based streams; only the RIS-chain RNG state is persisted).
> Buffers: bindings 16 (arena: 26 vec4/pixel, camera state and shift jobs
> alias it) and 17 (queues, with the 32 dispatch-ctrl entries in the first
> 512 bytes — NVIDIA caps compute shaders at 16 storage BLOCKS and the
> tracing kernels' include graph already sits at that limit, hence two
> blocks and the non-tracing kernels dropping light_tree/bdpt_common).
>
> **Validation** (fallback Cornell+duck, locked camera, same session):
> RIS-only wavefront = megakernel EXACTLY (0.116472 both); temporal-only
> 0.116468, spatial-only 0.116447 (= megakernel), temporal+spatial
> 0.116437-0.116450 vs megakernel 0.116436 — all within short-soak noise.
>
> **Perf** (RTX 5090, 1600×900, all-on, same-session A/B): spatial
> 21.4 → 10.0 ms (−53%), temporal 5.4 → 3.7 ms (−30%), camera 4.0 → 8.1 ms
> (regression: the per-round barriers + arena state round-trips outweigh
> the occupancy win on short coherent Cornell paths — the megakernel
> camera was already fast after the Phase 6 occupancy round). Totals:
> 34.5 → 22.8 ms (mask 7), 21.9 ms (mask 6 = megakernel camera + wavefront
> shifts, current best). Subgroup-aggregated queue appends measured no
> better than plain atomics (reverted — state traffic and round barriers
> dominate, not counter contention). Expect the wavefront camera pass to
> win on scenes with longer/incoherent paths (glass-heavy, Veach-style);
> re-evaluate the default mask then.
>
> **Post-Phase-7 robustness round** (grazing-angle transients): the
> confidence (M) cap is now a live editor setting
> (`RenderSettings::restirConfidenceCap`, slider 1–64, uploaded in
> prevCamPos.w, `RestirConfidenceCap()` in restir_common.glsl) — it
> directly controls how many frames an outlier/stale sample persists.
> Temporal history validation gained the normal-agreement test the
> spatial pass already had (same-material different-orientation surfaces
> passed the 2%-of-depth position test at silhouettes/grazing walls);
> the caustic proxy-confidence lookup mirrors it. All four reuse merges
> (megakernel + wavefront) now apply a SYMMETRIC Jacobian support
> restriction `J ∈ [1/10, 10]` (`RestirJacobianValid`) in both the
> candidate weight and the backward MIS term — unbiased (forward and
> inverse Jacobians are reciprocals, so both directions restrict the
> same shift map), and it kills the grazing-angle reconnection-Jacobian
> explosions that showed up as short-lived bright blotches on
> newly-appearing sharp-angle surfaces. Temporal validation also gained a
> depth-level test (the spatial pass's 10% relative-depth heuristic, made
> camera-motion-aware: gPrev.w is measured from the PREVIOUS camera, so it
> is compared against the current point's distance to that camera - a
> naive |dPrev-dCur| would reject all history during a dolly). With the
> 2%-of-depth world-position test this is mostly belt-and-braces (the
> position test implies <=2% depth agreement); it fires independently
> under extreme camera motion and guards any future relaxation of the
> position test. Soaks unchanged (WF 0.116423-0.116444 / MK
> 0.116430-0.116461, within the both-config band).
>
> **Anchored t=1 candidates (the actual fix for the grazing W blow-up).**
> The J-guard alone did not eliminate the transient: the ROOT CAUSE was a
> mismatched target function, not the Jacobian. Non-caustic t=1 candidates
> were created with f/pHat evaluated at the FREE landing point y_{s-1},
> while every shift evaluation (forward and backward) re-anchors to the
> V-buffer pixel center. At grazing incidence f varies by orders of
> magnitude across one pixel footprint (imageToSurface ~ cosToCam/
> cos^3AtCam/d^2 plus the reconnection geometry), so the temporal balance
> heuristic compared a free-point numerator against an anchored-point
> denominator - the partition of unity broke on exactly the BRIGHT
> candidates (f_free >> f_anchored => m_A ~ 1 while the history side also
> claims the path family), W inflated, inherited confidence ~cap in one
> merge, and decayed over ~cap frames. Camera-side techniques are anchored
> from birth, which is why the artifact needed light tracing. Fix
> (restir_light.comp): non-caustic t=1 candidates are ANCHORED AT
> CREATION - the free landing only selects the pixel; the candidate is
> built by the same Eq. 56 reverse-hybrid reconnection every shift
> performs (J_creation = (cosRcA/cosIn)(d2free/d2A) prices the collapse),
> with omega recomputed at the anchored geometry via the classic recursion
> across the replaced scatter (the formulas the shift's Phase 5 comment
> documented as the "future fix"). The old LrmReconnectionValid heuristic
> is subsumed (the creation reconnection IS that test), a static-camera
> temporal shift is now an exact identity (J = 1), and insertion/shift
> target functions agree everywhere. Caustic candidates stay free-landing
> (pure replay was always self-consistent).
>
> Two further t=1-gated Jacobian channels needed the same symmetric
> guard: (1) the CREATION collapse Jacobian itself - at camera-grazing
> pixels the footprint is huge, the free landing can be far from the
> anchor, and (cosRcA/cosIn)(d2free/d2A) reaches ~1e5, minting a huge-W
> reservoir at birth (RestirJacobianValid now rejects those candidates -
> the grazing analog of the paper's stability heuristic; costs -0.009%
> on the Cornell soak); (2) the CAUSTIC replay Jacobian - the light-tree
> descent is camera-anchored, so during camera motion the same seed can
> descend to a different light and the full-replay pdf ratio is
> arbitrary; restir_caustic_shift.comp rejects out-of-range forward
> shifts and restir_caustic_merge.comp mirrors the test on the backward
> map.
>
> **Stale copied omega was the deepest of the transient channels:
> `restirRecomputeMis` now defaults ON and covers t=1 shifts too.** With
> light tracing enabled, EVERY technique's omega carries the t=1
> competitor seed N_L*d^2/cosIn1 - violently anchor-dependent at grazing
> incidence. Copied omega is exact under identity shifts (the Phase 5
> static validation) but under camera motion the history's omega is
> stale w.r.t. the new anchor while the fresh candidate's is current:
> pHat = omega*lum(f) stops being a function of the path, the temporal
> balance-heuristic partition breaks by the omega ratio (orders of
> magnitude at grazing), and exactly the bright side gets over-counted -
> the last piece of the "W blows up on newly-visible sharp-angle
> surfaces, decays over ~cap frames, only with t=1 enabled" transient.
> The t=1 reverse shift now RECOMPUTES omega at the destination anchor
> (both restir_shift.glsl and the wavefront shiftstep, which regained
> the light-side MIS recursion in job slot 5); the old reason to copy -
> recompute amplifying the free-landing re-anchoring approximation - is
> obsolete since candidates are anchored at creation. Rocking-orbit A/B
> (ROYALGL_ORBIT rocks the camera; per-frame estimate stats): temporal-
> only hot pixels (lum>30) with copied omega disappear with recompute.
> Recompute-mode reuse baselines: T+S 0.116472-0.116489,
> lightweight-set T+S 0.116513.
>
> **Anchor-distance confidence fade + new defaults.** The residual t=1
> transient tracks reuse between spatially DISTANT anchors (user
> observation): along a grazing surface the t=1 target function varies
> rapidly with the anchor, so chained merges between far-apart (but
> accepted) anchors random-walk W into a bright transient even with
> exact omega/Jacobians - unbiased, but heavy-tailed. Temporal history
> confidence is now scaled by 1 - sep/(0.02*depth) (the acceptance
> radius), a sample-independent G-buffer function: the MIS partition
> stays exact, a static camera reproduces old soaks bit-for-bit
> (fade=1), and far-anchor history decays instead of compounding.
> Defaults changed to ship the robust configuration: enableRestir ON,
> spatial neighbors 1, confidence cap 8 (pure-default soaks: WF
> 0.116535 / MK 0.116531). NEW SOAK BASELINES (the
> estimator semantics changed: t=1 is now point-sampled at the pixel
> center like every other technique, and the old double-gating - free
> visibility AND anchor reconnectability - no longer drops penumbra
> energy): RIS 0.116556, temporal 0.116556 (= RIS exactly, identity
> shifts), spatial 0.116530, temporal+spatial 0.116513 (WF) / 0.116524
> (MK). The pre-existing ~-0.02% spatial drift persists unchanged -
> confirming it was never this mechanism (Phase 5 notes).

### Phase 8 — Megakernel removal + histogram-stratified spatial reuse

> **Phase 8 status: DONE.**
>
> **Megakernel removal.** The wavefront path is now the ONLY ReSTIR
> pipeline: `restir_camera/temporal/spatial.comp`, the orphaned
> `restir_shift.glsl`, the unidirectional `pathtrace.comp`, the
> `ROYALGL_RESTIR_WAVEFRONT` mask, and the `enableBidir`/`enableNEE`
> settings (+ `ROYALGL_BIDIR/NEE`) are gone. Non-ReSTIR frames always
> render the three-pass BDPT (also the lens-mode fallback and the
> unbiased reference); machines with <18 SSBO bindings fall back to it
> too. Post-removal soak matches the previous default band (0.116465).
>
> **Histogram-stratified spatial reuse** (Salaün et al., "Histogram
> Stratification for Spatio-Temporal Reservoir Sampling", SIGGRAPH '25)
> replaces the uniform-disk neighbor selection:
> - `restir_wf_ssort.comp` (one 256-thread workgroup per 16x16 block,
>   grid origin jittered per frame): shared-memory bitonic sort of the
>   block's post-temporal candidates by (cluster key, pHat). The sorted
>   list is the inverse CDF of the block's luminance histogram (Heitz &
>   Belcour 2019); cluster key = primary-hit instance+material is the
>   paper's object-id spatial mask. Output goes to the spare arena
>   region [25N,26N) - no new SSBO binding, the ray-tracing kernels
>   stay at NVIDIA's 16-storage-block limit.
> - `sinit`: per pixel ONE scrambled-Sobol draw per frame (van der
>   Corput over the frame counter, per-pixel XOR scramble) fans into K
>   antithetic stratified positions pos_k = k even ? k/K+u : (k+1)/K-u
>   (u in [0,1/K)) - one per stratum, pairwise summing to 1 (paper
>   fig. 3) - which index the pixel's cluster run in the sorted list.
> - **Merge had to become LINEAR over rounds**: the K rounds share one
>   u, so which candidate a round sees correlates with the sorted
>   VALUES (round 0 = dimmest stratum). Chaining pair-merges against a
>   running aggregate under that correlation measured 4% DARK
>   (0.111474); selection-vs-value conditioning breaks the chained-GRIS
>   independence assumption. `smerge` now builds, per round, the
>   2-technique pairwise-MIS estimator E_k = (FROZEN canonical vs
>   candidate) - identical math to the old chained step with the
>   canonical substituted for the aggregate - and mixes the K rounds
>   with constant weight 1/K by streaming WRS (outer weight v_k =
>   wSum_k/K; the winner's pHat cancels against its UCW, so W = V/pHat
>   stays in final form after every round, no finalization pass).
>   Linearity makes value-correlated selection harmless: each E_k's
>   expectation only involves its round's marginally-uniform pick.
>   Self-picks fold in the canonical EXACTLY (a reservoir pair-merged
>   with itself is the identity - confidences cancel), so 1-pixel
>   clusters are no-ops instead of W inflations.
> - Defaults: spatial candidates 4 (paper), block 16x16 (paper's best
>   trade-off), confidence cap unchanged (8). `ROYALGL_RESTIR_SPATIAL_NBRS`
>   and `ROYALGL_RESTIR_STRAT=0` (uniform-random picks from the same
>   clustered pool) exist for scripted A/B.
> - Soaks: strat + rand + K=1 + spatial-off all in band
>   (0.116464-0.116521); conductor 0.116615 (PT 0.11674); moving duck
>   per-frame relNoise 0.0905 hot=0 (recorded band was ~0.12); orbit
>   hot=0; plain BDPT 0.116591. K=4 accumulated relNoise 0.0322 @1280
>   vs K=1 0.0357 @448. Antithetic-vs-random measured a wash ON THIS
>   SCENE (0.03249 vs 0.03251 @1088; Cornell blocks are locally
>   luminance-uniform, so the sorted list is nearly flat - the paper's
>   gains come from scenes with strong local variation). Spatial pass
>   15.5 ms at 1600x900 for 4 candidates incl. the sort (~0.3 ms).

### Phase 9 — ReSTIR PT Enhanced adoptions (Lin et al. 2026)

> **Phase 9 status: DONE.** From "ReSTIR PT Enhanced: Algorithmic
> Advances for Faster and More Robust ReSTIR Path Tracing":
>
> **Adopted.**
> - **Footprint reconnection criteria (sec. 4)** replace the
>   material-class-only rc-pair scan: dual ray-footprint thresholds at
>   a fixed fraction (c=0.02) of the pixel's primary footprint
>   (RestirRcFootprintOk, restir_common.glsl) + a single-vertex
>   roughness threshold (0.2) folded into prevConnectable. The inverse
>   footprint uses the GGX peak-pdf proxy pi*alpha^2*d^2 >= T so it is
>   known before the scatter at x_k is sampled (deviation from the
>   paper, which defers the decision; exact for diffuse/emitter x_k
>   per their footnote 6). Bijectivity: creation gates the pair in the
>   source domain; shiftstep re-evaluates the SAME criteria on the
>   offset path (job slot 10 carries pdf/eligibility/threshold) - an
>   earlier offset pair passing, or the reconnection/terminal-emitter
>   pair failing, rejects the shift. Light-point and connection-vertex
>   rc stay ungated fallbacks on both sides (which is also the paper's
>   sec. 6.2.3 forced light reconnection - we already had it).
> - **Duplication-map decorrelation (sec. 5)**: restir_wf_dupmap.comp
>   counts same-seed (fSeed.w) reservoirs in 17x17 at END of frame ->
>   D in the persistent arena slot [26N,27N).w; next frame's tmerge
>   caps history confidence at lerp(cCap, 1, D^0.1). Slightly biased
>   by design; ROYALGL_RESTIR_DECORR=0 restores exactness (soaks:
>   static Cornell shows NO measurable bias, 0.116473 vs 0.116478).
> - **Color noise reduction (sec. 6.3)**: smerge accumulates VECTOR
>   resampling weights m*omega*F*W*J into [26N,27N).xyz (1/K mixture);
>   resolve shades that marginalized sum instead of the winner-only
>   omega*f*W whenever spatial reuse ran. Scalar weights still drive
>   the reservoirs (shading decoupled from resampling). Per-frame
>   relNoise 0.093 -> 0.084 at HALF the spatial candidates.
> - **Unified DI+GI (sec. 6.1)**: verified already inherent - the BDPT
>   technique set (s=0 emitter hits, s=1 NEE incl. the primary vertex,
>   t=1, s>=2) has always fed one reservoir; no separate DI pass
>   exists to unify.
>
> **Not adopted.** Paired/reciprocal spatial reuse (sec. 3) is
> mutually exclusive with the Phase 8 histogram-stratified selection
> (pairing textures would replace the sorted-cluster draw); stream
> compaction (sec. 6.2.2) is the wavefront architecture we already
> run; dual motion vectors (sec. 6.4) need a motion-vector channel the
> G-buffer doesn't carry yet (future work); reservoir compression and
> presampled light tiles don't pay off at our reservoir layout / light
> counts.
>
> Soaks (decorr off): default 0.116473-0.116479; conductor ReSTIR
> 0.116619 vs same-session BDPT 0.116693 (-0.06%); layered 0.117808
> (band 0.11778); moving duck per-frame relNoise 0.0806 hot=0 (was
> 0.0905 pre-criteria). All-on GPU total 31.1 ms at 1600x900 (spatial
> 10.3, temporal 5.2, camera 10.2) - the criteria cost nothing
> measurable and kill bad corner/glossy reconnections earlier.

### Phase 10 — Volumetric ReSTIR BDPT (homogeneous media)

> **Phase 10 status: DONE (airlight estimator redesigned in the 10.1
> round below).** Full writeup with derivations, tables and figures:
> **docs/volumetric_restir_paper.pdf** (6 pages). Summary: homogeneous
> fog (analytic Beer-Lambert + exponential free-flight + HG phase;
> ROYALGL_FOG="sS,sA,g") through the whole pipeline - BDPT reference
> walks, ReSTIR candidate walks (camera + light), LVC volume vertices,
> and the shifts. MIS convention: per-vertex cos -> sigma_t at volume
> vertices, transmittance excluded from MIS ratios (valid partition,
> approximate optimality); BDPT walks apply the distance pdf's 1/sigma_t
> at ARRIVAL (albedo-at-scatter dims every volume-vertex connection by
> sigma_t - the first bring-up bug). NEW SHIFT MAPPING: volume vertices
> as reconnection anchors - fixed world-space point, volume-measure
> Jacobian d^2/d'^2 (NO cosines: immune to grazing blow-ups), live Tr +
> phase re-eval, phase-peak footprint criteria, and an 8-bit per-vertex
> volume mask (tech word repacked s:4|t:4) that replay re-derives for
> invertibility. ROYALGL_RESTIR_VOLMODE: 0 = naive ReSTIR PT port
> (volume paths unshiftable), 1 = replay, 2 = anchored (default).
> Anchored == replay in HOMOGENEOUS media (exponential distance replay
> is exact) - the anchor's Jacobian is distance-density-free, which is
> what heterogeneous media will need.

### Phase 10.1 — The airlight estimator fix (the ω=1 design was wrong)

> **Status: DONE.** The original Phase 10 design routed camera-segment
> in-scatter (airlight) EXCLUSIVELY through light tracing at unit MIS
> weight ("V-buffer anchoring means no camera competitor exists"). That
> partition is valid - and the estimator it induces has INFINITE
> VARIANCE: the t=1 camera-connection factor grows as 1/d² as the
> in-scatter vertex approaches the camera, and with ω = 1 nothing damps
> it (∫(1/d⁴)·d²dd diverges; classic BDPT is immune because its eye walk
> also samples primary-segment in-scatter and MIS drives ω → 0 as
> d → 0). Symptoms: correct means but accumulation that never visibly
> converges (relNoise 0.45 @ 300 accumulated spp vs BDPT's 0.065; the
> accumulated max decayed exactly as 1/N - single mega-spike events),
> median pixel 12% dark, hot pixels persisting. 42% of the image energy
> rode on that channel. The paper's original Table 3 numbers (per-frame
> relNoise 0.68/0.91) were real but the "accumulation launders it"
> reading was wrong - the user-visible failure.
>
> **Fix (two halves, both required):**
> 1. **Camera-side airlight family (truncated split).** The camera pass
>    runs a SECOND wavefront walk per pixel (restir_wf_caminit.comp,
>    uniform uWfVolWalk=1; the host re-clears the queue ctrl and re-runs
>    the shade/trace/shadow rounds before camfinal). Its first vertex is
>    a volume in-scatter point on the primary segment sampled from the
>    TRUNCATED free-flight pdf sigma_t·Tr(t)/(1−Tr(d₁)) - it always
>    scatters, so the surface family keeps its deterministic
>    probability-1 anchor (fog-off behavior bit-identical, zero
>    classification-failure band at the primary segment). The walk
>    continues through the normal candidate blocks (NEE / s≥2 / s=0 at
>    and after the volume vertex) on a derived seed (volSeed, stored in
>    fSeed.w) and continues the same per-pixel RIS chain; volMask bit 0
>    marks the family. Shifts replay the truncated draw under the
>    destination ray and depth (WfShiftCreateJob's volume-primary
>    branch) - the paper's "distance-sliding re-anchoring", now shipped;
>    the truncated pdfs enter both replay-pdf products so the Eq. 53
>    ratio prices the src/dst depth difference and identity shifts stay
>    J = 1. MIS seeds mirror bdpt_eye's volume branch with the
>    truncation folded in: dVCM = (N_L/i2solid)·(t²/σt)·(1−Tr(d₁)).
> 2. **Real MIS weight on volume t=1.** Airlight candidates keep the
>    caustic-class free-landing reuse but their ω is now the true BDPT
>    weight against the truncated camera family: wLight = i2solid·σt /
>    (d²·(1−Tr(d_pix))·N_L) · (dVCM + p_rev·dVC | lightweight dLW form),
>    at creation (restir_light.comp, landing-pixel depth from the
>    G-buffer it already loads) and at replay (restir_caustic.glsl, new
>    gbufBase parameter: current G-buffer for the forward shift, prev
>    for the merge's backward map). ω ~ d² as d → 0 - singularity gone.
>
> **Bonus: the miss-pixel airlight limitation is LIFTED.** For a
> primary miss d₁ = ∞ ⇒ pScat = 1 (plain exponential); caminit's walk-2
> runs for miss pixels, camfinal finalizes them, t=1 landings on miss
> pixels are accepted, and the resolve shades their reservoirs (RIS-only
> - the temporal/spatial passes still skip anchor-less pixels; sinit
> round 0 copies the canonical for them instead of zeroing). Exterior
> fog views now match the reference (0.06572 vs BDPT 0.06568).
>
> **Third bug, caught by the temporal identity invariant** (temporal
> mean == RIS mean at a locked camera - the sharpest bias detector in
> the ladder; bisected with LIGHT/CONN/CAUSTIC/MISFIX toggles): the
> camshade s≥2 at-rc misCache cached cosLightV/d² where the shift's
> recompute pairing needs kLight/d² - for a VOLUME LVC end kLight = σt,
> not the unit cosine, so recomputed ω disagreed with creation's and the
> temporal partition broke (+0.17%, connections-mode only, fog only).
>
> **Validation (interior camera "0.85,1.35,1.55,-0.45,0.5,-0.9", fog
> 0.15,0.02,0.3, decorr off):** BDPT truth 0.09154. ReSTIR RIS-only
> 0.09156, temporal-only 0.09156 (= RIS: identity invariant restored),
> spatial-only 0.09156, default T+S 0.09160-0.09161 (+0.07% - the same
> temporal×spatial compounding class as the surface pipeline's
> documented −0.02...−0.04%), mode 1 0.09159, mode 0 0.09138. Pure
> absorption 0.08608 vs BDPT 0.08604. Fog-off default 0.116472 (in
> band). Per-frame, all reuse on: naive port relNoise 0.672 / median
> −27% / hot 0-1; replay AND anchored 0.148 / median −3% / hot 0 (the
> honest tie stands) - 4.5× less per-frame noise than the naive port
> and, more importantly, accumulation now works: default fog relNoise
> 0.043 @ 160 accumulated spp, faster per sample than the BDPT
> reference (0.065 @ 448). Orbit and moving-duck runs: hot 0-1.
>
> **Cost (the honest number):** fog all-on 71 ms at 1600×900 vs 38 ms
> surface-only (camera pass 14 → 38 ms: the airlight walk's paths never
> die early, so it's ~a full extra walk). Replayable Russian roulette
> on the volume walk is the obvious next lever; correctness first.

### Phase 10.2 — Miss-pixel reuse + motion-stable light pick

> **Status: DONE.** Two user-visible gaps in the 10.1 state:
> anchor-less (primary-miss) pixels sampled airlight but never reused
> it (RIS-only → the fog around the box stayed per-frame noisy), and
> fog noise rose under camera motion, most visibly in the bright glow
> near the lights.
>
> **Miss-pixel temporal reuse.** tinit's miss branch now looks up
> history by reprojecting a representative fog point (one mean free
> path 1/σ_t along the pixel ray) through the previous camera and
> reuses MISS-TO-MISS only; the airlight family's truncated replay
> (volMask bit 0) needs no surface anchor - WfShiftCreateJob rejects
> every surface-anchored class on a miss destination (new guards) and
> gives miss destinations an unpassable rc-footprint threshold, so the
> offset scan can never reconnect there (symmetric with creation,
> which never sets rc on miss pixels). The caustic passes mirror it:
> the shift keeps miss landings, the merge runs on miss pixels with
> the same fog-representative proxy-confidence reprojection
> (miss-to-miss validated). Exterior per-frame relNoise 0.545 → 0.188
> with temporal on (2.9×), means identical (0.0657); the temporal
> identity invariant now covers miss pixels too. Spatial reuse still
> skips them (no cluster key) - visible as slightly coarser grain
> outside the box, not as bias.
>
> **Replay-stable light pick (RestirSampleLightIndex).** The ReSTIR
> light subpath's emission pick switched from the camera-anchored
> light-tree descent to a binary search of the power CDF (binding 10)
> - one uniform draw, camera- and frame-INDEPENDENT, in restir_light
> + both replay sites (restir_caustic.glsl, shiftstep t=1 round 0).
> Under camera motion a replayed caustic/airlight path is now an
> EXACT world-space identity (all replayed pdfs camera-independent →
> J = 1, only the deterministic camera connection re-projects); the
> old tree replay could descend to a different light and turn history
> into an unrelated path (J-guard rejection or arbitrary ratios =
> churn where fog is brightest). Side benefit: the sampled pick pdf
> now EQUALS the RestirLightPdfs eval pdf, removing the documented
> sampled-vs-eval weight suboptimality. Camera-side NEE keeps the
> tree (its light points are stored, never re-picked). Costs light-
> selection importance (power-only vs camera-anchored) - a wash on
> this scene; revisit for many-light scenes.
>
> Soaks after both: fog-off 0.116480-0.116488 (in band), interior RIS
> 0.09155 / temporal 0.09156 (= RIS) / default 0.09159 (+0.05%,
> slightly better than 10.1's +0.07%), exterior accumulated 0.06574,
> moving duck hot=0 with improved tails. Remaining motion noise near
> lights is standard temporal-reuse churn (disocclusion bands +
> re-binning coverage), bounded by the confidence cap - the M-cap
> slider is the user-facing dial (higher = smoother fog under motion,
> longer-lived correlation).

### Phase 10.3 — Fog-parallax temporal pairing + paper rewrite

> **Status: DONE.** User observation: under camera TRANSLATION, fog
> history dies wherever the surface-anchor validation fails
> (disocclusion bands, silhouettes) - the airlight content in front of
> the surface is continuous there and reusable in principle.
>
> **Fog-parallax pairing (`restirFogPairing`, ROYALGL_RESTIR_FOGPAIR,
> default on).** Two constraints shape the design. (1) The history
> PAIRING must be SAMPLE-INDEPENDENT (a pairing chosen from realized
> reservoir content breaks E[sum of MIS weights] = 1 - same rule as
> proxy confidence). On surface-validation FAILURE only, tinit re-pairs
> by reprojecting the mean truncated free-flight depth
> E[t] = 1/sigma_t - d1*Tr(d1)/(1-Tr(d1)) (FogRepDistance,
> common.glsl - closed-form G-buffer function; -> 1/sigma_t for miss).
> (2) The fog pairing reuses the AIRLIGHT FAMILY ONLY, symmetrically in
> both merge directions (WfShiftCreateJob gained a volOnly flag; t>=2 &&
> volMask bit0): airlight shifts re-anchor to the destination ray and
> never touch the surface, while letting surface classes shift across
> disagreeing anchors is exactly what validation prevents. Static
> cameras never trigger the fallback - all identity invariants and
> bands bit-unchanged. New ROYALGL_DOLLY=<u/s> scripted lateral truck
> (rocking, 2s flips) provides the PARALLAX repro that ROYALGL_ORBIT
> (pure rotation) cannot. Measured: image-wide tail percentiles improve
> 5-15% under fast trucking; the benefit is local to the disocclusion
> bands (band-local metrics = future harness work).
>
> **Negative result (kept in the paper, Sec. 4.4):** ALSO inheriting
> the caustic reservoir's PROXY CONFIDENCE across the fog pairing
> drains energy progressively under sustained trucking (image mean
> 0.10 -> 0.05 over ~20 s, monotone): inflated cV discounts the
> canonical ~cap-fold through the backward MIS term while the re-binned
> landing supply in the sweeping band cannot compensate, and the
> shortfall compounds along the history chain. REVERTED - the caustic
> proxy stays surface-validated (miss-to-miss fog projection for miss
> pixels only). Lesson: re-pairing the REUSE (both MIS directions
> evaluated) is safe; inheriting a bare confidence scalar asserts reuse
> that never happens.
>
> **Paper rewritten** (docs/volumetric_restir_paper.pdf, 6 pages):
> explicit numbered contributions with provenance (new vs adapted),
> a related-work comparison table and prose positioning against
> Volumetric ReSTIR (Lin 2021, froxel RIS, no path-space shifts) and
> ReSTIR SSS (Werner 2024, HPG - the closest prior art: unidirectional
> shift-mapped SSS walks anchored at object surfaces with
> reconnection/delayed-reconnection selection heuristics [+ Guo 2025]),
> the airlight second-moment analysis, Sec. 4.4 fog pairing incl. the
> confidence counterexample, and the honest cost/limitation notes.

### Phase 10.4 — Convergence plots + paper v3 (snapshot rewrite)

> **Status: DONE.** Measurement tooling: `ROYALGL_EXPORT_SERIES=<prefix>`
> + `ROYALGL_EXPORT_FRAMES/STRIDE` dump the averaged accum buffer as raw
> RGBA32F per sample count (8-bit PNGs would quantize the variance
> floors away); stats lines carry wall-clock stamps (`StatsTime`);
> `ROYALGL_DOLLY` from 10.3. Plot suite (docs/make_plots.py -> repo-root
> plot_convergence/equal_time/ceiling.png + column variants for the
> paper): EXTERIOR default camera (fog-dominated view incl. miss
> pixels), static scene, relMSE vs a 4096-spp BDPT truth. Four
> estimators: BDPT accumulated (24 ms/spp), unidirectional PT
> accumulated (per-frame RIS, no reuse - the non-BDPT cross-reference,
> 30 ms), ReSTIR PT unidirectional per-frame (light+conn OFF, 44 ms),
> full ReSTIR BDPT per-frame (50 ms). Results: frame 1 relMSE 1.12
> (ours) vs 11.9 (ReSTIR PT); frame 8: 0.21 vs 1.66 (7.9x); per-frame
> floors 0.140 vs 0.847 (6.1x - the bidirectionality dividend in
> media); accumulated PT needs 112 frames to reach our per-frame floor,
> accumulated BDPT crosses it at frame 16 (static-scene accumulation
> eventually wins, as expected). Diagram figures for the paper
> (docs/make_diagrams.py): replay+mask shift, volume-anchor
> reconnection, airlight family split.
>
> **Paper v3** (docs/make_paper.py, 7 pages): snapshot style per review
> feedback - no project history or negative-result narration, no
> em-dashes, professional register; explanatory diagrams (Figs. 2-4) +
> two pseudocode listings that diff the volumetric additions against
> the GRIS procedures; displayed equations with symbol lists (free
> flight/truncated densities, volume-measure Jacobian derivation,
> airlight second-moment divergence, the t=1 MIS weight); ONE numeric
> table (unbiasedness) + the qualitative design-space comparison
> (Lin 2021 / Werner 2024 / ours); teaser + all plots on the exterior
> view; the naive mode-0 curve dropped from all figures.

---

## 7. Files touched / created

| File | Change |
|---|---|
| `shaders/restir_common.glsl` | **new** — reservoir structs, RIS/merge helpers, pairwise MIS |
| `shaders/restir_shift.glsl` | **new** — ShiftPath (Algorithm 3), Jacobians (App. B), recursive reconnection MIS (Sec. 6) |
| `shaders/restir_light.comp` | **new** — Algorithm 1 (light subpaths, LVC append, t=1 candidates, LRM stage) |
| `shaders/lrm_scan.comp`, `lrm_scatter.comp` | ~~new — LRM sort~~ superseded: Phase 2 shipped the linked-list LRM (`shaders/restir_lrm.glsl`, bindings 13/14); revisit the sort in Phase 6 if traversal coherence hurts |
| `shaders/restir_camera.comp` | **new** — Algorithm 2 (G-buffer, initial RIS, LRM merge) |
| `shaders/restir_temporal.comp`, `restir_caustic_temporal.comp`, `restir_spatial.comp`, `restir_resolve.comp` | **new** — reuse + resolve passes |
| `shaders/common.glsl` | counter-based RNG; material-only RR helper |
| `shaders/bdpt_common.glsl` | expose dP/dVC update as shared functions used by both classic BDPT and ReSTIR passes |
| `src/pathtracer/PathTracer.{h,cpp}` | ReSTIR frame path, new buffers/bindings 15–24, ping-pong, prev-camera tracking |
| `src/pathtracer/RenderSettings.h` | `enableRestir`, N_L, spatial params, confidence cap, MIS mode, debug view enum |
| `src/gfx/GPUTypes.h` | UBO additions (prev camera, restir params) |
| `src/core/Application.cpp` | don't reset reservoirs on camera move in ReSTIR mode; reset on scene/material edits |
| `src/ui/UILayer.cpp` | ReSTIR panel + debug views |

---

## 8. Risks & open questions

1. **UCW bookkeeping bugs** manifest as subtle brightness bias, not crashes. Mitigation:
   the Phase 1.1 "RIS-only, no reuse" checkpoint and soak tests (long average vs
   reference image, per-phase) are non-negotiable.
2. **Register pressure / divergence** in the unified shift kernel on GL compute
   (no Shader Execution Reordering here, unlike the paper's Falcor/OptiX context).
   The paper already flags this; splitting dispatches by technique class is the
   known fix. Budget for it in Phase 6.
3. **Prefix-sum LRM on GL** — a workgroup-hierarchical scan is standard but fiddly;
   linked-list fallback documented in Sec. 3.
4. **32-bit seed entropy** — one seed per subpath per frame is plenty (paper does the
   same via random replay), but seeds must be decorrelated across pixels/frames;
   reuse existing WangHash seeding discipline.
5. **Glass-only "roughness"** means every diffuse vertex is connectable — reconnection
   shifts will succeed more often than in the paper's scenes (good), but caustic
   classification is binary and aggressive (all glass-touching t≤1 paths are caustic).
   Watch the regular/caustic split ratio in debug views.
6. **Temporal accumulation semantics** — ReSTIR is per-frame; RoyalGL is progressive.
   Decide UX: default ReSTIR shows the live 1spp-reuse estimate (+optional OIDN);
   "converge" button switches to spatial-only accumulation (paper Fig. 12 mode).
