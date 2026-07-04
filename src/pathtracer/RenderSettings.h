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
