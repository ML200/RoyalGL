#pragma once

#include <glm/glm.hpp>
#include "optics/LensSystem.h"

namespace RoyalGL
{
    enum class CameraMode : int
    {
        Pinhole = 0,
        Lens = 1 // physical lens per Steinert et al. 2011
    };

    // User-tunable render settings, surfaced in the UI. Changing any of
    // these (compared via operator==) resets progressive accumulation.
    struct RenderSettings
    {
        int maxBounces = 6;
        float exposure = 1.0f;
        glm::vec3 backgroundColor{0.015f, 0.015f, 0.02f}; // much darker than the old sky-blue default, so small bright light sources actually pop
        float backgroundIntensity = 1.0f;
        int maxSamples = 0; // 0 = unlimited

        // ReSTIR BDPT (docs/RESTIR_BDPT_PLAN.md): per-frame spatiotemporal
        // reservoir reuse over bidirectional path candidates, full-frame
        // (non-tiled) wavefront dispatch. Pinhole cameras only - lens mode
        // falls back to plain BDPT (the stochastic pupil makes the primary
        // hit a random variable, which is Area-ReSTIR territory). Off =
        // plain progressive BDPT.
        bool enableRestir = true;
        // 0=off, then G-buffer normals / depth / motion vectors, reservoir
        // W / confidence / technique index (see shaders/restir_debug.comp).
        int restirDebugView = 0;
        // Reuse passes; disable both for pure per-pixel RIS (the Phase 1.1
        // unbiasedness baseline).
        bool restirTemporal = true;
        bool restirSpatial = true;
        // Spatial candidates per pixel and frame, drawn with the antithetic
        // stratified pattern from the block's sorted candidate list
        // (Salaün 2025; the paper uses 4). Even counts pair antithetically;
        // 2 = one antithetic pair {u, 1-u}, half the shift cost of 4.
        int restirSpatialNeighbors = 2;
        // A/B switch (env ROYALGL_RESTIR_STRAT=0, no UI): off = uniform
        // random picks from the same sorted/clustered pool - isolates the
        // benefit of the antithetic stratification itself.
        bool restirStratified = true;
        // Spatial candidate selection & MIS family (env ROYALGL_RESTIR_SPMODE):
        //  0 = pair mixture (default): each round pair-merges one
        //      rank-selected candidate against the frozen canonical and the
        //      rounds mix linearly (restir_wf_smerge.comp header).
        //  1 = SPMIS baseline (Hedstrom et al. 2026, "Stochastic Pairwise
        //      MIS"): candidates drawn with replacement from the cluster
        //      run proportionally to c_i*pHat(X_i)*W_i (source luminance),
        //      folded into ONE reservoir chain with stochastic pairwise MIS
        //      weights m~_i = 1/(N~ P(i)) * m_i; canonical MIS weight
        //      estimated with N~c=1; non-canonical confidences scaled N~/M.
        //  2 = history-guided SPMIS: the SPMIS weight additionally
        //      attenuated by a learned per-pixel shift-survival ratio
        //      (bounded EMA, reprojected with history). Measured a WASH
        //      on our scenes (kept as the paper's ablation arm - see
        //      docs/NEIGHBOR_SELECTION_PLAN.md "KILLED with full
        //      attribution").
        // All modes are unbiased; they differ in reuse efficiency.
        int restirSpatialMode = 0;
        // Mode 2 ablation (env ROYALGL_RESTIR_VSCORE, no UI): fold the
        // online-learned shift-survival score into the selection weights.
        // Off = mode 2 degenerates to exactly mode 1.
        bool restirShiftScore = true;
        // Iterated spatial reuse (env ROYALGL_RESTIR_SPATIAL_ITERS): run
        // the whole spatial pass family (sort + K candidate rounds) J
        // times per frame, feeding each pass the previous pass's outputs.
        // Reused candidates then carry AGGREGATES, so effective sample
        // counts compound multiplicatively (~(K+1)^J) instead of linearly
        // in K - the spatial-only substitute for temporal accumulation,
        // with all correlation resetting every frame. The marginalized
        // shading averages the passes' estimates (running mean).
        int restirSpatialIterations = 1;
        // SPMIS cell search (env ROYALGL_RESTIR_CELLSEARCH, modes >= 1):
        // per pixel, WRS over probed pixels' cluster runs (own and
        // neighboring blocks) weighted by run confidence mass, with
        // growing probe radius (Hedstrom et al. 2026 Sec. 5.1). Recovers
        // reuse partners when a disocclusion swallows the whole local
        // block. Off = reuse only from the pixel's own block run.
        bool restirCellSearch = true;
        // Cell-search shape (envs ROYALGL_RESTIR_SEARCH_ITERS/_RADIUS):
        // probe count and initial radius in pixels (growth fixed x1.25).
        // Paper Sec. 5.1 uses 12/30; the older tune was 8/24.
        int restirSearchIters = 12;
        int restirSearchRadius = 30;
        // Starvation gate (env ROYALGL_RESTIR_SEARCH_GATE): 0 = the
        // paper's unconditional probe, 1 = probe only near-empty runs
        // (count <= 2), 2 = additionally probe pixels whose temporal
        // history vanished (tinit disocclusion flag), 3 = probe runs with
        // ZERO selection mass, probes weighted by their selection mass -
        // BIASED (+12-16% bright on sparse scenes, ablation only): the
        // value-dependent trigger fires exactly where the estimator's
        // accounting needs a zero, see restir_wf_sinit.comp.
        // Measured (dolly + orbit, deterministic fixed-dt, 2026-07-04):
        // unconditional probing costs 23-45% whole-image noise under
        // motion (the WRS keeps the own run with probability
        // ~1/(1+iters), so probing replaces close partners with distant
        // ones), and even wide fresh disocclusion strips reuse better
        // among their own run's same-frame candidates than from distant
        // converged runs (gate 2 masked 0.328 vs 0.298 no-search). Gate 1
        // = no measurable cost vs no-search anywhere, keeps rescue for
        // partnerless pixels.
        int restirSearchGate = 1;
        // Quantized-normal cluster term (env ROYALGL_RESTIR_CLUSTER_NORMAL):
        // key cluster runs on the world-normal octant in addition to
        // instance+material (paper Sec. 5.1 hashes objectID + quantized
        // normal). Splits curved geometry by orientation in every spatial
        // mode's clustering; off = the pre-2026-07-04 instance+material key.
        bool restirClusterNormal = true;
        // Dual-direction shift-asymmetry diagnostic (env
        // ROYALGL_RESTIR_SHIFTDIAG, 0 = off): for the same (canonical, z')
        // pair, run BOTH shift directions each frame (mode 1, temporal
        // off, K >= 5 so round 1 exists) and accumulate per-pixel tallies
        // in the learn region. Sub-mode semantics (exact formulas in
        // restir_wf_smerge.comp): 1 = paired support-asymmetry counts,
        // 2 = valid/both-ok context counts, 3 = sum log(rF/rB), 4 = sum
        // rB / sum rF, 5 = the m~_c beta channel evaluated through both
        // directions (the direct bias channel). Chasing the SPMIS-chain
        // residual's forward/backward shift asymmetry.
        int restirShiftDiag = 0;
        // Diagnostic reuse distance (env ROYALGL_RESTIR_SHIFTDIAG_DIST,
        // pixels; 0 = own-run pairs): overrides the reuse run with an
        // INVOLUTIVE fixed-stride partner (x-cells pair mutually at
        // +-stride), so the distant-pair ensemble stays exchangeable and
        // direction comparisons remain clean at any distance - unlike the
        // cell search, whose chosen runs are role-asymmetric. Input-set
        // selection, so the chain stays unbiased; the run's soak mean
        // under this pairing is itself a distance dose-response probe.
        int restirShiftDiagDist = 0;
        // Probe-guided union selection (env ROYALGL_RESTIR_PSEL, mode 1):
        // once per frame each 16x16 block probes SearchIters source runs
        // (value-independent positions) and runs ONE representative shift
        // per probed run into the block center; the measured ideal weight
        // w^_r = S_r * survival * MIS-factor guides a two-level draw over
        // the union of own + probed runs with exact per-draw P. The
        // sparse-carrier rescue (LightMaze class) with dense-scene safety
        // via the defensive DEFMIX share of plain luminance selection.
        bool restirProbeSelection = false;
        // Mode-2 history-guided selection knobs (envs ROYALGL_RESTIR_EMA /
        // ROYALGL_RESTIR_DEFMIX): score EMA rate for the realized-shift
        // observations, and the defensive source-luminance fraction of the
        // selection weight (1 - it goes to the learned score).
        float restirScoreEmaRate = 0.125f;
        float restirScoreDefMix = 0.25f;
        // Duplication-map temporal decorrelation (ReSTIR PT Enhanced
        // sec. 5): end-of-frame same-seed counting adaptively lowers the
        // temporal confidence cap in correlated regions. Trades a small,
        // bounded bias (paper: ~3% in pathological scenes, far less
        // typically) for much faster firefly/blob decay. Off = exactly
        // unbiased (soak mode). Env: ROYALGL_RESTIR_DECORR.
        bool restirDecorrelate = true;
        // Fog-parallax temporal pairing (env ROYALGL_RESTIR_FOGPAIR): when
        // the surface-anchor history validation fails under camera motion,
        // re-pair with the pixel that saw the same FOG (representative
        // in-scatter depth reprojection, sample-independent) and reuse the
        // airlight family only. Off = ablation baseline (fog history dies
        // with the surface anchor).
        bool restirFogPairing = true;
        // Volumetric shift mode (env ROYALGL_RESTIR_VOLMODE):
        //  0 = naive ReSTIR PT port: paths containing volume vertices
        //      cannot shift at all (single-frame candidates),
        //  1 = replay-extended: distance replay + scatter-classification
        //      masks make volume paths shiftable by random replay,
        //  2 = volume-anchored (default, ours): additionally, volume
        //      vertices qualify as reconnection anchors (volume-measure
        //      Jacobians, phase-footprint criteria).
        // All modes are unbiased; they differ in reuse efficiency.
        int restirVolumeMode = 2;
        // Confidence (M) cap for all reuse passes (paper uses 20): bounds
        // the effective temporal sample count a reservoir can claim.
        // Lower = fresher (outliers and stale history wash out in fewer
        // frames), higher = smoother but longer-lived correlation. Default
        // 8: t=1 reuse between spatially distant anchors is heavy-tailed
        // at grazing incidence, and shorter chains recover faster.
        float restirConfidenceCap = 8.0f;
        // Phase 2 "lightweight BDPT": t=1 light tracing candidates binned
        // into per-pixel reservoirs through the LRM, with caustic paths in
        // a second reservoir. Off = Phase 1 camera-side techniques only.
        bool restirLightTracing = true;
        // Caustic temporal reuse (Phase 3 shift + merge passes). Off = the
        // caustic reservoir holds only the per-frame canonical RIS result -
        // the isolation switch for chasing temporal transients to the
        // caustic vs the path reservoir. Env: ROYALGL_RESTIR_CAUSTIC.
        bool restirCausticReuse = true;
        // Light subpaths traced per frame (N_L, paper Alg. 1). Sizes the
        // LVC / light-vertex-count / LRM buffers (PathTracer reallocates on
        // change) and enters the t<=1 MIS weights as 1/N_L. Clamped to
        // [4096, min(pixelCount, 262144)]. Lower = cheaper light pass but
        // sparser t=1/LRM coverage per pixel. Env: ROYALGL_RESTIR_NL.
        int restirLightPaths = 262144;
        // Phase 4 full BDPT: s>=2 vertex connections against the compacted
        // global LVC. Off = lightweight mode; the MIS weights track the
        // active technique set either way.
        bool restirConnections = true;
        // Phase 5: recompute omega_tau at the destination anchor for every
        // shift (camera-side, t=1 reverse, caustic replay); off = copy
        // omega through shifts (paper Sec. 6.4). Default ON: with light
        // tracing enabled, every technique's omega carries the t=1
        // competitor seed N_L*d^2/cosIn1, which varies violently with the
        // anchor at grazing incidence - a stale copied omega under camera
        // motion breaks the temporal balance-heuristic partition by orders
        // of magnitude and W blows up on newly-visible sharp-angle
        // surfaces for ~confidence-cap frames. (The historical reason to
        // keep this off - the free-landing t=1 re-anchoring approximation
        // that recompute amplified - is gone since t=1 candidates are
        // anchored at creation.) See RESTIR_BDPT_PLAN.md.
        bool restirRecomputeMis = true;

        // Homogeneous global medium ("fog"): analytic Beer-Lambert model
        // with a Henyey-Greenstein phase function - exact for a homogeneous
        // medium. Coefficients are per world unit; the Cornell box is ~2
        // units across, so sigma ~0.1-0.3 gives thin-to-moderate fog.
        bool fogEnable = false;
        float fogSigmaS = 0.15f; // scattering coefficient
        float fogSigmaA = 0.02f; // absorption coefficient
        float fogG = 0.0f;       // HG anisotropy (-1..1, 0 = isotropic)

        // Global: off = every pipeline overwrites the image with its latest
        // sample instead of averaging, so naive PT / NEE / BDPT / ReSTIR
        // per-frame quality can be compared live.
        bool accumulate = true;

        // Physical lens camera (Steinert et al. 2011). Lens mode renders
        // through the plain BDPT pipeline (ReSTIR is pinhole-only).
        CameraMode cameraMode = CameraMode::Pinhole;
        LensSettings lens;

        bool operator==(const RenderSettings& other) const
        {
            return maxBounces == other.maxBounces &&
                   exposure == other.exposure &&
                   backgroundColor == other.backgroundColor &&
                   backgroundIntensity == other.backgroundIntensity &&
                   maxSamples == other.maxSamples &&
                   enableRestir == other.enableRestir &&
                   restirDebugView == other.restirDebugView &&
                   restirTemporal == other.restirTemporal &&
                   restirSpatial == other.restirSpatial &&
                   restirSpatialNeighbors == other.restirSpatialNeighbors &&
                   restirStratified == other.restirStratified &&
                   restirSpatialMode == other.restirSpatialMode &&
                   restirShiftScore == other.restirShiftScore &&
                   restirCellSearch == other.restirCellSearch &&
                   restirSearchIters == other.restirSearchIters &&
                   restirSearchRadius == other.restirSearchRadius &&
                   restirSearchGate == other.restirSearchGate &&
                   restirClusterNormal == other.restirClusterNormal &&
                   restirShiftDiag == other.restirShiftDiag &&
                   restirShiftDiagDist == other.restirShiftDiagDist &&
                   restirScoreEmaRate == other.restirScoreEmaRate &&
                   restirScoreDefMix == other.restirScoreDefMix &&
                   restirProbeSelection == other.restirProbeSelection &&
                   restirSpatialIterations == other.restirSpatialIterations &&
                   restirDecorrelate == other.restirDecorrelate &&
                   restirVolumeMode == other.restirVolumeMode &&
                   restirFogPairing == other.restirFogPairing &&
                   restirConfidenceCap == other.restirConfidenceCap &&
                   restirLightTracing == other.restirLightTracing &&
                   restirCausticReuse == other.restirCausticReuse &&
                   restirLightPaths == other.restirLightPaths &&
                   restirConnections == other.restirConnections &&
                   restirRecomputeMis == other.restirRecomputeMis &&
                   fogEnable == other.fogEnable &&
                   fogSigmaS == other.fogSigmaS &&
                   fogSigmaA == other.fogSigmaA &&
                   fogG == other.fogG &&
                   accumulate == other.accumulate &&
                   cameraMode == other.cameraMode &&
                   lens == other.lens;
        }
        bool operator!=(const RenderSettings& other) const { return !(*this == other); }
    };
}
