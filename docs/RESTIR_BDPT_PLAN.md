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
> size tuning (8x8 kept; retest once register counts settle), the N_L and
> resolution-scale sliders, a Vulkan/RT-core backend as the ultimate
> answer to the software-traversal cost, and the "spatial-only offline
> converge" preset (already reachable via the existing temporal/spatial
> toggles — temporal correlation shows up in soaks as slower
> accumulated-relNoise decay).

- Profile with existing `ROYALGL_STATS=1` GL timer queries per pass.
- Reservoir packing (fp16 normals/throughputs where safe), register pressure in the
  shift kernel (consider splitting caustic vs non-caustic shifts into separate
  dispatches — the paper lists this as a known win).
- Progressive accumulation when camera static (ReSTIR frame estimates averaged after
  reservoirs warm up; note temporal correlation — offer spatial-only offline mode
  like the paper's Fig. 12).
- Optional: lens-mode ReSTIR investigation (Area ReSTIR), GGX materials (real
  roughness threshold 0.08 semantics kick in here).

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
